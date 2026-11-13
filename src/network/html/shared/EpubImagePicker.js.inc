// Load the ZIP reader and optimizer only when EPUB tools are used.
const SCRIPT_LOAD_TIMEOUT_MS = 20000;
let jszipPromise = null;
let epubToolsPromise = null;

function loadScript(src) {
  return new Promise((resolve, reject) => {
    const s = document.createElement('script');
    const fail = (message) => {
      clearTimeout(timer);
      s.remove();
      reject(new Error(message));
    };
    // A stalled request would otherwise block uploads and Cancel indefinitely.
    const timer = setTimeout(() => fail('Timed out loading ' + src), SCRIPT_LOAD_TIMEOUT_MS);
    s.onload = () => {
      clearTimeout(timer);
      resolve();
    };
    s.onerror = () => fail('Failed to load ' + src);
    s.src = src;
    document.head.appendChild(s);
  });
}

// File Manager plugins continue to use JSZip.
function ensureJszip() {
  if (typeof JSZip !== 'undefined') return Promise.resolve();
  if (!jszipPromise) {
    jszipPromise = loadScript('/js/jszip.min.js');
    jszipPromise.catch(() => { jszipPromise = null; });  // let a later use retry
  }
  return jszipPromise;
}

function ensureEpubTools() {
  if (!epubToolsPromise) {
    // One at a time: the device serves one connection at a time. An earlier
    // timed-out attempt may already have loaded the optimizer.
    epubToolsPromise = (typeof zip !== 'undefined' ? Promise.resolve() : loadScript('/js/zip.min.js'))
      .then(() => (typeof convertEpubFile === 'function' ? undefined : loadScript('/js/epub-optimizer.js')));
    epubToolsPromise.catch(() => { epubToolsPromise = null; });  // let a later use retry
  }
  return epubToolsPromise;
}

const EPUB_PREVIEW_PAGE_SIZE = 24;
let epubImagesCache = [];
let imagePickerSession = null;
let imageStates = new Map();
let imageBatchStates = new Map();
let imageStateRevision = 0;
let activeEpubConversion = null;

function isImagePickerVisible() {
  return document.getElementById('imagePickerSection').style.display !== 'none';
}

function releaseImagePicker() {
  const session = imagePickerSession;
  imagePickerSession = null;
  session?.controller.abort();
  session?.pageController?.abort();
  epubImagesCache = [];
  document.getElementById('imageGrid').replaceChildren();
  document.getElementById('imagePickerSection').style.display = 'none';
  document.getElementById('imagePickerPagination').hidden = true;
  document.getElementById('startConversionBtn').style.display = 'none';
  document.getElementById('uploadBtn').style.display = 'block';
  document.querySelector('#uploadModal .modal').classList.remove('picker-mode');
  document.getElementById('pickerColumns').classList.remove('picker-active');
}

function clearImagePicker() {
  releaseImagePicker();
  imageStates.clear();
  imageBatchStates.clear();
  imageStateRevision = 0;
}

async function showImagePicker(file) {
  clearImagePicker();
  const session = { controller: new AbortController(), page: 0 };
  imagePickerSession = session;
  const { signal } = session.controller;
  const section = document.getElementById('imagePickerSection');
  const count = document.getElementById('imagePickerCount');
  section.style.display = 'block';
  count.textContent = epubPickerText.loading;
  document.querySelector('#uploadModal .modal').classList.add('picker-mode');
  document.getElementById('pickerColumns').classList.add('picker-active');
  try {
    await ensureEpubTools();
    signal.throwIfAborted();
    await ensureDeviceTarget();
    signal.throwIfAborted();
    const archive = await openEpub(file);
    try {
      session.images = await listEpubImages(archive.files, signal);
    } finally {
      await archive.reader.close();
    }
    signal.throwIfAborted();
    if (!session.images.length) {
      count.textContent = epubPickerText.empty;
      return;
    }
    session.file = file;
    document.getElementById('uploadBtn').style.display = 'none';
    document.getElementById('startConversionBtn').style.display = 'block';
    await showEpubPreviewPage(0);
  } catch (error) {
    if (signal.aborted) return;
    console.warn('Could not index EPUB images:', error);
    count.textContent = epubPickerText.failed;
  }
}

function moveEpubPreviewPage(delta) {
  if (imagePickerSession?.images) void showEpubPreviewPage(imagePickerSession.page + delta);
}

async function showEpubPreviewPage(page) {
  const session = imagePickerSession;
  const pageCount = Math.ceil(session.images.length / EPUB_PREVIEW_PAGE_SIZE);
  if (page < 0 || page >= pageCount) return;
  session.pageController?.abort();
  const controller = new AbortController();
  const { signal } = controller;
  session.pageController = controller;
  session.page = page;
  const first = page * EPUB_PREVIEW_PAGE_SIZE;
  const images = session.images.slice(first, first + EPUB_PREVIEW_PAGE_SIZE).map(image => ({ ...image }));
  epubImagesCache = images;

  const pagination = document.getElementById('imagePickerPagination');
  pagination.hidden = pageCount < 2;
  document.getElementById('epubPreviewPrevious').disabled = page === 0;
  document.getElementById('epubPreviewNext').disabled = page === pageCount - 1;
  document.getElementById('imagePickerCount').textContent =
    epubPickerText.range(first + 1, first + images.length, session.images.length);
  renderImageGrid();

  // Reopen only the catalog. Keeping it local lets closing the picker release the file.
  try {
    const archive = await openEpub(session.file);
    try {
      for (const image of images) {
        signal.throwIfAborted();
        try {
          const data = await readEpubEntry(archive.files.get(image.path), signal);
          Object.assign(image, await withEpubImage(data, signal, img => makeEpubPreview(img, image)));
        } catch (error) {
          if (signal.aborted) break;
          image.failed = true;
          console.warn('Could not preview EPUB image:', image.path, error);
        }
        if (!signal.aborted) updateEpubPreviewCard(image);
      }
    } finally {
      await archive.reader.close();
    }
  } catch (error) {
    if (signal.aborted) return;
    console.warn('Could not read EPUB preview page:', error);
    for (const image of images) {
      if (image.dataUrl) continue;
      image.failed = true;
      updateEpubPreviewCard(image);
    }
  }
}

function getImageState(path, image, locked) {
  if (locked) return 0;
  const width = image.naturalWidth || image.width;
  const height = image.naturalHeight || image.height;
  const manual = imageStates.get(path);
  let state = manual?.state || 0;
  let revision = manual?.revision || 0;
  for (const choice of imageBatchStates.values()) {
    if (choice.revision <= revision) continue;
    // A bulk choice uses the screen dimensions selected when the button was pressed.
    const mode = choice.state;
    const flags = imageTargetFlags({ width, height }, choice.width, choice.height);
    if (flags.fitsScreen) continue;
    const landscape = width > height;
    if (mode === 1 && !(landscape && flags.canHSplit)) continue;
    if (mode === 2 && !flags.canVSplit) continue;
    if (mode === 3 && !landscape) continue;
    state = mode;
    revision = choice.revision;
  }
  return state;
}

function applyStateToAll(state) {
  if (state === 0) {
    imageStates.clear();
    imageBatchStates.clear();
  } else {
    // At most one rule per mode and target, regardless of how many times a button is pressed.
    const key = `${state}:${MAX_WIDTH}:${MAX_HEIGHT}`;
    imageBatchStates.set(key, { state, revision: ++imageStateRevision, width: MAX_WIDTH, height: MAX_HEIGHT });
  }
  renderImageGrid();
}

function renderImageGrid() {
  const grid = document.getElementById('imageGrid');
  grid.replaceChildren();
  for (const image of epubImagesCache) {
    image.card = document.createElement('div');
    updateEpubPreviewCard(image);
    grid.appendChild(image.card);
  }
}

function updateEpubPreviewCard(image) {
  const card = image.card;
  card.className = 'image-item';
  card.onclick = null;
  const name = escapeHtml(image.name);
  const preview = ENABLE_AUTO_CROP && image.cropPreview;
  const src = preview?.dataUrl || image.dataUrl;
  if (!src) {
    card.classList.add('preview-placeholder');
    card.title = image.failed ? epubPickerText.failed : epubPickerText.loading;
    card.innerHTML = `<div class="preview-status">${escapeHtml(card.title)}</div><div class="image-name">${name}</div>`;
    return;
  }

  const locked = image.isCover || image.isSeparator;
  const state = getImageState(image.path, image, locked);
  const label = image.isCover ? epubPickerText.cover : image.isSeparator ? epubPickerText.separator : epubPickerText.states[state];
  const width = preview?.width || image.width;
  const height = preview?.height || image.height;
  const rotation = state === 1 || state === 3;
  let parts = 1;
  if (MAX_WIDTH && MAX_HEIGHT && (state === 1 || state === 2)) {
    const scaled = Math.max(1, Math.round(MAX_HEIGHT * (state === 1 ? height / width : width / height)));
    parts = epubSplitCount(scaled);
  }
  const lines = [];
  for (let i = 1; i < parts; i++) {
    lines.push(`<div class="split-line ${state === 1 ? 'split-h' : 'split-v'}" style="left:${100 * i / parts}%"></div>`);
  }
  const cropText = preview ? ` | ${epubPickerText.crop}: ${width}×${height}` : '';
  card.title = `${image.width}×${image.height} · ${label}${parts > 1 ? epubPickerText.parts(parts) : ''}${cropText}`;
  card.classList.add(image.isCover ? 'cover-locked' : image.isSeparator ? 'separator-locked' : `state-${state}`);
  if (rotation) card.classList.add(HANDEDNESS === 'right' ? 'rotate-cw' : 'rotate-ccw');
  if (preview) card.classList.add('crop-preview');
  if (!locked) {
    card.onclick = () => {
      const next = (getImageState(image.path, image, false) + 1) % 4;
      imageStates.set(image.path, { state: next, revision: ++imageStateRevision });
      updateEpubPreviewCard(image);
    };
  }
  const badge = image.isCover ? '🔒' : image.isSeparator ? '✦' : preview ? '✂' : state ? label : '•';
  card.innerHTML = `
    <span class="image-state-badge ${image.isCover ? 'cover-badge' : image.isSeparator ? 'separator-badge' : ''}">${escapeHtml(badge)}</span>
    <div class="image-preview-overlay"><div class="split-lines">${lines.join('')}</div></div>
    <img src="${src}" alt="${name}"><div class="image-name">${name}</div>
  `;
}

async function optimizeEpubForUpload(file, onProgress) {
  const controller = new AbortController();
  activeEpubConversion = controller;
  try {
    await ensureEpubTools();
    controller.signal.throwIfAborted();
    return await convertEpubFile(file, onProgress, controller.signal);
  } finally {
    activeEpubConversion = null;
  }
}
