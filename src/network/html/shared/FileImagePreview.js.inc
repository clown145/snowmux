let xmpicPreviewRequest = null;

function isXmpicFile(name) {
  return /\.xmpic$/i.test(name);
}

function isImageFile(name) {
  return /\.(png|jpe?g|bmp|gif|webp)$/i.test(name) || isXmpicFile(name);
}

// Factory wallpaper: 300×400, top down, MSB first, with 1 representing black.
const XMPIC_WIDTH = 300;
const XMPIC_HEIGHT = 400;
const XMPIC_STRIDE = Math.ceil(XMPIC_WIDTH / 8);
const XMPIC_BYTES = XMPIC_STRIDE * XMPIC_HEIGHT;

function xmpicPreviewUrl(bytes) {
  if (bytes.length !== XMPIC_BYTES) throw new Error('Invalid XMPIC raster size');
  const canvas = document.createElement('canvas');
  canvas.width = XMPIC_WIDTH;
  canvas.height = XMPIC_HEIGHT;
  const ctx = canvas.getContext('2d');
  const image = ctx.createImageData(XMPIC_WIDTH, XMPIC_HEIGHT);
  for (let y = 0; y < XMPIC_HEIGHT; y++) {
    for (let x = 0; x < XMPIC_WIDTH; x++) {
      const black = bytes[y * XMPIC_STRIDE + (x >> 3)] & (0x80 >> (x & 7));
      const shade = black ? 0 : 255;
      const pixel = (y * XMPIC_WIDTH + x) * 4;
      image.data[pixel] = image.data[pixel + 1] = image.data[pixel + 2] = shade;
      image.data[pixel + 3] = 255;
    }
  }
  ctx.putImageData(image, 0, 0);
  return canvas.toDataURL();
}

async function openImagePreview(url, name) {
  xmpicPreviewRequest?.abort();
  xmpicPreviewRequest = null;
  const img = document.getElementById('imagePreviewImg');
  document.getElementById('imagePreviewName').textContent = name;
  img.removeAttribute('src');
  img.alt = name;
  document.getElementById('imagePreviewDownload').href = url;
  openWebModal('imagePreviewModal');
  if (!isXmpicFile(name)) {
    img.src = url;
    return;
  }

  const request = new AbortController();
  xmpicPreviewRequest = request;
  try {
    const response = await fetch(url, {signal: request.signal, cache: 'no-store'});
    if (request.signal.aborted) return;
    if (!response.ok || Number(response.headers.get('content-length')) !== XMPIC_BYTES) {
      throw new Error('Invalid XMPIC download');
    }
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (!request.signal.aborted) img.src = xmpicPreviewUrl(bytes);
  } catch (error) {
    if (!request.signal.aborted) showWebToast(img.dataset.loadError, true);
  } finally {
    if (xmpicPreviewRequest === request) xmpicPreviewRequest = null;
  }
}

function closeImagePreview() {
  xmpicPreviewRequest?.abort();
  xmpicPreviewRequest = null;
  closeWebModal('imagePreviewModal');
  document.getElementById('imagePreviewImg').removeAttribute('src');
}
