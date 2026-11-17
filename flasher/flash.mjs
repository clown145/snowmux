const SECTOR = 0x1000;
const PARTITIONS = 0x8000;
const FIRST_SELECTOR = 0xf000;
const SECOND_SELECTOR = 0x10000;
const APP_SLOTS = [0x20000, 0x750000];
const SELECTOR_ENTRY_SIZE = 32;
const OTA_STATE_VALID = 2;
const OTA_STATE_INVALID = 3;
const OTA_STATE_ABORTED = 4;
const OTA_STATE_UNDEFINED = 0xffffffff;
const WEB_SELECTOR_LABEL = new TextEncoder().encode('SnowMux');
const SLOT_SIZE = 0x730000;
const FLASH_READ_CHUNK = 256 * 1024;
const PARTITION_SHA256 = '6bc034d19c972844b71b0bc45ef77401b018cc2ec08cc7440193c87fd11149ab';
const FIRMWARE_MIRRORS = [
  'https://gh.dpik.top',
  'https://gh-proxy.com',
  'https://hk.gh-proxy.org',
  'https://edgeone.gh-proxy.org',
];
const MIRROR_SAMPLE_BYTES = 64 * 1024;
const MIRROR_PROBE_TIMEOUT_MS = 4_000;
const MIRROR_DOWNLOAD_TIMEOUT_MS = 20_000;

export const FLASH_SIZE = 16 * 1024 * 1024;
export const RELEASE_MANIFEST = './api/v1/firmware/latest.json';

export class UnconfirmedBootError extends Error {
  constructor() {
    super('当前固件的启动记录尚未确认。');
  }
}

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

export function firmwareReleaseUrl(release) {
  return `https://github.com/${release.repository}/releases/download/${release.releaseTag}/${release.releaseTag}-ota1.bin`;
}

function hasEsp32S3Header(bytes) {
  return bytes.length >= 14 && bytes[0] === 0xe9 && bytes[12] === 9 && bytes[13] === 0;
}

async function probeMirror(url, sampleSize, request) {
  const started = performance.now();
  let reader;
  try {
    const response = await request(url, {
      headers: { Range: `bytes=0-${sampleSize - 1}` },
      cache: 'no-store',
      signal: AbortSignal.timeout(MIRROR_PROBE_TIMEOUT_MS),
    });
    reader = response.body?.getReader();
    if (!response.ok || !reader) return null;

    const sample = new Uint8Array(sampleSize);
    let received = 0;
    while (received < sampleSize) {
      const { done, value } = await reader.read();
      if (done) return null;
      const chunk = value.subarray(0, sampleSize - received);
      sample.set(chunk, received);
      received += chunk.length;
    }
    return hasEsp32S3Header(sample) ? { url, elapsed: performance.now() - started } : null;
  } catch {
    return null;
  } finally {
    // Some mirrors ignore Range and respond with the entire image.
    void reader?.cancel().catch(() => {});
  }
}

export async function downloadFirmware(release, { request = fetch, onSource = () => {} } = {}) {
  const download = async (url, options) => {
    const response = await request(url, options);
    if (!response.ok) {
      void response.body?.cancel().catch(() => {});
      throw new Error(`固件下载失败（HTTP ${response.status}）。`);
    }
    const bytes = new Uint8Array(await response.arrayBuffer());
    await validateFirmware(bytes, release);
    return bytes;
  };

  const releaseUrl = firmwareReleaseUrl(release);
  const sampleSize = Math.min(release.bytes, MIRROR_SAMPLE_BYTES);
  const probes = await Promise.all(
    FIRMWARE_MIRRORS.map((mirror) => probeMirror(`${mirror}/${releaseUrl}`, sampleSize, request)),
  );
  const ranked = probes.filter(Boolean).sort((first, second) => first.elapsed - second.elapsed);
  for (const { url } of ranked) {
    onSource(new URL(url).hostname);
    try {
      return await download(url, { signal: AbortSignal.timeout(MIRROR_DOWNLOAD_TIMEOUT_MS) });
    } catch {
      // A full transfer can fail even after a successful probe.
    }
  }
  onSource('GitHub Pages');
  return download(firmwarePath(release));
}

export async function validateFirmware(bytes, release) {
  if (
    bytes.length !== release.bytes ||
    !hasEsp32S3Header(bytes) ||
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

function makeSelector(sequence) {
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

  return { address, sequence, state };
}

function planBootSelection(selectors) {
  if (selectors.length !== 2 * SECTOR) throw new Error('启动记录读取不完整。');
  const first = readSelector(selectors.subarray(0, SECTOR), FIRST_SELECTOR);
  const second = readSelector(selectors.subarray(SECTOR), SECOND_SELECTOR);
  const active = second && (!first || second.sequence > first.sequence) ? second : first;
  if (active && active.state < OTA_STATE_VALID) {
    throw new UnconfirmedBootError();
  }
  // With two app slots, odd sequences select OTA0 and even sequences select OTA1.
  const currentSlot = active ? (active.sequence - 1) % APP_SLOTS.length : 0;
  const targetSlot = 1 - currentSlot;
  const sequence = active ? active.sequence + 1 : 2;
  if (sequence === 0xffffffff) throw new Error('启动记录序号已用尽。');
  return {
    selectors,
    target: { address: APP_SLOTS[targetSlot], label: `OTA${targetSlot}` },
    activate: {
      address: active?.address === SECOND_SELECTOR ? FIRST_SELECTOR : SECOND_SELECTOR,
      sequence,
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
  return planBootSelection(await readSelectors(loader));
}

export async function backupFlash(loader, report) {
  const result = new Uint8Array(FLASH_SIZE);
  // Keep esptool-js's accumulating read buffer bounded to one chunk.
  const chunkSize = FLASH_READ_CHUNK;
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
  const { address: targetAddress, label } = bootPlan.target;
  const writing = `写入 ${label}`;
  report(writing, 0);
  await write(loader, targetAddress, bytes, (_, written, total) => report(writing, (90 * written) / total));
  report(`校验 ${label}`, 92);
  if ((await loader.flashMd5sum(targetAddress, bytes.length)) !== release.md5)
    throw new Error(`${label} 写入校验失败。`);
  const { address, sequence } = bootPlan.activate;
  const expected = makeSelector(sequence);
  report('设置启动槽', 96);
  await write(loader, address, expected.slice());
  const selector = await loader.readFlash(address, SECTOR);
  if (!equals(selector, expected)) throw new Error('启动记录读回校验失败。');
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
