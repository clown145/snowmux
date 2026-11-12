const SECTOR = 0x1000;
const PARTITIONS = 0x8000;
const FIRST_SELECTOR = 0xf000;
const SECOND_SELECTOR = 0x10000;
const OTA0 = 0x20000;
const OTA1 = 0x750000;
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

function makeOta1Selector() {
  const sector = new Uint8Array(SECTOR).fill(0xff);
  const entry = new DataView(sector.buffer);
  // The reference bootloader accepts the same sequence-2 entry used in hardware trials.
  entry.setUint32(0, 2, true);
  entry.setUint32(28, 0x55f63774, true); // ESP-IDF CRC32 over the little-endian sequence.
  return sector;
}

const OTA1_SELECTOR = makeOta1Selector();

function equals(bytes, other) {
  return bytes.length === other.length && bytes.every((byte, index) => byte === other[index]);
}

export function selectorMode(first, second) {
  const stockFirst =
    first.length === SECTOR && first.every((byte, index) => byte === (index < 4 ? 0 : 0xff));
  const erased = (bytes) => bytes.length === SECTOR && bytes.every((byte) => byte === 0xff);
  if (!stockFirst && !erased(first)) throw new Error('启动记录与已验证布局不同。');
  if (erased(second)) return 'install';
  if (equals(second, OTA1_SELECTOR)) return 'update';
  throw new Error('OTA1 启动记录与已验证布局不同。');
}

async function readSelectors(loader) {
  const bytes = await loader.readFlash(FIRST_SELECTOR, SECTOR * 2);
  return selectorMode(bytes.subarray(0, SECTOR), bytes.subarray(SECTOR));
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
    if ((await loader.flashMd5sum(OTA0, stock.bytes)) === stock.md5) return readSelectors(loader);
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

export async function installFirmware(loader, bytes, release, expectedMode, report) {
  await validateFirmware(bytes, release);
  if ((await readSelectors(loader)) !== expectedMode)
    throw new Error('设备启动记录已变化，请重新连接。');
  report('写入 OTA1', 0);
  await write(loader, OTA1, bytes, (_, written, total) => report('写入 OTA1', (90 * written) / total));
  report('校验 OTA1', 92);
  if ((await loader.flashMd5sum(OTA1, bytes.length)) !== release.md5)
    throw new Error('OTA1 写入校验失败。');
  if (expectedMode === 'install') {
    report('设置启动槽', 96);
    await write(loader, SECOND_SELECTOR, OTA1_SELECTOR.slice());
    const selector = await loader.readFlash(SECOND_SELECTOR, SECTOR);
    if (!equals(selector, OTA1_SELECTOR)) throw new Error('启动记录读回校验失败。');
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
