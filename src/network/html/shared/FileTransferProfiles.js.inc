// Portrait dimensions for manual targets and status responses from older firmware.
const DEVICE_PROFILES = {
  X4: { width: 480, height: 800, label: 'X4' },
  X3: { width: 528, height: 792, label: 'X3' },
  A4: { width: 552, height: 768, label: 'eego A4' },
  M4: { width: 480, height: 800, label: 'Murphy M4' },
  Xuesu: { width: 300, height: 400, label: XUESU_DEVICE_LABEL },
};
const STATUS_DEVICE_MAP = {
  xteink_x4: 'X4', xteink_x3: 'X3', xteink_x3_uc8279: 'X3',
  eego_a4: 'A4', murphy_m4: 'M4', 'xuesu-300-std': 'Xuesu',
};

let DEVICE_TARGET = 'auto';
let detectedProfile = null;
let MAX_WIDTH = 0;
let MAX_HEIGHT = 0;
let deviceStatusRequest = null;
let crosspointVersion = 'Unknown';

function profileFromStatus({ device, screenWidth, screenHeight }) {
  const id = Object.hasOwn(STATUS_DEVICE_MAP, device) ? STATUS_DEVICE_MAP[device] : device;
  const preset = Object.hasOwn(DEVICE_PROFILES, id) ? DEVICE_PROFILES[id] : null;
  if (Number.isInteger(screenWidth) && screenWidth > 0 && Number.isInteger(screenHeight) && screenHeight > 0) {
    return { width: screenWidth, height: screenHeight, label: preset?.label || device };
  }
  return preset;
}

function fetchVersion() {
  if (!deviceStatusRequest) {
    deviceStatusRequest = fetchDeviceStatus().finally(() => { deviceStatusRequest = null; });
  }
  return deviceStatusRequest;
}

async function fetchDeviceStatus() {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 5000);
  try {
    const response = await fetch('/api/status', { signal: controller.signal });
    if (!response.ok) return;
    const data = await response.json();
    crosspointVersion = data.version || 'Unknown';
    detectedProfile = profileFromStatus(data);
    applyDeviceTarget();
  } catch (error) {
    console.warn('Could not read device status:', error);
  } finally {
    clearTimeout(timeout);
  }
}

async function ensureDeviceTarget() {
  if (DEVICE_TARGET === 'auto' && !detectedProfile) await fetchVersion();
  return MAX_WIDTH > 0 && MAX_HEIGHT > 0;
}

function imageTargetFlags({ width, height }, targetWidth = MAX_WIDTH, targetHeight = MAX_HEIGHT) {
  return {
    fitsScreen: targetWidth > 0 && width <= targetWidth && height <= targetHeight,
    canHSplit: targetHeight > 0 && width >= targetHeight,
    canVSplit: targetHeight > 0 && height >= targetHeight,
  };
}

function applyDeviceTarget() {
  const profile = DEVICE_TARGET === 'auto' ? detectedProfile : DEVICE_PROFILES[DEVICE_TARGET];
  MAX_WIDTH = profile?.width ?? 0;
  MAX_HEIGHT = profile?.height ?? 0;

  const summary = document.getElementById('convertSizeSummary');
  summary.textContent = profile
    ? `${summary.dataset.maxLabel} ${MAX_WIDTH}×${MAX_HEIGHT} px`
    : summary.dataset.unknown;
  document.querySelectorAll('.device-btn').forEach(button => {
    button.classList.toggle('active', button.dataset.value === DEVICE_TARGET);
  });
  const autoLabel = document.getElementById('deviceAutoLabel');
  const label = autoLabel.dataset.label;
  autoLabel.textContent = detectedProfile?.label ? `${label} (${detectedProfile.label})` : label;

  for (const image of epubImagesCache) Object.assign(image, imageTargetFlags(image));
  if (epubImagesCache.length && isImagePickerVisible()) renderImageGrid();
}

function setDeviceTarget(value) {
  if (isUploadInProgress) return;
  DEVICE_TARGET = Object.hasOwn(DEVICE_PROFILES, value) ? value : 'auto';
  applyDeviceTarget();
  updateUploadSettingsPersistence();
}
