import assert from 'node:assert/strict';
import test from 'node:test';
import { requestXuesuPort, usesAndroidUsb } from './ports.mjs';

test('Android selects CDC over WebUSB even when Bluetooth Web Serial is present', async () => {
  const device = {
    vendorId: 0x303a,
    productId: 0x1001,
    configurations: [{
      interfaces: [
        { interfaceNumber: 0, alternates: [{ interfaceClass: 2 }] },
        {
          interfaceNumber: 1,
          alternates: [{
            interfaceClass: 10,
            endpoints: [
              { direction: 'in', endpointNumber: 1, packetSize: 64 },
              { direction: 'out', endpointNumber: 1, packetSize: 64 },
            ],
          }],
        },
      ],
    }],
  };
  const browser = {
    userAgentData: { platform: 'Android' },
    usb: {
      async requestDevice({ filters }) {
        assert.deepEqual(filters, [{ vendorId: 0x303a, productId: 0x1001 }]);
        return device;
      },
    },
    serial: { requestPort() { assert.fail('Android should use WebUSB'); } },
  };
  const { port, usbDevice } = await requestXuesuPort(browser);
  assert.equal(usbDevice, device);
  assert.deepEqual(port.getInfo(), { usbVendorId: 0x303a, usbProductId: 0x1001 });

  const denial = new DOMException('No device selected', 'NotFoundError');
  browser.usb.requestDevice = async () => { throw denial; };
  await assert.rejects(requestXuesuPort(browser), (error) => error === denial);
});

test('desktop Chrome with both APIs keeps using native Web Serial', async () => {
  const nativePort = {};
  const browser = {
    userAgentData: { platform: 'Windows' },
    usb: { requestDevice() { assert.fail('Desktop should use Web Serial'); } },
    serial: {
      async requestPort({ filters }) {
        assert.deepEqual(filters, [{ usbVendorId: 0x303a, usbProductId: 0x1001 }]);
        return nativePort;
      },
    },
  };
  assert.equal(usesAndroidUsb(browser), false);
  assert.deepEqual(await requestXuesuPort(browser), { port: nativePort });
});
