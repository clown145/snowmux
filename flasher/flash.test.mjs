import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import test from 'node:test';
import {
  backupFlash,
  FLASH_SIZE,
  installFirmware,
  selectorMode,
  sha256,
  validateRelease,
} from './flash.mjs';

const erased = new Uint8Array(4096).fill(0xff);
const stock = erased.slice();
stock.fill(0, 0, 4);
const ota1 = erased.slice();
ota1.set([2, 0, 0, 0]);
ota1.set([0x74, 0x37, 0xf6, 0x55], 28);

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

function mockLoader({ selector = erased, digest = release.md5 } = {}) {
  const writes = [];
  return {
    writes,
    async readFlash(address, length) {
      if (address === 0xf000 && length === 8192) return Uint8Array.from([...stock, ...selector]);
      assert.equal(address, 0x10000);
      assert.equal(length, 4096);
      return writes.at(-1).data;
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

test('accepts only known startup records', () => {
  assert.equal(selectorMode(stock, erased), 'install');
  assert.equal(selectorMode(stock, ota1), 'update');
  assert.throws(() => selectorMode(stock, stock), /启动记录/);
  assert.throws(() => selectorMode(ota1, erased), /启动记录/);
});

test('first installation writes the app before the verified second selector', async () => {
  const loader = mockLoader();
  await installFirmware(loader, image, release, 'install', () => {});
  assert.deepEqual(
    loader.writes.map(({ address }) => address),
    [0x750000, 0x10000],
  );
  assert.deepEqual(loader.writes[1].data, ota1);
});

test('updates leave both boot selectors untouched', async () => {
  const loader = mockLoader({ selector: ota1 });
  await installFirmware(loader, image, release, 'update', () => {});
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
    installFirmware(loader, damaged, release, 'install', () => {}),
    /SHA-256/,
  );
  await assert.rejects(
    installFirmware(loader, image, release, 'update', () => {}),
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
    installFirmware(loader, image, release, 'install', () => {}),
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
