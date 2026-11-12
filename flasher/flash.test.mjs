import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import test from 'node:test';
import { crc32 } from 'node:zlib';
import {
  backupFlash,
  FLASH_SIZE,
  inspectDevice,
  installFirmware,
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

const OTA0 = 0x20000;
const OTA1 = 0x750000;
const partitionTable = Buffer.alloc(0xc00, 0xff);
// Partition records and MD5 footer generated from partitions_xuesu_300.csv.
partitionTable.set(Buffer.from([
  'aa50010200900000006000006e76730000000000000000000000000000000000',
  'aa50010000f00000002000006f74616461746100000000000000000000000000',
  'aa50001000000200000073006f74615f30000000000000000000000000000000',
  'aa50001100007500000073006f74615f31000000000000000000000000000000',
  'aa5001820000e800000018007370696666730000000000000000000000000000',
  'ebebffffffffffffffffffffffffffffaf8aece4c2c246a0f5b19dad2448c932',
].join(''), 'hex'));

function appImage(board = 'xuesu_300', tagOffset = 0) {
  const segment = Buffer.alloc(tagOffset + 32);
  segment.write(`CROSSPOINT-BOARD-V1:${board};`, tagOffset);
  const image = Buffer.alloc(32 + segment.length);
  image[0] = 0xe9;
  image[1] = 1;
  image[12] = 9;
  image.writeUInt32LE(segment.length, 28);
  image.set(segment, 32);
  return image;
}

const image = appImage();
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

function mockLoader({ bootData = selectors(), ota0App = 'stock', ota1App, failReadback = false } = {}) {
  const writes = [];
  const apps = new Map([[OTA0, ota0App], [OTA1, ota1App]]);
  const records = bootData.slice();
  return {
    writes,
    async getSecurityInfo() { return { chipId: 9, parsedFlags: {}, flashCryptCnt: 0 }; },
    async detectFlashSize() { return '16MB'; },
    async readFlash(address, length) {
      if (address === 0x8000) return partitionTable.slice(0, length);
      if (address === 0xf000 || address === 0x10000) {
        const offset = address - 0xf000;
        return records.slice(offset, offset + length);
      }
      const slot = address >= OTA1 ? OTA1 : OTA0;
      const app = apps.get(slot);
      const offset = address - slot;
      const read = new Uint8Array(length).fill(0xff);
      if (app instanceof Uint8Array) read.set(app.subarray(offset, offset + length));
      return read;
    },
    async writeFlash({ fileArray, eraseAll, flashSize }) {
      assert.equal(eraseAll, false);
      assert.equal(flashSize, 'keep');
      assert.equal(fileArray.length, 1);
      const { address, data } = fileArray[0];
      writes.push({ address, data });
      if (apps.has(address)) apps.set(address, data);
      else records.set(data, address - 0xf000);
    },
    async flashMd5sum(address, size) {
      const app = apps.get(address);
      if (app === 'stock' && size === 2_915_888) return 'd5c277975ddeec6428857a398750a4df';
      if (failReadback && size === image.length) return 'bad';
      return app instanceof Uint8Array ? createHash('md5').update(app.subarray(0, size)).digest('hex') : '';
    },
  };
}

test('accepts blank factory records and the legacy web installation', async () => {
  assert.equal((await inspectDevice(mockLoader())).mode, 'install');
  assert.equal((await inspectDevice(mockLoader({ bootData: selectors(erased, erased) }))).mode, 'install');
  const legacy = mockLoader({ bootData: selectors(stock, ota1), ota1App: image });
  assert.equal((await inspectDevice(legacy)).mode, 'update');
});

test('restored factory OTA0 keeps its active record while selecting OTA1', async () => {
  const bootData = selectors(restoredStock, ota1);
  const loader = mockLoader({ bootData });
  const plan = await inspectDevice(loader);
  assert.equal(plan.mode, 'install');
  await installFirmware(loader, image, release, plan, () => {});
  assert.deepEqual(loader.writes.map(({ address }) => address), [0x750000, 0x10000]);
  const newSelector = loader.writes[1].data;
  const entry = new DataView(newSelector.buffer);
  assert.equal(entry.getUint32(0, true), 4);
  assert.equal(entry.getUint32(28, true), crc32(newSelector.subarray(0, 4), 0xffffffff));
  assert.equal((await inspectDevice(loader)).mode, 'update');
});

test('preserves the second record if it selects factory OTA0', async () => {
  const loader = mockLoader({ bootData: selectors(selector(8), selector(9, 2)) });
  assert.deepEqual((await inspectDevice(loader)).activate, {
    address: 0xf000,
    sequence: 10,
  });
});

test('does not trust damaged, pending, or unrelated OTA1 boot records', async () => {
  const damaged = restoredStock.slice();
  damaged[28] ^= 1;
  await assert.rejects(inspectDevice(mockLoader({ bootData: selectors(damaged, ota1) })), /校验异常/);
  await assert.rejects(inspectDevice(mockLoader({ bootData: selectors(selector(3, 1), ota1) })), /尚未确认/);
  const wrongBoard = mockLoader({ bootData: selectors(restoredStock, selector(4)), ota1App: appImage('x4') });
  await assert.rejects(inspectDevice(wrongBoard), /未找到受支持/);
  const aborted = mockLoader({ bootData: selectors(restoredStock, selector(4, 4)) });
  assert.equal((await inspectDevice(aborted)).mode, 'install');
});

test('first installation writes the app before the verified second selector', async () => {
  const loader = mockLoader();
  await installFirmware(loader, image, release, await inspectDevice(loader), () => {});
  assert.deepEqual(
    loader.writes.map(({ address }) => address),
    [0x750000, 0x10000],
  );
  assert.equal((await inspectDevice(loader)).mode, 'update');
});

test('updates leave both boot selectors untouched', async () => {
  const bootData = selectors(stock, ota1);
  const loader = mockLoader({ bootData, ota1App: image });
  await installFirmware(loader, image, release, await inspectDevice(loader), () => {});
  assert.deepEqual(
    loader.writes.map(({ address }) => address),
    [0x750000],
  );
});

test('retries an interrupted web write while the factory app still identifies the device', async () => {
  const loader = mockLoader({ bootData: selectors(stock, ota1) });
  const plan = await inspectDevice(loader);
  assert.equal(plan.mode, 'update');
  await installFirmware(loader, image, release, plan, () => {});
  assert.deepEqual(loader.writes.map(({ address }) => address), [OTA1]);
});

test('recognizes both slots after alternating in-device updates', async () => {
  const ota0Active = mockLoader({
    bootData: selectors(selector(3, 2), ota1), ota0App: image, ota1App: image,
  });
  const fromOta0 = await inspectDevice(ota0Active);
  assert.equal(fromOta0.mode, 'update');
  assert.equal(fromOta0.target.label, 'OTA1');
  await installFirmware(ota0Active, image, release, fromOta0, () => {});
  assert.deepEqual(ota0Active.writes.map(({ address }) => address), [OTA1, 0x10000]);

  const ota1Active = mockLoader({
    // An ESP-IDF record has no web label. This tag spans two Flash reads.
    bootData: selectors(selector(3, 2), selector(4, 2)),
    ota0App: image,
    ota1App: appImage('xuesu_300', 256 * 1024 - 10),
  });
  const fromOta1 = await inspectDevice(ota1Active);
  assert.equal(fromOta1.mode, 'update');
  assert.equal(fromOta1.target.label, 'OTA0');
  await installFirmware(ota1Active, image, release, fromOta1, () => {});
  assert.deepEqual(ota1Active.writes.map(({ address }) => address), [OTA0, 0xf000]);
  assert.equal((await inspectDevice(ota1Active)).target.label, 'OTA1');
});

test('recovers an interrupted device update without overwriting the booted app', async () => {
  const loader = mockLoader({ bootData: selectors(stock, ota1), ota0App: image.subarray(0, 24), ota1App: image });
  const plan = await inspectDevice(loader);
  assert.equal(plan.target.label, 'OTA0');
  assert.deepEqual(plan.activate, { address: 0xf000, sequence: 3 });
});

test('installs into OTA0 after factory firmware is restored to OTA1', async () => {
  const loader = mockLoader({
    bootData: selectors(selector(3, 2), selector(4, 2)), ota0App: image, ota1App: 'stock',
  });
  const plan = await inspectDevice(loader);
  assert.equal(plan.mode, 'install');
  assert.equal(plan.target.label, 'OTA0');
  await installFirmware(loader, image, release, plan, () => {});
  assert.deepEqual(loader.writes.map(({ address }) => address), [OTA0, 0xf000]);
});

test('ignores a board tag outside declared image segments', async () => {
  const app = appImage();
  app.writeUInt32LE(0x730000, 28);
  const loader = mockLoader({ bootData: selectors(restoredStock, selector(4, 2)), ota1App: app });
  await assert.rejects(inspectDevice(loader), /未找到受支持/);
});

test('invalid downloads and changed selectors are rejected before writing', async () => {
  const loader = mockLoader();
  const plan = await inspectDevice(loader);
  const damaged = Uint8Array.from(image);
  damaged[30] = 1;
  await assert.rejects(
    installFirmware(loader, damaged, release, plan, () => {}),
    /SHA-256/,
  );
  await assert.rejects(
    installFirmware(mockLoader({ bootData: selectors(restoredStock, ota1) }), image, release, plan, () => {}),
    /已变化/,
  );
  assert.equal(loader.writes.length, 0);
  const oversized = { ...manifest, assets: [{ ...manifest.assets[0], size: 0x730010 }] };
  assert.throws(() => validateRelease(oversized), /发布信息/);
  assert.throws(() => validateRelease({ ...manifest, target_partition: 'ota_0' }), /发布信息/);
});

test('a failed OTA1 readback prevents changing the boot selector', async () => {
  const loader = mockLoader({ failReadback: true });
  await assert.rejects(
    installFirmware(loader, image, release, await inspectDevice(loader), () => {}),
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
