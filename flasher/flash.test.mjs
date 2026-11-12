import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import test from 'node:test';
import { crc32 } from 'node:zlib';
import {
  backupFlash,
  FLASH_SIZE,
  installFirmware,
  planBootSelection,
  sha256,
  validateRelease,
} from './flash.mjs';

const erased = new Uint8Array(4096).fill(0xff);
const stock = erased.slice();
stock.fill(0, 0, 4);

function selector(sequence, state = 0xffffffff) {
  const sector = erased.slice();
  const entry = new DataView(sector.buffer);
  entry.setUint32(0, sequence, true);
  entry.setUint32(24, state, true);
  entry.setUint32(28, crc32(sector.subarray(0, 4), 0xffffffff), true);
  return sector;
}

function selectors(first = stock, second = erased) {
  const data = new Uint8Array(8192);
  data.set(first);
  data.set(second, 4096);
  return data;
}

const ota1 = selector(2);
const restoredStock = erased.slice();
restoredStock.set(Buffer.from('03000000ffffffffffffffffffffffffffffffffffffffff0200000011504aed', 'hex'));

const image = new Uint8Array(64);
image[0] = 0xe9;
image[12] = 9;
const hash = await sha256(image);
const manifest = {
  schema: 1,
  board: 'xuesu-300-std',
  target_partition: 'ota_1',
  tag_name: '0.1.0',
  release_tag: 'xuesu-v0.1.0',
  published_at: '2026-01-01T00:00:00Z',
  repository: 'test/snowmux',
  commit: 'a'.repeat(40),
  assets: [
    {
      name: 'firmware.bin',
      size: image.length,
      browser_download_url: `https://test.github.io/snowmux/firmware/${hash}.bin`,
      digest: `sha256:${hash}`,
      md5: createHash('md5').update(image).digest('hex'),
    },
  ],
};
const release = validateRelease(manifest);

function mockLoader({ bootData = selectors(), digest = release.md5 } = {}) {
  const writes = [];
  return {
    writes,
    async readFlash(address, length) {
      if (address === 0xf000 && length === 8192) return bootData.slice();
      assert.equal(length, 4096);
      return writes.findLast((write) => write.address === address).data;
    },
    async writeFlash({ fileArray, eraseAll, flashSize }) {
      assert.equal(eraseAll, false);
      assert.equal(flashSize, 'keep');
      assert.equal(fileArray.length, 1);
      writes.push(fileArray[0]);
    },
    async flashMd5sum(address, size) {
      assert.equal(address, 0x750000);
      assert.equal(size, image.length);
      return digest;
    },
  };
}

test('accepts blank factory records and the legacy web installation', () => {
  assert.equal(planBootSelection(selectors()).mode, 'install');
  assert.equal(planBootSelection(selectors(erased, erased)).mode, 'install');
  assert.equal(planBootSelection(selectors(stock, ota1)).mode, 'update');
  assert.throws(() => planBootSelection(selectors(stock, stock)), /启动记录/);
  assert.throws(() => planBootSelection(selectors(ota1, erased)), /无法确认/);
});

test('restored factory OTA0 keeps its active record while selecting OTA1', async () => {
  const bootData = selectors(restoredStock, ota1);
  const plan = planBootSelection(bootData);
  assert.equal(plan.mode, 'install');
  const loader = mockLoader({ bootData });
  await installFirmware(loader, image, release, plan, () => {});
  assert.deepEqual(loader.writes.map(({ address }) => address), [0x750000, 0x10000]);
  const newSelector = loader.writes[1].data;
  const entry = new DataView(newSelector.buffer);
  assert.equal(entry.getUint32(0, true), 4);
  assert.equal(entry.getUint32(28, true), crc32(newSelector.subarray(0, 4), 0xffffffff));
  assert.equal(planBootSelection(selectors(restoredStock, newSelector)).mode, 'update');
});

test('preserves the second record if it selects factory OTA0', () => {
  assert.deepEqual(planBootSelection(selectors(selector(8), selector(9, 2))).activate, {
    address: 0xf000,
    sequence: 10,
  });
});

test('does not trust damaged, pending, or unrelated OTA1 boot records', () => {
  const damaged = restoredStock.slice();
  damaged[28] ^= 1;
  assert.throws(() => planBootSelection(selectors(damaged, ota1)), /校验异常/);
  assert.throws(() => planBootSelection(selectors(selector(3, 1), ota1)), /尚未确认/);
  assert.throws(() => planBootSelection(selectors(restoredStock, selector(4))), /无法确认/);
  assert.equal(planBootSelection(selectors(restoredStock, selector(4, 4))).mode, 'install');
});

test('first installation writes the app before the verified second selector', async () => {
  const loader = mockLoader();
  await installFirmware(loader, image, release, planBootSelection(selectors()), () => {});
  assert.deepEqual(
    loader.writes.map(({ address }) => address),
    [0x750000, 0x10000],
  );
  assert.equal(planBootSelection(selectors(stock, loader.writes[1].data)).mode, 'update');
});

test('updates leave both boot selectors untouched', async () => {
  const bootData = selectors(stock, ota1);
  const loader = mockLoader({ bootData });
  await installFirmware(loader, image, release, planBootSelection(bootData), () => {});
  assert.deepEqual(
    loader.writes.map(({ address }) => address),
    [0x750000],
  );
});

test('invalid downloads and changed selectors are rejected before writing', async () => {
  const loader = mockLoader();
  const damaged = image.slice();
  damaged[30] = 1;
  await assert.rejects(
    installFirmware(loader, damaged, release, planBootSelection(selectors()), () => {}),
    /SHA-256/,
  );
  await assert.rejects(
    installFirmware(loader, image, release, planBootSelection(selectors(restoredStock, ota1)), () => {}),
    /已变化/,
  );
  assert.equal(loader.writes.length, 0);
  const oversized = { ...manifest, assets: [{ ...manifest.assets[0], size: 0x730010 }] };
  assert.throws(() => validateRelease(oversized), /发布信息/);
  assert.throws(() => validateRelease({ ...manifest, target_partition: 'ota_0' }), /发布信息/);
});

test('a failed OTA1 readback prevents changing the boot selector', async () => {
  const loader = mockLoader({ digest: 'bad' });
  await assert.rejects(
    installFirmware(loader, image, release, planBootSelection(selectors()), () => {}),
    /写入校验失败/,
  );
  assert.deepEqual(
    loader.writes.map(({ address }) => address),
    [0x750000],
  );
});

test('the Flash backup reads the complete device in bounded chunks', async () => {
  let offset = 0;
  const loader = {
    async readFlash(address, length) {
      assert.equal(address, offset);
      assert.equal(length, 256 * 1024);
      offset += length;
      return new Uint8Array(length).fill(0xff);
    },
  };
  const backup = await backupFlash(loader, () => {});
  assert.equal(offset, FLASH_SIZE);
  assert.equal(backup.length, FLASH_SIZE);
  assert.equal(backup.at(-1), 0xff);
});
