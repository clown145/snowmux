const SECTOR = 0x1000;
const PARTITIONS = 0x8000;
const FIRST_SELECTOR = 0xf000;
const SECOND_SELECTOR = 0x10000;
const OTA0 = 0x20000;
const OTA1 = 0x750000;
const SELECTOR_ENTRY_SIZE = 32;
const OTA_STATE_VALID = 2;
const OTA_STATE_INVALID = 3;
const OTA_STATE_ABORTED = 4;
const OTA_STATE_UNDEFINED = 0xffffffff;
const WEB_SELECTOR_LABEL = new TextEncoder().encode('SnowMux');
const SLOT_SIZE = 0x730000;
const PARTITION_SHA256 = '6bc034d19c972844b71b0bc45ef77401b018cc2ec08cc7440193c87fd11149ab';
const STOCK_APPS = [
  { bytes: 2_915_888, md5: 'd5c277975ddeec6428857a398750a4df' },
  { bytes: 2_897_184, md5: '882c078a04fc19a13d38a39be08e8eae' },
];

export const FLASH_SIZE = 16 * 1024 * 1024;
export const RELEASE_MANIFEST = './api/v1/firmware/latest.json';

export async function sha256(bytes) {
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  return Array.from(new Uint8Array(digest), (byte) => byte.toString(16).padStart(2, '0')).join('');
}

export function validateRelease(release) {
  const asset = release?.assets?.[0];
  if (
    release?.schema !== 1 ||
    release.board !== 'xuesu-300-std' ||
    release.target_partition !== 'ota_1' ||
    !/^\d+\.\d+\.\d+$/.test(release.tag_name) ||
    release.release_tag !== `xuesu-v${release.tag_name}` ||
    !/^[a-f0-9]{40}$/.test(release.commit) ||
    !/^[\w.-]+\/[\w.-]+$/.test(release.repository) ||
    !Number.isFinite(Date.parse(release.published_at)) ||
    !Array.isArray(release.assets) ||
    release.assets.length !== 1 ||
    asset?.name !== 'firmware.bin' ||
    !/^sha256:[a-f0-9]{64}$/.test(asset.digest) ||
    !/^[a-f0-9]{32}$/.test(asset.md5) ||
    !Number.isSafeInteger(asset.size) ||
    asset.size < 64 ||
    asset.size > SLOT_SIZE ||
    asset.size % 16 !== 0
  ) {
    throw new Error('固件发布信息无效。');
  }
  return {
    version: release.tag_name,
    releaseTag: release.release_tag,
    publishedAt: release.published_at,
    repository: release.repository,
    bytes: asset.size,
    sha256: asset.digest.slice('sha256:'.length),
    md5: asset.md5,
  };
}

export function firmwarePath(release) {
  return `./firmware/${release.sha256}.bin`;
}

export async function validateFirmware(bytes, release) {
  const chip = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (
    bytes.length !== release.bytes ||
    bytes[0] !== 0xe9 ||
    chip.getUint16(12, true) !== 9 ||
    (await sha256(bytes)) !== release.sha256
  ) {
    throw new Error('固件大小、芯片型号或 SHA-256 校验失败。');
  }
}

function otaSequenceCrc(sequence) {
  // esp_rom_crc32_le(UINT32_MAX, &ota_seq, 4) starts with the inverted seed.
  let crc = 0;
  for (let byte = 0; byte < 4; byte++) {
    crc ^= (sequence >>> (8 * byte)) & 0xff;
    for (let bit = 0; bit < 8; bit++) {
      crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
    }
  }
  return ~crc >>> 0;
}

function makeOta1Selector(sequence) {
  const sector = new Uint8Array(SECTOR).fill(0xff);
  const entry = new DataView(sector.buffer);
  entry.setUint32(0, sequence, true);
  sector.set(WEB_SELECTOR_LABEL, 4); // The bootloader ignores seq_label.
  entry.setUint32(28, otaSequenceCrc(sequence), true);
  return sector;
}

function equals(bytes, other) {
  return bytes.length === other.length && bytes.every((byte, index) => byte === other[index]);
}

function readSelector(sector, address) {
  if (sector.every((byte) => byte === 0xff)) return null;
  if (address === FIRST_SELECTOR && sector.every((byte, index) => byte === (index < 4 ? 0 : 0xff))) {
    return null; // The original reference unit's uninitialized marker.
  }

  const entry = new DataView(sector.buffer, sector.byteOffset, SELECTOR_ENTRY_SIZE);
  const sequence = entry.getUint32(0, true);
  const state = entry.getUint32(24, true);
  if (
    sequence === 0 || sequence === 0xffffffff ||
    (state > OTA_STATE_ABORTED && state !== OTA_STATE_UNDEFINED) ||
    entry.getUint32(28, true) !== otaSequenceCrc(sequence) ||
    !sector.subarray(SELECTOR_ENTRY_SIZE).every((byte) => byte === 0xff)
  ) {
    throw new Error('启动记录格式或校验异常，已停止刷机。');
  }
  if (state === OTA_STATE_INVALID || state === OTA_STATE_ABORTED) return null;

  const label = sector.subarray(4, 24);
  const webLabel = equals(label.subarray(0, WEB_SELECTOR_LABEL.length), WEB_SELECTOR_LABEL) &&
    label.subarray(WEB_SELECTOR_LABEL.length).every((byte) => byte === 0xff);
  const legacyWebSelector = address === SECOND_SELECTOR && sequence === 2 && state === OTA_STATE_UNDEFINED &&
    label.every((byte) => byte === 0xff);
  return { address, sequence, state, webSelector: webLabel || legacyWebSelector };
}

export function planBootSelection(selectors) {
  if (selectors.length !== 2 * SECTOR) throw new Error('启动记录读取不完整。');
  const first = readSelector(selectors.subarray(0, SECTOR), FIRST_SELECTOR);
  const second = readSelector(selectors.subarray(SECTOR), SECOND_SELECTOR);
  const active = second && (!first || second.sequence > first.sequence) ? second : first;
  if (active && active.state < OTA_STATE_VALID) {
    throw new Error('当前固件的启动记录尚未确认，请先启动当前固件再重试。');
  }
  // With two app slots, odd sequences select OTA0 and even sequences select OTA1.
  if (active && active.sequence % 2 === 0) {
    if (!active.webSelector) throw new Error('当前从 OTA1 启动，网页无法确认它是 SnowMux。');
    return { selectors, mode: 'update' };
  }

  return {
    selectors,
    mode: 'install',
    activate: {
      address: active?.address === SECOND_SELECTOR ? FIRST_SELECTOR : SECOND_SELECTOR,
      sequence: active ? active.sequence + 1 : 2,
    },
  };
}

function readSelectors(loader) {
  return loader.readFlash(FIRST_SELECTOR, SECTOR * 2);
}

export async function inspectDevice(loader) {
  const security = await loader.getSecurityInfo();
  if (
    security.chipId !== 9 ||
    security.parsedFlags.SECURE_BOOT_EN ||
    security.parsedFlags.SECURE_DOWNLOAD_ENABLE ||
    (security.flashCryptCnt & 0b111) !== 0
  ) {
    throw new Error('芯片型号或安全配置与雪酥不符。');
  }
  if ((await loader.detectFlashSize()) !== '16MB') throw new Error('设备的 Flash 容量不是 16 MiB。');
  const partitionTable = await loader.readFlash(PARTITIONS, 0xc00);
  if ((await sha256(partitionTable)) !== PARTITION_SHA256)
    throw new Error('设备的分区表与雪酥不符。');
  for (const stock of STOCK_APPS) {
    if ((await loader.flashMd5sum(OTA0, stock.bytes)) === stock.md5) {
      return planBootSelection(await readSelectors(loader));
    }
  }
  throw new Error('OTA0 未找到受支持的原厂固件。');
}

export async function backupFlash(loader, report) {
  const result = new Uint8Array(FLASH_SIZE);
  // Keep esptool-js's accumulating read buffer bounded to one chunk.
  const chunkSize = 256 * 1024;
  for (let offset = 0; offset < result.length; offset += chunkSize) {
    const chunk = await loader.readFlash(offset, chunkSize, (_, received) =>
      report(offset + received, result.length),
    );
    if (chunk.length !== chunkSize) throw new Error('Flash 备份读取不完整。');
    result.set(chunk, offset);
  }
  return result;
}

async function write(loader, address, data, reportProgress) {
  await loader.writeFlash({
    fileArray: [{ address, data }],
    flashMode: 'keep',
    flashFreq: 'keep',
    flashSize: 'keep',
    eraseAll: false,
    compress: true,
    reportProgress,
  });
}

export async function installFirmware(loader, bytes, release, bootPlan, report) {
  await validateFirmware(bytes, release);
  if (!equals(await readSelectors(loader), bootPlan.selectors)) {
    throw new Error('设备启动记录已变化，请重新连接。');
  }
  report('写入 OTA1', 0);
  await write(loader, OTA1, bytes, (_, written, total) => report('写入 OTA1', (90 * written) / total));
  report('校验 OTA1', 92);
  if ((await loader.flashMd5sum(OTA1, bytes.length)) !== release.md5)
    throw new Error('OTA1 写入校验失败。');
  if (bootPlan.activate) {
    const { address, sequence } = bootPlan.activate;
    const expected = makeOta1Selector(sequence);
    report('设置启动槽', 96);
    await write(loader, address, expected.slice());
    const selector = await loader.readFlash(address, SECTOR);
    if (!equals(selector, expected)) throw new Error('启动记录读回校验失败。');
  }
  report('写入校验完成', 100);
}

export async function resetToFirmware(loader) {
  // Mirror ESP32-S3 esptool's USB Serial/JTAG hard reset: clear
  // RTC_CNTL_FORCE_DOWNLOAD_BOOT in RTC_CNTL_OPTION1_REG, then pulse RTS.
  // esptool-js 0.7.1 HardReset only releases RTS, which is already idle.
  await loader.writeReg(0x6000812c, 0, 1);
  await loader.transport.setRTS(true);
  await new Promise((resolve) => setTimeout(resolve, 100));
  await loader.transport.setRTS(false);
}
