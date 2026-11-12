import { ESPLoader, Transport } from 'esptool-js';
import {
  backupFlash,
  firmwarePath,
  inspectDevice,
  installFirmware,
  RELEASE_MANIFEST,
  resetToFirmware,
  validateRelease,
} from './flash.mjs';
import { requestXuesuPort, usesAndroidUsb } from './ports.mjs';

const byId = (id) => document.getElementById(id);
const connectButton = byId('connect');
const backupButton = byId('backup');
const installButton = byId('install');
const backupDownload = byId('backup-download');
const androidUsb = usesAndroidUsb(navigator);
const supported = window.isSecureContext && (androidUsb || Boolean(navigator.serial));
let release;
let session;
let busy = false;
let ready = false;

function message(text, error = false) {
  const status = byId('status');
  status.textContent = text;
  status.dataset.error = String(error);
}

function renderButtons() {
  connectButton.disabled = !ready || !supported || busy;
  backupButton.disabled = busy || !session;
  installButton.disabled =
    busy || !release || !session || (session.mode === 'install' && !session.backedUp);
  installButton.classList.toggle('dark', !installButton.disabled);
  installButton.classList.toggle('light', installButton.disabled);
  connectButton.textContent = session ? '断开设备' : '连接设备 ↗';
  const connection = byId('connection');
  connection.textContent = session ? '已验证设备' : '未连接';
  connection.classList.toggle('connected', Boolean(session));
}

function progress(stage, percent) {
  byId('progress-panel').hidden = false;
  byId('stage').textContent = stage;
  const value = Math.max(0, Math.min(100, Math.floor(percent)));
  byId('percent').textContent = `${value}%`;
  byId('progress').value = value;
}

async function operation(work) {
  busy = true;
  renderButtons();
  try {
    await work();
  } catch (error) {
    const cancelled = error?.name === 'NotFoundError';
    message(
      cancelled ? '未选择设备。若列表为空，请检查数据线并唤醒雪酥。' : (error?.message ?? String(error)),
      !cancelled,
    );
  } finally {
    busy = false;
    renderButtons();
  }
}

function serialLog(text) {
  if (text.includes('MAC:')) return;
  const log = byId('log');
  byId('diagnostics').hidden = false;
  log.textContent = (log.textContent + text).slice(-6000);
  log.scrollTop = log.scrollHeight;
}

async function close(transport, usbDevice) {
  try {
    if (usbDevice) {
      // Closing the USB handle aborts pending transfers, even if a failed write left a stream locked.
      if (usbDevice.opened) await usbDevice.close();
    } else {
      await transport.disconnect();
    }
  } catch {
    // Reset or cable removal may already have closed the serial port.
  }
}

connectButton.addEventListener('click', () =>
  operation(async () => {
    if (session) {
      const { loader, transport, usbDevice } = session;
      session = undefined;
      byId('progress-panel').hidden = true;
      try {
        await resetToFirmware(loader);
        message('设备已断开，已发送重启命令。');
      } catch {
        message('设备已断开。若屏幕没有刷新，请重新插入 USB 线，再短按确认键。');
      } finally {
        await close(transport, usbDevice);
      }
      return;
    }

    const { port, usbDevice } = await requestXuesuPort(navigator);
    if (!backupDownload.hidden) {
      URL.revokeObjectURL(backupDownload.href);
      backupDownload.removeAttribute('href');
      backupDownload.hidden = true;
    }
    const transport = new Transport(port);
    const loader = new ESPLoader({
      transport,
      baudrate: 115200,
      // Read one USB packet at a time so a full CDC packet never waits for a short packet to flush it.
      serialOptions: usbDevice ? { bufferSize: 64 } : undefined,
      terminal: {
        clean: () => {
          byId('log').textContent = '';
        },
        writeLine: (text) => serialLog(`${text}\n`),
        write: serialLog,
      },
    });
    byId('progress-panel').hidden = true;
    message('正在连接并读取设备信息…');
    try {
      await loader.main();
      const mode = await inspectDevice(loader);
      session = { port, usbDevice, transport, loader, mode, backedUp: false };
      message(
        mode === 'install'
          ? '已识别雪酥。请先备份 Flash。'
          : '已识别雪酥，可以更新 OTA1。',
      );
    } catch (error) {
      await close(transport, usbDevice);
      throw error;
    }
  }),
);

backupButton.addEventListener('click', () =>
  operation(async () => {
    const current = session;
    message('正在读取 16 MiB Flash，备份期间请保持 USB 连接。');
    progress('读取 Flash', 0);
    const bytes = await backupFlash(current.loader, (read, total) =>
      progress('读取 Flash', (100 * read) / total),
    );
    if (!backupDownload.hidden) URL.revokeObjectURL(backupDownload.href);
    backupDownload.href = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
    backupDownload.download = `xuesu-flash-${new Date().toISOString().replaceAll(':', '-')}.bin`;
    backupDownload.hidden = false;
    progress('备份完成', 100);
    if (current.usbDevice) {
      message('备份已生成，请点「下载 Flash 备份」。');
    } else {
      backupDownload.click();
      message('请在浏览器的下载列表确认备份文件已保存。');
    }
  }),
);

backupDownload.addEventListener('click', () => {
  if (session) session.backedUp = true;
  if (!busy) message('请在浏览器的下载列表确认备份文件已保存。');
  renderButtons();
});

installButton.addEventListener('click', () => {
  const current = session;
  const reminder = current.mode === 'install' ? '请确认已保存本机 Flash 备份。' : '';
  if (!window.confirm(`${reminder}将 v${release.version} 写入 OTA1。开始后请保持 USB 连接。`)) return;
  operation(async () => {
    message('正在下载并校验固件…');
    progress('下载固件', 0);
    const response = await fetch(firmwarePath(release));
    if (!response.ok) throw new Error(`固件下载失败（HTTP ${response.status}）。`);
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (session !== current) throw new Error('USB 连接已断开，请重新连接设备。');
    message('正在刷机，请勿拔出 USB 数据线。');
    await installFirmware(current.loader, bytes, release, current.mode, progress);
    session = undefined;
    try {
      await resetToFirmware(current.loader);
      message('固件写入和校验完成，已发送重启命令。请等待屏幕刷新。');
    } catch {
      message('固件写入和校验完成。若屏幕没有刷新，请重新插入 USB 线，再短按确认键。');
    } finally {
      await close(current.transport, current.usbDevice);
    }
  });
});

function deviceDisconnected() {
  const current = session;
  session = undefined;
  if (!busy) message('USB 连接已断开。');
  renderButtons();
  if (current.usbDevice) void close(current.transport, current.usbDevice);
}

navigator.serial?.addEventListener('disconnect', (event) => {
  if (event.target === session?.port) deviceDisconnected();
});

navigator.usb?.addEventListener('disconnect', (event) => {
  if (session?.usbDevice && event.device === session.usbDevice) deviceDisconnected();
});

window.addEventListener('beforeunload', (event) => {
  if (!busy) return;
  event.preventDefault();
  event.returnValue = '';
});

async function loadRelease() {
  try {
    const response = await fetch(RELEASE_MANIFEST, { cache: 'no-cache' });
    if (response.status === 404) {
      message('站点暂无已发布的固件。');
      return;
    }
    if (!response.ok) throw new Error(`无法读取发布信息（HTTP ${response.status}）。`);
    release = validateRelease(await response.json());
    byId('version').textContent = `v${release.version}`;
    byId('date').textContent = new Intl.DateTimeFormat('zh-CN', { dateStyle: 'medium' }).format(
      new Date(release.publishedAt),
    );
    const download = byId('download');
    download.href = firmwarePath(release);
    download.download = `${release.releaseTag}-ota1.bin`;
    download.hidden = false;
    const source = byId('source');
    source.href = `https://github.com/${release.repository}`;
    source.hidden = false;
    const notes = byId('release-notes');
    notes.href = `${source.href}/releases/tag/${release.releaseTag}`;
    notes.hidden = false;
    byId('license').hidden = false;
    message('连接设备后，浏览器会先检查型号与分区。');
  } catch (error) {
    message(error.message, true);
  } finally {
    ready = true;
    if (!supported) message('请通过 HTTPS 打开本页，使用电脑上的 Chrome 或 Edge，或安卓上的 Chrome。');
    renderButtons();
  }
}

loadRelease();
