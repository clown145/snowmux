import { ESPLoader, Transport } from 'esptool-js';
import {
  backupFlash,
  firmwarePath,
  inspectDevice,
  installFirmware,
  RELEASE_MANIFEST,
  validateRelease,
} from './flash.mjs';

const byId = (id) => document.getElementById(id);
const connectButton = byId('connect');
const backupButton = byId('backup');
const installButton = byId('install');
const supported = window.isSecureContext && Boolean(navigator.serial);
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
    message(
      error.name === 'NotFoundError' ? '已取消串口选择。' : error.message,
      error.name !== 'NotFoundError',
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

async function close(transport) {
  try {
    await transport.disconnect();
  } catch {
    // Reset or cable removal may already have closed the serial port.
  }
}

connectButton.addEventListener('click', () =>
  operation(async () => {
    if (session) {
    const { loader, transport } = session;
      session = undefined;
      byId('progress-panel').hidden = true;
    try {
      await loader.after('hard_reset');
      message('设备已断开，正在重启。');
    } catch {
      message('设备已断开。若设备仍停留在刷机模式，请手动重启。');
    } finally {
      await close(transport);
    }
      return;
    }

    const port = await navigator.serial.requestPort({
      filters: [{ usbVendorId: 0x303a, usbProductId: 0x1001 }],
    });
    const transport = new Transport(port);
    const loader = new ESPLoader({
      transport,
      baudrate: 115200,
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
      session = { port, transport, loader, mode, backedUp: false };
      message(
        mode === 'install'
          ? '已识别雪酥。请先备份 Flash。'
          : '已识别雪酥，可以更新 OTA1。',
      );
    } catch (error) {
      await close(transport);
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
    const link = document.createElement('a');
    link.href = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
    link.download = `xuesu-flash-${new Date().toISOString().replaceAll(':', '-')}.bin`;
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(link.href), 60_000);
    current.backedUp = true;
    message('备份已生成，请在浏览器的下载列表确认文件已保存。');
    progress('备份完成', 100);
  }),
);

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
    message('正在刷机，请勿拔出 USB 数据线。');
    await installFirmware(current.loader, bytes, release, current.mode, progress);
    session = undefined;
    try {
      await current.loader.after('hard_reset');
      message('固件写入和校验完成，设备正在重启。');
    } catch {
      message('固件写入和校验完成。请手动重启设备。');
    } finally {
      await close(current.transport);
    }
  });
});

navigator.serial?.addEventListener('disconnect', (event) => {
  if (event.target !== session?.port) return;
  session = undefined;
  if (!busy) message('USB 连接已断开。');
  renderButtons();
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
    if (!supported) message('请在电脑上用 Chrome 或 Edge 打开 HTTPS 站点进行刷机。');
    renderButtons();
  }
}

loadRelease();
