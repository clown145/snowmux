import { SerialPort } from 'web-serial-polyfill';

const vendorId = 0x303a;
const productId = 0x1001;

export function usesAndroidUsb(browser) {
  const android = browser.userAgentData?.platform === 'Android' || /\bAndroid\b/.test(browser.userAgent);
  return android && typeof browser.usb?.requestDevice === 'function';
}

export async function requestXuesuPort(browser) {
  if (usesAndroidUsb(browser)) {
    // Native Web Serial may only offer Bluetooth ports on Android.
    const usbDevice = await browser.usb.requestDevice({ filters: [{ vendorId, productId }] });
    return { port: new SerialPort(usbDevice), usbDevice };
  }

  const port = await browser.serial.requestPort({ filters: [{ usbVendorId: vendorId, usbProductId: productId }] });
  return { port };
}
