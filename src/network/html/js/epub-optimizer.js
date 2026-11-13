// EPUB optimizer for the File Manager: repairs EPUB metadata and re-encodes,
// splits, crops and rotates images. FilesPage.html loads it on demand, after
// zip.js; it reads that page's top-level conversion settings (JPEG_QUALITY,
// MAX_WIDTH, HANDEDNESS, imageStates, ...) and log helpers.

// ═══════════════════════════════════════════════════════════════════
// EPUB Utilities — ported from EPUB Optimizer Pro
// MIT. Original code by zgredex. See licenses/borrowed-code.txt.
// ═══════════════════════════════════════════════════════════════════

const EPUB_IMAGE = /\.(png|gif|webp|bmp|jpe?g)$/i;
const EPUB_HTML = /\.(xhtml|html|htm)$/i;
const EPUB_MIME = 'application/epub+zip';

async function openEpub(file) {
  const reader = new zip.ZipReader(new zip.BlobReader(file));
  try {
    const entries = await reader.getEntries();
    const files = new Map(entries.filter(entry => !entry.directory).map(entry => [entry.filename, entry]));
    return { reader, files };
  } catch (error) {
    await reader.close();
    throw error;
  }
}

function readEpubEntry(entry, signal) {
  return entry.getData(new zip.BlobWriter(), { signal, checkSignature: true });
}

/** Defensive CSS injected into every XHTML <head> — prevents e-ink overflow. */
const DEFENSIVE_STYLE = '<style type="text/css">img,svg{max-width:100%;height:auto}body{overflow-wrap:break-word}table{max-width:100%;table-layout:fixed}pre,code{white-space:pre-wrap;word-wrap:break-word}*{box-sizing:border-box}</style>';

/** Escape a string for safe insertion into XML attribute values / text content. */
function xmlEscape(str) {
  return str.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}


/**
 * Decode a URI-encoded href (e.g., "my%20image.jpg" → "my image.jpg").
 * Handles double-encoding gracefully.
 */
function decodeHref(href) {
  try { return decodeURIComponent(href); }
  catch (e) { return href; }
}

/**
 * Safely read a text file from the zip, handling BOM and encoding.
 * Strips UTF-8 BOM. Detects encoding from XML declaration or meta tag.
 */
async function safeReadText(entry, signal) {
  const raw = new Uint8Array(await (await readEpubEntry(entry, signal)).arrayBuffer());

  // Detect and strip UTF-8 BOM (EF BB BF)
  let offset = 0;
  if (raw.length >= 3 && raw[0] === 0xEF && raw[1] === 0xBB && raw[2] === 0xBF) {
    offset = 3;
  }

  // Try UTF-8 first (vast majority of EPUBs)
  const utf8 = new TextDecoder('utf-8', { fatal: true });
  try {
    return utf8.decode(raw.subarray(offset));
  } catch (e) { /* not valid UTF-8 */ }

  // Peek at XML declaration or meta charset for encoding hint
  const ascii = new TextDecoder('ascii', { fatal: false }).decode(raw.subarray(offset, offset + 512));
  const encodingMatch = ascii.match(/encoding=["']([^"']+)["']/i) ||
                        ascii.match(/charset=["']?([^"'\s;]+)/i);
  const encoding = encodingMatch ? encodingMatch[1].toLowerCase() : 'windows-1252';

  try {
    return new TextDecoder(encoding, { fatal: false }).decode(raw.subarray(offset));
  } catch (e) {
    // Last resort: lossy latin1
    return new TextDecoder('iso-8859-1', { fatal: false }).decode(raw.subarray(offset));
  }
}

/**
 * Find the canonical OPF path by parsing META-INF/container.xml.
 * Falls back to scanning for any .opf file.
 */
async function findOPFPath(files, signal) {
  const containerPath = [...files.keys()].find(path => path.toLowerCase() === 'meta-inf/container.xml');
  if (containerPath) {
    const doc = new DOMParser().parseFromString(await safeReadText(files.get(containerPath), signal), 'application/xml');
    for (const rootfile of doc.getElementsByTagNameNS('*', 'rootfile')) {
      const path = rootfile.getAttribute('full-path');
      if (files.has(path)) return path;
    }
  }
  return [...files.keys()].find(path => (/\.opf$/i.test(path)));
}

function sanitizeMetadataFilenamePart(value) {
  const text = String(value || '').replace(/\s+/g, ' ').trim();
  return (text.normalize ? text.normalize('NFC') : text)
    .replace(/[<>:"/\\|?*\x00-\x1F]/g, ' ')
    .replace(/\s+/g, ' ')
    .replace(/^[. ]+/g, '')
    .replace(/[. ]+$/g, '')
    .trim();
}

function buildMetadataFilename(title, author) {
  title = sanitizeMetadataFilenamePart(title);
  author = sanitizeMetadataFilenamePart(author);
  if (!title) return '';
  let base = author ? `${title} - ${author}` : title;
  if (base.length > 180) base = base.substring(0, 180).replace(/\s+\S*$/g, '').trim() || base.substring(0, 180).trim();
  return `${base}.epub`;
}

async function getMetadataFilenameForEpub(file) {
  const { reader, files } = await openEpub(file);
  let doc;
  try {
    const opfPath = await findOPFPath(files);
    if (!opfPath) return '';
    doc = new DOMParser().parseFromString(await safeReadText(files.get(opfPath)), 'application/xml');
  } finally {
    await reader.close();
  }
  if (doc.querySelector('parsererror')) return '';

  const title = doc.getElementsByTagNameNS('*', 'title')[0]?.textContent || '';
  const creators = Array.from(doc.getElementsByTagNameNS('*', 'creator'));
  const getCreatorRole = el => (
    el.getAttribute('role') ||
    el.getAttribute('opf:role') ||
    el.getAttributeNS('http://www.idpf.org/2007/opf', 'role') ||
    ''
  ).toLowerCase();
  const authorEl = creators.find(el => getCreatorRole(el) === 'aut') ||
                   creators.find(el => !getCreatorRole(el));
  const authorText = authorEl?.textContent?.trim();
  const author = authorText ||
                 authorEl?.getAttribute('file-as') ||
                 authorEl?.getAttribute('opf:file-as') ||
                 authorEl?.getAttributeNS('http://www.idpf.org/2007/opf', 'file-as') ||
                 '';
  return buildMetadataFilename(title, author);
}

/**
 * Resolve a relative href against a base file path.
 * Handles multiple ../, ./, absolute /, and bare relative paths.
 */
function resolvePath(basePath, href) {
  if (href.startsWith('/')) return href.substring(1);
  href = href.replace(/^\.\//, '');
  const baseDir = basePath.includes('/') ? basePath.substring(0, basePath.lastIndexOf('/')) : '';
  const baseParts = baseDir ? baseDir.split('/') : [];
  const hrefParts = href.split('/');
  while (hrefParts.length > 0 && hrefParts[0] === '..') {
    hrefParts.shift();
    if (baseParts.length > 0) baseParts.pop();
  }
  const resolved = [...baseParts, ...hrefParts].join('/');
  return resolved.replace(/\/+/g, '/');
}

function resolveEpubReference(basePath, href) {
  const path = href.split(/[?#]/, 1)[0];
  if (!path || /^(?:[a-z][a-z0-9+.-]*:|\/\/)/i.test(path)) return '';
  return resolvePath(basePath, decodeHref(path));
}

function relativeEpubPath(basePath, path) {
  const base = basePath.split('/').slice(0, -1);
  const target = path.split('/');
  let shared = 0;
  while (shared < base.length && shared < target.length && base[shared] === target[shared]) shared++;
  return '../'.repeat(base.length - shared) + target.slice(shared).map(encodeURIComponent).join('/');
}

function epubPackageInfo(opfPath, opfContent) {
  const doc = new DOMParser().parseFromString(opfContent, 'application/xml');
  const items = [...doc.getElementsByTagNameNS('*', 'item')];
  const paths = new Map(items.map(item => [
    item.getAttribute('id'), resolveEpubReference(opfPath, item.getAttribute('href') || ''),
  ]));
  const spine = [...doc.getElementsByTagNameNS('*', 'itemref')].map(ref => paths.get(ref.getAttribute('idref')));
  const coverId = items.find(item => item.getAttribute('properties')?.split(/\s+/).includes('cover-image'))?.getAttribute('id')
    || [...doc.getElementsByTagNameNS('*', 'meta')].find(meta => meta.getAttribute('name') === 'cover')?.getAttribute('content');
  const coverRef = [...doc.getElementsByTagNameNS('*', 'reference')].find(ref => ref.getAttribute('type') === 'cover');
  const coverPage = coverRef ? resolveEpubReference(opfPath, coverRef.getAttribute('href') || '') : spine[0];
  return { coverPath: paths.get(coverId), coverPage, spine };
}

function epubPageImagePaths(path, source) {
  const doc = new DOMParser().parseFromString(source, 'application/xhtml+xml');
  if (doc.querySelector('parsererror')) {
    return [...source.matchAll(/(?:src|xlink:href)=["']([^"']+)["']/gi)]
      .map(match => resolveEpubReference(path, match[1]));
  }
  return [...doc.querySelectorAll('img, image')].map(node => resolveEpubReference(path,
    node.getAttribute('src') || node.getAttribute('xlink:href') || node.getAttribute('href') || ''));
}

async function findEpubCoverPaths(files, opfPath, opfContent, signal, info) {
  if (!opfPath) return new Set();
  const { coverPath, coverPage } = info || epubPackageInfo(opfPath, opfContent);
  const cover = files.get(coverPath);
  if (!cover) return new Set();
  const result = new Set([coverPath]);
  if (files.has(coverPage)) {
    const html = await safeReadText(files.get(coverPage), signal);
    for (const path of epubPageImagePaths(coverPage, html)) {
      const entry = files.get(path);
      // Compare ZIP checksums and sizes without decoding the cover images.
      if (entry?.signature === cover.signature && entry.uncompressedSize === cover.uncompressedSize) result.add(path);
    }
  }
  return result;
}

function isEpubCover(path, covers) {
  return covers.has(path) || (covers.size === 0 && /(^|\/)cover[^/]*$/i.test(path));
}

async function listEpubImages(files, signal) {
  const paths = [...files.keys()].filter(path => EPUB_IMAGE.test(path));
  const available = new Set(paths);
  const ordered = new Set();
  const opfPath = await findOPFPath(files, signal);
  let covers = new Set();
  if (opfPath) {
    const opf = await safeReadText(files.get(opfPath), signal);
    const info = epubPackageInfo(opfPath, opf);
    covers = await findEpubCoverPaths(files, opfPath, opf, signal, info);
    for (const path of info.spine) {
      signal.throwIfAborted();
      if (available.has(path)) ordered.add(path);
      if (!path || !files.has(path) || !EPUB_HTML.test(path)) continue;
      const source = await safeReadText(files.get(path), signal);
      for (const image of epubPageImagePaths(path, source)) {
        if (available.has(image)) ordered.add(image);
      }
    }
  }
  for (const path of ordered.size ? paths : paths.sort()) ordered.add(path);
  return [...ordered].map(path => ({ path, name: path.split('/').pop(), isCover: isEpubCover(path, covers) }));
}

/** Remove XML comments from XHTML source text. Returns {text, count, bytes}. */
function stripComments(xml) {
  const SKIPPED = [
    { open: '<![CDATA[', close: ']]>' },
    { open: '<?', close: '?>' },
  ];

  const encoder = new TextEncoder();
  const out = [];
  let i = 0;
  let count = 0;
  let bytes = 0;

  const doctypeEnd = (from) => {
    const gt = xml.indexOf('>', from);
    const subset = xml.indexOf('[', from);
    if (subset < 0 || (gt >= 0 && subset > gt)) return gt < 0 ? xml.length : gt + 1;
    const close = xml.indexOf(']>', subset);
    return close < 0 ? xml.length : close + 2;
  };

  while (i < xml.length) {
    const next = xml.indexOf('<', i);
    if (next < 0) { out.push(xml.slice(i)); break; }
    if (next > i) out.push(xml.slice(i, next));

    if (xml.startsWith('<!--', next)) {
      const close = xml.indexOf('-->', next + 4);
      const end = close < 0 ? xml.length : close + 3;
      count++;
      bytes += encoder.encode(xml.slice(next, end)).length;
      i = end;
      continue;
    }

    if (xml.startsWith('<!DOCTYPE', next)) {
      const end = doctypeEnd(next + 9);
      out.push(xml.slice(next, end));
      i = end;
      continue;
    }

    const skipped = SKIPPED.find(s => xml.startsWith(s.open, next));
    if (skipped) {
      const close = xml.indexOf(skipped.close, next + skipped.open.length);
      const end = close < 0 ? xml.length : close + skipped.close.length;
      out.push(xml.slice(next, end));
      i = end;
      continue;
    }

    out.push('<');
    i = next + 1;
  }

  return { text: out.join(''), count, bytes };
}

/**
 * Serialize an XML doc back to string, preserving the original <?xml?> declaration
 * and cleaning up XMLSerializer namespace prefix noise (xmlns:ns0 etc).
 */
function safeSerialize(doc, originalContent) {
  let result = new XMLSerializer().serializeToString(doc);

  // Restore <?xml?> declaration if original had one
  if (originalContent && /^\s*<\?xml\b/.test(originalContent) && !/^\s*<\?xml\b/.test(result)) {
    const declMatch = originalContent.match(/^\s*(<\?xml[^?]*\?>)/);
    if (declMatch) result = declMatch[1] + '\n' + result;
  }

  // Clean up XMLSerializer namespace prefix noise (xmlns:ns0="..." ns0:attr="...")
  result = result.replace(/ xmlns:ns\d+="[^"]*"/g, '');
  result = result.replace(/ ns\d+:/g, ' ');

  return result;
}

/**
 * Extract main identifier from OPF for NCX sync. DOMParser with regex fallback.
 */
function extractIdentifier(opfContent) {
  let mainIdentifier = null;
  try {
    const doc = new DOMParser().parseFromString(opfContent, 'application/xml');
    if (!doc.querySelector('parsererror')) {
      const pkg = doc.getElementsByTagNameNS('*', 'package')[0];
      const uid = pkg ? pkg.getAttribute('unique-identifier') : null;
      if (uid) {
        const el = [...doc.getElementsByTagNameNS('*', 'identifier')].find(e => e.getAttribute('id') === uid);
        if (el) mainIdentifier = (el.textContent || '').trim();
      }
      if (!mainIdentifier) {
        const el = doc.getElementsByTagNameNS('*', 'identifier')[0];
        if (el) mainIdentifier = (el.textContent || '').trim();
      }
    }
  } catch (e) { /* fall through to regex */ }
  if (!mainIdentifier) {
    const uniqueIdMatch = opfContent.match(/<(?:\w+:)?package[^>]*unique-identifier=["']([^"']+)["']/i);
    if (uniqueIdMatch) {
      const idRegex = new RegExp(`<dc:identifier[^>]*id=["']${uniqueIdMatch[1]}["'][^>]*>([^<]+)</dc:identifier>`, 'i');
      const idMatch = opfContent.match(idRegex);
      if (idMatch) mainIdentifier = idMatch[1].trim();
    }
    if (!mainIdentifier) {
      const firstIdMatch = opfContent.match(/<dc:identifier[^>]*>([^<]+)</i);
      if (firstIdMatch) mainIdentifier = firstIdMatch[1].trim();
    }
  }
  return mainIdentifier;
}

/**
 * Sync NCX dtb:uid with the given identifier. DOMParser with regex fallback.
 */
function syncNCXIdentifier(ncxText, mainIdentifier) {
  if (!mainIdentifier) return ncxText;
  let t = ncxText;
  try {
    const doc = new DOMParser().parseFromString(t, 'application/xml');
    if (!doc.querySelector('parsererror')) {
      const meta = [...doc.getElementsByTagNameNS('*', 'meta')].find(m => m.getAttribute('name') === 'dtb:uid');
      if (meta) {
        meta.setAttribute('content', mainIdentifier);
        t = safeSerialize(doc, ncxText);
      }
    }
  } catch (e) {
    t = t.replace(/<meta\s+name=["']dtb:uid["']\s+content=["'][^"']*["']\s*\/?>/gi, `<meta name="dtb:uid" content="${xmlEscape(mainIdentifier)}"/>`);
  }
  return t;
}

// ===== Embedded font removal =====
// CrossPoint renders with built-in / SD-card fonts and never loads fonts
// embedded in an EPUB, so they are dead weight (often 100KB-2MB per book).
const FONT_EXT_REGEX = /\.(ttf|otf|woff2?|eot)$/;
const FONT_MEDIA_TYPE_REGEX = /^(font\/|application\/font-|application\/x-font-|application\/vnd\.ms-(opentype|fontobject)$)/i;

// Returns a Set of zip-root-relative paths of embedded font files, detected by
// file extension and by OPF manifest media-type (catches odd extensions).
function collectFontPaths(files, opfContent, opfPath) {
  const fontPaths = new Set();
  for (const path of files.keys()) if (FONT_EXT_REGEX.test(path.toLowerCase())) fontPaths.add(path);

  if (opfContent && opfPath) {
    try {
      const doc = new DOMParser().parseFromString(opfContent, 'application/xml');
      if (!doc.querySelector('parsererror')) {
        for (const item of [...doc.getElementsByTagNameNS('*', 'item')]) {
          const type = item.getAttribute('media-type') || '';
          const href = item.getAttribute('href') || '';
          if (href && FONT_MEDIA_TYPE_REGEX.test(type)) {
            const resolved = resolvePath(opfPath, decodeHref(href));
            if (files.has(resolved)) fontPaths.add(resolved);
          }
        }
      }
    } catch (e) {
      // Extension-based detection already covers the common cases
    }
  }
  return fontPaths;
}

// Strips all @font-face rules from CSS. Rules can also embed fonts directly as
// multi-hundred-KB data: URIs, so this is a size win even without font files.
function stripFontFaceRules(css) {
  let count = 0;
  const stripped = css.replace(/@font-face(?:\s|\/\*[\s\S]*?\*\/)*\{[^{}]*\}/gi, () => { count++; return ''; });
  return { css: stripped, count };
}

// Removes font-obfuscation <EncryptedData> entries from META-INF/encryption.xml.
// Returns { dropFile: true } when nothing meaningful remains, { modified, xml }
// when some entries were removed, or { modified: false } to keep the file as-is.
function stripFontEncryptionEntries(xmlText, fontPaths) {
  try {
    const doc = new DOMParser().parseFromString(xmlText, 'application/xml');
    if (doc.querySelector('parsererror')) return { modified: false };
    const entries = [...doc.getElementsByTagNameNS('*', 'EncryptedData')];
    let removed = 0;
    for (const entry of entries) {
      const ref = entry.getElementsByTagNameNS('*', 'CipherReference')[0];
      const uri = ref ? decodeHref(ref.getAttribute('URI') || '') : '';
      if (fontPaths.has(uri.replace(/^\//, ''))) {
        entry.parentNode.removeChild(entry);
        removed++;
      }
    }
    if (!removed) return { modified: false };
    const remaining = doc.getElementsByTagNameNS('*', 'EncryptedData').length +
                      doc.getElementsByTagNameNS('*', 'EncryptedKey').length;
    if (remaining === 0) return { dropFile: true };
    return { modified: true, xml: safeSerialize(doc, xmlText) };
  } catch (e) {
    return { modified: false };
  }
}

function fixOPF(opfContent, opfPath, renamed, splitImages, fontPaths) {
  const doc = new DOMParser().parseFromString(opfContent, 'application/xml');
  const items = [...doc.getElementsByTagNameNS('*', 'item')];
  const manifest = doc.getElementsByTagNameNS('*', 'manifest')[0];
  const ids = new Set(items.map(item => item.getAttribute('id')));
  let nextId = 1;

  for (const item of items) {
    const path = resolveEpubReference(opfPath, item.getAttribute('href') || '');
    if (fontPaths.has(path)) {
      item.remove();
      continue;
    }

    const parts = splitImages[path];
    const outputPath = parts?.[0] || renamed[path];
    if (outputPath) {
      item.setAttribute('href', relativeEpubPath(opfPath, outputPath));
      item.setAttribute('media-type', 'image/jpeg');
    }

    const props = item.getAttribute('properties') || '';
    const updated = props.split(/\s+/).filter(prop => prop && prop !== 'svg').join(' ');
    if (updated) item.setAttribute('properties', updated);
    else if (props) item.removeAttribute('properties');

    if (!parts || !manifest) continue;
    for (const part of parts.slice(1)) {
      while (ids.has(`img-part-${nextId}`)) nextId++;
      const newItem = doc.createElementNS(manifest.namespaceURI, 'item');
      newItem.setAttribute('id', `img-part-${nextId++}`);
      newItem.setAttribute('href', relativeEpubPath(opfPath, part));
      newItem.setAttribute('media-type', 'image/jpeg');
      manifest.appendChild(newItem);
    }
  }

  const serialized = safeSerialize(doc, opfContent);
  return ensureCoverMeta(serialized).o;
}

// Fix SVG cover - converts SVG-wrapped covers to plain HTML img tags
function fixSvgCover(content) {
  const hasSvg = content.includes('<svg') || content.includes('<svg:');
  if (!hasSvg || !content.includes('xlink:href')) return { c: content, fixed: false, count: 0 };
  if (!content.includes('calibre:cover') && !content.includes('name="cover"') && !content.includes('<title>Cover</title>')) return { c: content, fixed: false, count: 0 };

  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(content, 'application/xhtml+xml');

    if (doc.querySelector('parsererror')) {
      // Fallback to regex
      const m = content.match(/xlink:href=["']([^"']+)["']/);
      if (!m) return { c: content, fixed: false, count: 0 };
      return { c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${m[1]}"/></section></body>
</html>`, fixed: true, count: 1 };
    }

    // Find SVG elements - check both standard and namespaced variants
    let imgHref = null;
    const svgNS = 'http://www.w3.org/2000/svg';
    const xlinkNS = 'http://www.w3.org/1999/xlink';

    // Try to find all SVG elements
    const svgs = [
      ...doc.getElementsByTagName('svg'),
      ...doc.getElementsByTagNameNS(svgNS, 'svg'),
      ...doc.getElementsByTagName('svg:svg')
    ];

    for (const svg of svgs) {
      // Find image element inside - try all variants
      const imageEl = svg.getElementsByTagName('image')[0] ||
                      svg.getElementsByTagNameNS(svgNS, 'image')[0] ||
                      svg.getElementsByTagName('svg:image')[0];

      if (imageEl) {
        imgHref = imageEl.getAttributeNS(xlinkNS, 'href') ||
                  imageEl.getAttribute('xlink:href') ||
                  imageEl.getAttribute('href');
        if (imgHref) break;
      }
    }

    if (!imgHref) {
      // Fallback to regex
      const m = content.match(/xlink:href=["']([^"']+)["']/);
      if (!m) return { c: content, fixed: false, count: 0 };
      imgHref = m[1];
    }

    return {
      c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${imgHref}"/></section></body>
</html>`,
      fixed: true,
      count: 1
    };
  } catch (e) {
    // Fallback to regex
    const m = content.match(/xlink:href=["']([^"']+)["']/);
    if (!m) return { c: content, fixed: false, count: 0 };
    return { c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${m[1]}"/></section></body>
</html>`, fixed: true, count: 1 };
  }
}

// Fix SVG-wrapped images - unwrap SVG and replace with plain img
function fixSvgWrappedImages(content) {
  const hasSvg = content.includes('<svg') || content.includes('<svg:');
  if (!hasSvg || !content.includes('xlink:href')) return { c: content, fixed: false, count: 0 };

  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(content, 'application/xhtml+xml');

    if (doc.querySelector('parsererror')) {
      // Fallback to regex
      let fixedCount = 0;
      const svgImageRegex = /<(?:svg:)?svg\b[^>]*>[\s\S]*?<(?:svg:)?image\b[^>]*xlink:href=["']([^"']+)["'][^>]*\/?>\s*<\/(?:svg:)?svg>/gi;
      const newContent = content.replace(svgImageRegex, (match, href) => { fixedCount++; return `<img style="max-width:100%;height:auto" src="${href}" alt="" />`; });
      return { c: newContent, fixed: fixedCount > 0, count: fixedCount };
    }

    const svgNS = 'http://www.w3.org/2000/svg';
    const xlinkNS = 'http://www.w3.org/1999/xlink';

    const svgElements = [...doc.querySelectorAll('svg'), ...doc.getElementsByTagNameNS(svgNS, 'svg')];
    const uniqueSvgs = [...new Set(svgElements)];
    let fixedCount = 0;

    for (const svg of uniqueSvgs) {
      const imageEl = svg.querySelector('image[*|href]') || svg.getElementsByTagNameNS(svgNS, 'image')[0] || svg.getElementsByTagNameNS('*', 'image')[0];
      if (!imageEl) continue;
      const href = imageEl.getAttributeNS(xlinkNS, 'href') || imageEl.getAttribute('xlink:href') || imageEl.getAttribute('href');
      if (!href) continue;
      const width = imageEl.getAttribute('width') || svg.getAttribute('width');
      const height = imageEl.getAttribute('height') || svg.getAttribute('height');
      const img = doc.createElementNS('http://www.w3.org/1999/xhtml', 'img');
      img.setAttribute('src', href);
      img.setAttribute('alt', '');
      img.setAttribute('style', 'max-width:100%;height:auto');
      if (width) img.setAttribute('width', width);
      if (height) img.setAttribute('height', height);
      svg.parentNode.replaceChild(img, svg);
      fixedCount++;
    }

    if (fixedCount === 0) return { c: content, fixed: false, count: 0 };
    return { c: safeSerialize(doc, content), fixed: true, count: fixedCount };

  } catch (e) {
    // Fallback to regex
    let fixedCount = 0;
    const svgImageRegex = /<(?:svg:)?svg\b[^>]*>[\s\S]*?<(?:svg:)?image\b[^>]*xlink:href=["']([^"']+)["'][^>]*\/?>\s*<\/(?:svg:)?svg>/gi;
    const newContent = content.replace(svgImageRegex, (match, href) => { fixedCount++; return `<img style="max-width:100%;height:auto" src="${href}" alt="" />`; });
    return { c: newContent, fixed: fixedCount > 0, count: fixedCount };
  }
}

// Ensure cover meta tag exists in OPF — DOMParser with regex fallback
function ensureCoverMeta(opfString) {
  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(opfString, 'application/xml');
    if (doc.querySelector('parsererror')) throw new Error('Parse failed');

    // Find cover image id: properties="cover-image", or id/href containing "cover"
    let coverId = null;
    const items = [...doc.getElementsByTagNameNS('*', 'item')];
    for (const item of items) {
      const props = item.getAttribute('properties') || '';
      const id = item.getAttribute('id') || '';
      const type = item.getAttribute('media-type') || '';
      if (!type.startsWith('image/')) continue;
      if (props.includes('cover-image')) { coverId = id; break; }
    }
    if (!coverId) {
      for (const item of items) {
        const id = item.getAttribute('id') || '';
        const href = item.getAttribute('href') || '';
        const type = item.getAttribute('media-type') || '';
        if (!type.startsWith('image/')) continue;
        if (id.toLowerCase().includes('cover') || href.toLowerCase().includes('cover')) { coverId = id; break; }
      }
    }
    if (!coverId) return { o: opfString, fixed: false };

    // Find or create <meta name="cover" content="..."/>
    const metas = [...doc.getElementsByTagNameNS('*', 'meta')];
    const coverMeta = metas.find(m => m.getAttribute('name') === 'cover');
    if (coverMeta) {
      if (coverMeta.getAttribute('content') === coverId) return { o: opfString, fixed: false };
      coverMeta.setAttribute('content', coverId);
    } else {
      const metadata = doc.getElementsByTagNameNS('*', 'metadata')[0];
      if (!metadata) return { o: opfString, fixed: false };
      const ns = metadata.namespaceURI || 'http://www.idpf.org/2007/opf';
      const newMeta = doc.createElementNS(ns, 'meta');
      newMeta.setAttribute('name', 'cover');
      newMeta.setAttribute('content', coverId);
      metadata.appendChild(newMeta);
    }
    return { o: safeSerialize(doc, opfString), fixed: true };
  } catch (e) {
    // Regex fallback
    return ensureCoverMetaRegex(opfString);
  }
}

function ensureCoverMetaRegex(o) {
  let coverId = null, m;
  if (!coverId && (m = o.match(/<\w+:?item[^>]+id="([^"]+)"[^>]+properties="[^"]*cover-image[^"]*"/i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]+properties="[^"]*cover-image[^"]*"[^>]+id="([^"]+)"/i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*id="([^"]+)"[^>]*href="[^"]*cover[^"]*"[^>]*media-type="image\//i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*href="[^"]*cover[^"]*"[^>]*id="([^"]+)"[^>]*media-type="image\//i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*id="([^"]*cover[^"]*)"[^>]*media-type="image\//i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*media-type="image\/[^"]*"[^>]*id="([^"]*cover[^"]*)"/i))) coverId = m[1];
  if (!coverId) return { o, fixed: false };
  const metaMatch = o.match(/<\w+:?meta\s+name=["']cover["']\s+content=["']([^"']+)["']/i) || o.match(/<\w+:?meta\s+content=["']([^"']+)["']\s+name=["']cover["']/i);
  if (metaMatch) {
    if (metaMatch[1] === coverId && !metaMatch[1].includes('/')) return { o, fixed: false };
    const esc = xmlEscape(coverId);
    o = o.replace(/<\w+:?meta\s+name=["']cover["']\s+content=["'][^"']+["']\s*\/?>/gi, `<meta name="cover" content="${esc}" />`);
    o = o.replace(/<\w+:?meta\s+content=["'][^"']+["']\s+name=["']cover["']\s*\/?>/gi, `<meta name="cover" content="${esc}" />`);
    return { o, fixed: true };
  }
  const idx = o.indexOf('</metadata>');
  if (idx !== -1) return { o: o.substring(0, idx) + `    <meta name="cover" content="${xmlEscape(coverId)}"/>\n  </metadata>` + o.substring(idx + 11), fixed: true };
  return { o, fixed: false };
}

// Auto-crop tuning.
const CROP_WHITE_THRESHOLD = 245;
const CROP_BACKGROUND_TOLERANCE = 28;
const CROP_BACKGROUND_MAX_SPREAD = 24;
const CROP_EDGE_SAMPLE_SIZE = 12;
const CROP_PADDING_PX = 8;
const MIN_CROP_SAVINGS_RATIO = 0.08;
const MIN_COLOR_CROP_SAVINGS_RATIO = 0.20;
const MIN_CROP_DIMENSION = 240;

// Apply grayscale to canvas image data
function applyGrayscale(ctx, width, height) {
  if (!ENABLE_GRAYSCALE) return;
  const imageData = ctx.getImageData(0, 0, width, height);
  const data = imageData.data;
  for (let i = 0; i < data.length; i += 4) {
    // Alpha-blend against white background before grayscaling (handles transparent PNGs)
    const a = data[i + 3] / 255;
    const blendedR = data[i] * a + 255 * (1 - a);
    const blendedG = data[i + 1] * a + 255 * (1 - a);
    const blendedB = data[i + 2] * a + 255 * (1 - a);
    const gray = Math.round(blendedR * 0.299 + blendedG * 0.587 + blendedB * 0.114);
    data[i] = gray; data[i + 1] = gray; data[i + 2] = gray; data[i + 3] = 255;
  }
  ctx.putImageData(imageData, 0, 0);
}

function shouldSkipAutoCrop(imagePath, width, height) {
  if (!ENABLE_AUTO_CROP) return true;
  if (width < MIN_CROP_DIMENSION || height < MIN_CROP_DIMENSION) return true;
  return /(^|\/)(cover|thumbnail|thumb|icon)[^/]*\.(jpe?g|png|gif|webp|bmp)$/i.test(imagePath || '');
}

function blendedPixel(data, i) {
  const alpha = data[i + 3] / 255;
  return {
    r: data[i] * alpha + 255 * (1 - alpha),
    g: data[i + 1] * alpha + 255 * (1 - alpha),
    b: data[i + 2] * alpha + 255 * (1 - alpha)
  };
}

function estimateCropBackground(data, width, height) {
  const sampleSize = Math.min(CROP_EDGE_SAMPLE_SIZE, Math.floor(width / 8), Math.floor(height / 8));
  if (sampleSize < 2) return null;

  // Use corners plus edge midpoints so colored margins work without trimming full-bleed art.
  const samples = [];
  const points = [
    [0, 0],
    [width - sampleSize, 0],
    [0, height - sampleSize],
    [width - sampleSize, height - sampleSize],
    [Math.floor((width - sampleSize) / 2), 0],
    [Math.floor((width - sampleSize) / 2), height - sampleSize],
    [0, Math.floor((height - sampleSize) / 2)],
    [width - sampleSize, Math.floor((height - sampleSize) / 2)]
  ];

  for (const [startX, startY] of points) {
    let r = 0, g = 0, b = 0, count = 0;
    for (let y = startY; y < startY + sampleSize; y++) {
      const row = y * width * 4;
      for (let x = startX; x < startX + sampleSize; x++) {
        const px = blendedPixel(data, row + x * 4);
        r += px.r; g += px.g; b += px.b; count++;
      }
    }
    samples.push({ r: r / count, g: g / count, b: b / count });
  }

  const avg = samples.reduce((sum, px) => ({
    r: sum.r + px.r,
    g: sum.g + px.g,
    b: sum.b + px.b
  }), { r: 0, g: 0, b: 0 });
  avg.r /= samples.length;
  avg.g /= samples.length;
  avg.b /= samples.length;

  const maxSpread = samples.reduce((spread, px) => Math.max(
    spread,
    Math.abs(px.r - avg.r),
    Math.abs(px.g - avg.g),
    Math.abs(px.b - avg.b)
  ), 0);

  if (maxSpread > CROP_BACKGROUND_MAX_SPREAD) return null;
  return avg;
}

function isCropContentPixel(px, background) {
  if (background) {
    return Math.abs(px.r - background.r) > CROP_BACKGROUND_TOLERANCE ||
           Math.abs(px.g - background.g) > CROP_BACKGROUND_TOLERANCE ||
           Math.abs(px.b - background.b) > CROP_BACKGROUND_TOLERANCE;
  }
  return px.r < CROP_WHITE_THRESHOLD || px.g < CROP_WHITE_THRESHOLD || px.b < CROP_WHITE_THRESHOLD;
}

function isNearWhiteBackground(background) {
  return background &&
         background.r >= CROP_WHITE_THRESHOLD &&
         background.g >= CROP_WHITE_THRESHOLD &&
         background.b >= CROP_WHITE_THRESHOLD;
}

function findNonWhiteBounds(ctx, width, height, padding = CROP_PADDING_PX) {
  const imageData = ctx.getImageData(0, 0, width, height);
  const data = imageData.data;
  const background = estimateCropBackground(data, width, height);
  let left = width, top = height, right = -1, bottom = -1;

  for (let y = 0; y < height; y++) {
    const row = y * width * 4;
    for (let x = 0; x < width; x++) {
      const i = row + x * 4;
      const px = blendedPixel(data, i);

      if (isCropContentPixel(px, background)) {
        if (x < left) left = x;
        if (x > right) right = x;
        if (y < top) top = y;
        if (y > bottom) bottom = y;
      }
    }
  }

  if (right < left || bottom < top) return null;

  left = Math.max(0, left - padding);
  top = Math.max(0, top - padding);
  right = Math.min(width - 1, right + padding);
  bottom = Math.min(height - 1, bottom + padding);

  const cropW = right - left + 1;
  const cropH = bottom - top + 1;
  const savedRatio = 1 - ((cropW * cropH) / (width * height));
  // Colored-background crops need a bigger win to avoid shaving illustrated pages.
  const minSavedRatio = background && !isNearWhiteBackground(background)
    ? MIN_COLOR_CROP_SAVINGS_RATIO
    : MIN_CROP_SAVINGS_RATIO;
  if (savedRatio < minSavedRatio) return null;

  return { x: left, y: top, width: cropW, height: cropH };
}

function createAutoCroppedCanvas(img, imagePath, width, height, locked) {
  if (locked || shouldSkipAutoCrop(imagePath, width, height)) return { canvas: img, width, height };
  // Build a white-backed source canvas, detect the uniform page/background color
  // from its edges, crop to pixels that differ from that background, then let the
  // normal resize/rotate/split pipeline fit that cropped content to the device.
  // If the image is a cover/icon/small asset, the edges are not uniform, or the
  // crop would save too little, return the original canvas unchanged.
  const sourceCanvas = document.createElement('canvas');
  sourceCanvas.width = width;
  sourceCanvas.height = height;
  const sourceCtx = sourceCanvas.getContext('2d');
  sourceCtx.fillStyle = '#FFF';
  sourceCtx.fillRect(0, 0, width, height);
  sourceCtx.drawImage(img, 0, 0);

  const crop = findNonWhiteBounds(sourceCtx, width, height);
  if (!crop) {
    return { canvas: sourceCanvas, width, height };
  }

  const croppedCanvas = document.createElement('canvas');
  croppedCanvas.width = crop.width;
  croppedCanvas.height = crop.height;
  const croppedCtx = croppedCanvas.getContext('2d');
  croppedCtx.fillStyle = '#FFF';
  croppedCtx.fillRect(0, 0, crop.width, crop.height);
  croppedCtx.drawImage(sourceCanvas, crop.x, crop.y, crop.width, crop.height, 0, 0, crop.width, crop.height);

  sourceCanvas.width = sourceCanvas.height = 0;
  return { canvas: croppedCanvas, width: crop.width, height: crop.height, crop };
}

const IMAGE_LOAD_TIMEOUT_MS = 30000;

function isEpubSeparator(img, path) {
  const width = img.naturalWidth, height = img.naturalHeight;
  if (width < 200 && height < 200) return true;
  if (width >= 150 && height >= 150) return false;
  if (/separator|divider|ornament|break|flourish|scene|divid|decor/i.test(path.split('/').pop())) return true;
  const ratio = width / height;
  if (ratio > 10 || ratio < 0.1 || width < 10 || height < 10) return true;
  const canvas = document.createElement('canvas');
  const scale = Math.min(1, 100 / Math.max(width, height));
  const w = canvas.width = Math.max(2, Math.round(width * scale));
  const h = canvas.height = Math.max(1, Math.round(height * scale));
  try {
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    ctx.drawImage(img, 0, 0, w, h);
    const { data } = ctx.getImageData(0, 0, w, h);
    const half = Math.floor(w / 2);
    let matches = 0;
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < half; x++) {
        const a = (y * w + x) * 4, b = (y * w + w - 1 - x) * 4;
        if (Math.abs(data[a] - data[b]) < 30 && Math.abs(data[a + 1] - data[b + 1]) < 30 &&
            Math.abs(data[a + 2] - data[b + 2]) < 30) matches++;
      }
    }
    return matches >= 0.85 * half * h;
  } finally {
    canvas.width = canvas.height = 0;
  }
}

function epubSplitCount(width) {
  if (width <= MAX_WIDTH) return 1;
  const overlap = Math.round(MAX_WIDTH * OVERLAP_PERCENT / 100);
  return Math.max(2, Math.ceil((width - overlap) / (MAX_WIDTH - overlap)));
}

function makeEpubPreview(img, image) {
  const width = img.naturalWidth, height = img.naturalHeight;
  const isSeparator = !image.isCover && isEpubSeparator(img, image.path);
  const canvas = document.createElement('canvas');
  const scale = Math.min(1, 220 / width, 160 / height);
  canvas.width = Math.max(1, Math.round(width * scale));
  canvas.height = Math.max(1, Math.round(height * scale));
  try {
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    ctx.fillStyle = '#fff';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
    const dataUrl = canvas.toDataURL('image/jpeg', 0.82);
    // Estimate the crop on the thumbnail. Conversion checks the original image.
    const crop = image.isCover || isSeparator || Math.min(width, height) < MIN_CROP_DIMENSION
      ? null : findNonWhiteBounds(ctx, canvas.width, canvas.height, Math.max(1, Math.round(CROP_PADDING_PX * scale)));
    let cropPreview;
    if (crop) {
      const thumb = document.createElement('canvas');
      thumb.width = crop.width;
      thumb.height = crop.height;
      thumb.getContext('2d').drawImage(canvas, crop.x, crop.y, crop.width, crop.height, 0, 0, crop.width, crop.height);
      cropPreview = {
        dataUrl: thumb.toDataURL('image/jpeg', 0.82),
        width: Math.round(crop.width * width / canvas.width),
        height: Math.round(crop.height * height / canvas.height),
      };
      thumb.width = thumb.height = 0;
    }
    return { width, height, isSeparator, dataUrl, cropPreview };
  } finally {
    canvas.width = canvas.height = 0;
  }
}

async function withEpubImage(blob, signal, useImage) {
  const url = URL.createObjectURL(blob);
  const img = new Image();
  let timer;
  let abort;
  try {
    await new Promise((resolve, reject) => {
      abort = () => reject(signal.reason);
      signal?.addEventListener('abort', abort, { once: true });
      if (signal?.aborted) return abort();
      timer = setTimeout(() => reject(new Error('Image load timeout')), IMAGE_LOAD_TIMEOUT_MS);
      img.onload = resolve;
      img.onerror = () => reject(new Error('Image load failed'));
      img.src = url;
    });
    signal?.throwIfAborted();
    return await useImage(img);
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener('abort', abort);
    img.onload = img.onerror = null;
    img.removeAttribute('src');
    URL.revokeObjectURL(url);
  }
}

function encodeEpubJpeg(canvas) {
  return new Promise((resolve, reject) => {
    canvas.toBlob(blob => blob ? resolve(blob) : reject(new Error('JPEG encoding failed')),
      'image/jpeg', JPEG_QUALITY / 100);
  });
}

async function processImage(img, origSize, imageState, imagePath, signal, locked) {
  const origW = img.naturalWidth, origH = img.naturalHeight;
  const source = createAutoCroppedCanvas(img, imagePath, origW, origH, locked);
  const sourceCanvas = source.canvas;
  const sourceW = source.width;
  const sourceH = source.height;
  const sourceWasCropped = !!source.crop;
  const canvases = [];
  if (sourceCanvas !== img) canvases.push(sourceCanvas);
  function createCanvas() {
    const canvas = document.createElement('canvas');
    canvases.push(canvas);
    return canvas;
  }

  try {
    // imageState: 0=Normal, 1=H-Split (CW/CCW), 2=V-Split, 3=Rotate & Fit
    // ========================================================================
    // STATE 1: H-Split (Rotate + Split) - EXACT COPY FROM index.html
    // Step 1: Scale WIDTH to 800px (keep aspect ratio)
    // Step 2: Rotate 90° CW or CCW based on HANDEDNESS
    // Step 3: If WIDTH > 480, split vertically with overlap
    // ========================================================================
    if (imageState === 1) {
      // Step 1: Scale WIDTH to 800 (this is the key difference!)
      const scale = MAX_HEIGHT / sourceW;  // 800 / sourceW
      const scaledW = MAX_HEIGHT;  // 800
      const scaledH = Math.max(1, Math.round(sourceH * scale));

      const scaledCanvas = createCanvas();
      scaledCanvas.width = scaledW;
      scaledCanvas.height = scaledH;
      const scaledCtx = scaledCanvas.getContext('2d');
      scaledCtx.imageSmoothingEnabled = true;
      scaledCtx.imageSmoothingQuality = 'high';
      scaledCtx.fillStyle = '#FFF';
      scaledCtx.fillRect(0, 0, scaledW, scaledH);
      scaledCtx.drawImage(sourceCanvas, 0, 0, sourceW, sourceH, 0, 0, scaledW, scaledH);

      // Step 2: Rotate 90° CW or CCW
      const rotW = scaledH;
      const rotH = scaledW;  // 800

      const rotCanvas = createCanvas();
      rotCanvas.width = rotW;
      rotCanvas.height = rotH;
      const rotCtx = rotCanvas.getContext('2d');
      rotCtx.fillStyle = '#FFF';
      rotCtx.fillRect(0, 0, rotW, rotH);

      const isClockwise = HANDEDNESS === 'right';
      if (isClockwise) {
        // Rotate 90° CW
        rotCtx.translate(rotW, 0);
        rotCtx.rotate(Math.PI / 2);
      } else {
        // Rotate 90° CCW
        rotCtx.translate(0, rotH);
        rotCtx.rotate(-Math.PI / 2);
      }
      rotCtx.drawImage(scaledCanvas, 0, 0);
      rotCtx.setTransform(1, 0, 0, 1, 0, 0); // Reset transform
      applyGrayscale(rotCtx, rotW, rotH);

      // Step 3: If WIDTH > 480, split vertically
      if (rotW <= MAX_WIDTH) {
        const blob = await encodeEpubJpeg(rotCanvas);
        return {
          parts: [{ data: blob, suffix: '', width: rotW, height: rotH, size: blob.size }],
          meta: { origW, origH, origSize, wasSplit: false, rotated: true, finalW: rotW, finalH: rotH, finalSize: blob.size, imageState: 1 }
        };
      } else {
        // Split by WIDTH (vertical cuts) - from RIGHT to LEFT for CW, LEFT to RIGHT for CCW
        const parts = [];
        const maxW = MAX_WIDTH;  // 480

        // Centered distribution: calculate numParts first, then distribute evenly
        let overlapPx, step;
        const minOverlapPx = Math.round(maxW * (OVERLAP_PERCENT / 100));  // Configurable overlap
        const numParts = epubSplitCount(rotW);
        // Now calculate step to distribute evenly
        step = Math.round((rotW - maxW) / (numParts - 1));
        overlapPx = maxW - step;
        // Ensure minimum overlap
        if (overlapPx < minOverlapPx) {
          overlapPx = minOverlapPx;
          step = maxW - overlapPx;
        }

        // Calculate all x positions first to ensure consistency
        const positions = [];
        for (let i = 0; i < numParts; i++) {
          let x;
          if (isClockwise) {
            // CW: right to left - start from right edge
            x = rotW - maxW - (i * step);
          } else {
            // CCW: left to right - start from left edge
            x = i * step;
          }
          // Clamp to valid range
          x = Math.max(0, Math.min(x, rotW - maxW));
          positions.push(x);
        }

        // Ensure first and last positions are at edges
        if (isClockwise) {
          positions[0] = rotW - maxW; // First part at right edge
          positions[numParts - 1] = 0; // Last part at left edge
        } else {
          positions[0] = 0; // First part at left edge
          positions[numParts - 1] = rotW - maxW; // Last part at right edge
        }

        for (let i = 0; i < numParts; i++) {
          signal.throwIfAborted();
          const x = positions[i];
          const partW = maxW; // Always full width for consistency

          const partCanvas = createCanvas();
          partCanvas.width = partW;
          partCanvas.height = rotH;
          const partCtx = partCanvas.getContext('2d');
          // Clear canvas first
          partCtx.fillStyle = '#FFFFFF';
          partCtx.fillRect(0, 0, partW, rotH);
          // Draw the slice
          partCtx.drawImage(rotCanvas, x, 0, partW, rotH, 0, 0, partW, rotH);

          const blob = await encodeEpubJpeg(partCanvas);
          partCanvas.width = partCanvas.height = 0;
          parts.push({ data: blob, suffix: `_part${i + 1}`, width: partW, height: rotH, size: blob.size });
        }

        const totalSize = parts.reduce((sum, p) => sum + p.size, 0);
        return {
          parts,
          meta: { origW, origH, origSize, wasSplit: true, splitCount: numParts, rotated: true, finalW: parts[0].width, finalH: parts[0].height, finalSize: totalSize, imageState: 1 }
        };
      }
    }
    // ========================================================================
    // STATE 2: V-Split (Vertical Split, no rotation)
    // Step 1: Scale HEIGHT to 800px (up or down)
    // Step 2: If WIDTH > 480, split vertically with overlap
    // ========================================================================
    else if (imageState === 2) {
      // ALWAYS scale height to 800 (up or down)
      const scale = MAX_HEIGHT / sourceH;  // 800 / sourceH
      const scaledW = Math.max(1, Math.round(sourceW * scale));
      const scaledH = MAX_HEIGHT;  // Always 800

      const scaledCanvas = createCanvas();
      scaledCanvas.width = scaledW;
      scaledCanvas.height = scaledH;
      const scaledCtx = scaledCanvas.getContext('2d');
      scaledCtx.imageSmoothingEnabled = true;
      scaledCtx.imageSmoothingQuality = 'high';
      scaledCtx.fillStyle = '#FFF';
      scaledCtx.fillRect(0, 0, scaledW, scaledH);
      scaledCtx.drawImage(sourceCanvas, 0, 0, sourceW, sourceH, 0, 0, scaledW, scaledH);
      applyGrayscale(scaledCtx, scaledW, scaledH);

      // Check if split needed
      if (scaledW <= MAX_WIDTH) {
        const blob = await encodeEpubJpeg(scaledCanvas);
        return {
          parts: [{ data: blob, suffix: '', width: scaledW, height: scaledH, size: blob.size }],
          meta: { origW, origH, origSize, wasSplit: false, rotated: false, finalW: scaledW, finalH: scaledH, finalSize: blob.size, imageState: 2 }
        };
      } else {
        // Split by WIDTH (vertical cuts) - LEFT to RIGHT (natural reading order)
        const parts = [];
        const maxW = MAX_WIDTH;

        // Centered distribution: calculate numParts first, then distribute evenly
        let overlapPx, step;
        const minOverlapPx = Math.round(maxW * (OVERLAP_PERCENT / 100));  // Configurable overlap
        const numParts = epubSplitCount(scaledW);
        // Now calculate step to distribute evenly
        step = Math.round((scaledW - maxW) / (numParts - 1));
        overlapPx = maxW - step;
        // Ensure minimum overlap
        if (overlapPx < minOverlapPx) {
          overlapPx = minOverlapPx;
          step = maxW - overlapPx;
        }

        // Calculate all x positions first to ensure consistency
        const positions = [];
        for (let i = 0; i < numParts; i++) {
          let x = i * step;
          // Clamp to valid range
          x = Math.max(0, Math.min(x, scaledW - maxW));
          positions.push(x);
        }
        // Ensure last position is at right edge
        positions[0] = 0;
        positions[numParts - 1] = scaledW - maxW;

        for (let i = 0; i < numParts; i++) {
          signal.throwIfAborted();
          const x = positions[i];
          const partW = maxW; // Always full width for consistency

          const partCanvas = createCanvas();
          partCanvas.width = partW;
          partCanvas.height = scaledH;
          const partCtx = partCanvas.getContext('2d');
          // Clear canvas first
          partCtx.fillStyle = '#FFFFFF';
          partCtx.fillRect(0, 0, partW, scaledH);
          // Draw the slice
          partCtx.drawImage(scaledCanvas, x, 0, partW, scaledH, 0, 0, partW, scaledH);

          const blob = await encodeEpubJpeg(partCanvas);
          partCanvas.width = partCanvas.height = 0;
          parts.push({ data: blob, suffix: `_part${i + 1}`, width: partW, height: scaledH, size: blob.size });
        }

        const totalSize = parts.reduce((sum, p) => sum + p.size, 0);
        return {
          parts,
          meta: { origW, origH, origSize, wasSplit: true, splitCount: numParts, rotated: false, finalW: parts[0].width, finalH: parts[0].height, finalSize: totalSize, imageState: 2 }
        };
      }
    }

    if (imageState === 3) {
      // Rotate while resizing so a large scan does not need a second full-size canvas.
      const fit = Math.min(MAX_WIDTH / sourceH, MAX_HEIGHT / sourceW);
      const scale = sourceWasCropped ? fit : Math.min(1, fit);
      const width = Math.max(1, Math.round(sourceH * scale));
      const height = Math.max(1, Math.round(sourceW * scale));
      const canvas = createCanvas();
      canvas.width = width;
      canvas.height = height;
      const ctx = canvas.getContext('2d');
      ctx.imageSmoothingQuality = 'high';
      ctx.fillStyle = '#fff';
      ctx.fillRect(0, 0, width, height);
      if (HANDEDNESS === 'right') {
        ctx.translate(width, 0);
        ctx.rotate(Math.PI / 2);
      } else {
        ctx.translate(0, height);
        ctx.rotate(-Math.PI / 2);
      }
      ctx.drawImage(sourceCanvas, 0, 0, height, width);
      applyGrayscale(ctx, width, height);
      const blob = await encodeEpubJpeg(canvas);
      return {
        parts: [{ data: blob, suffix: '', width, height, size: blob.size }],
        meta: { origW, origH, origSize, wasSplit: false, rotated: true, finalW: width, finalH: height, finalSize: blob.size, imageState: 3 }
      };
    }

    const fit = Math.min(MAX_WIDTH / sourceW, MAX_HEIGHT / sourceH);
    const scale = sourceWasCropped ? fit : Math.min(1, fit);
    const width = Math.max(1, Math.round(sourceW * scale));
    const height = Math.max(1, Math.round(sourceH * scale));
    const canvas = createCanvas();
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext('2d');
    ctx.imageSmoothingQuality = 'high';
    ctx.fillStyle = '#fff';
    ctx.fillRect(0, 0, width, height);
    ctx.drawImage(sourceCanvas, 0, 0, width, height);
    applyGrayscale(ctx, width, height);
    const blob = await encodeEpubJpeg(canvas);
    return {
      parts: [{ data: blob, suffix: '', width, height, size: blob.size }],
      meta: { origW, origH, origSize, wasSplit: false, rotated: false, finalW: width, finalH: height, finalSize: blob.size, imageState: 0 }
    };
  } finally {
    for (const canvas of canvases) canvas.width = canvas.height = 0;
  }
}

async function convertEpubFile(file, progressCallback, signal) {
  const { reader, files } = await openEpub(file);
  const destination = new zip.BlobWriter(EPUB_MIME);
  const out = new zip.ZipWriter(destination, { zip64: false, extendedTimestamp: false });
  try {
    return await writeOptimizedEpub(file, files, out, progressCallback, signal);
  } catch (error) {
    await destination.writable.abort(error).catch(() => {});
    throw error;
  } finally {
    await reader.close();
  }
}

async function writeOptimizedEpub(file, files, out, progressCallback, signal) {
  const startTime = Date.now();
  const originalSize = file.size;
  const text = epubOptimizerText;
  const encoder = new TextEncoder();
  const entries = [...files];
  const renamed = Object.create(null);
  const splitImages = Object.create(null);
  const reservedPaths = new Set(files.keys());
  const opfPath = await findOPFPath(files, signal);
  const opfContent = opfPath ? await safeReadText(files.get(opfPath), signal) : '';
  if (!opfPath || new DOMParser().parseFromString(opfContent, 'application/xml').querySelector('parsererror')) {
    throw new Error(text.invalidPackage);
  }
  const imageEntries = entries.filter(([path]) => EPUB_IMAGE.test(path));
  const htmlEntries = entries.filter(([path]) => EPUB_HTML.test(path));
  let completed = 0;
  function updateProgress() {
    progressCallback?.(++completed * 100 / (entries.length + 1));
  }
  function addEntry(path, content, stored = false, mimetype = false) {
    signal.throwIfAborted();
    const data = content instanceof Blob ? content : new Blob([content]);
    return out.add(path, new zip.BlobReader(data), {
      level: stored ? 0 : 6, signal, dataDescriptor: !mimetype, bufferedWrite: mimetype,
    });
  }

  clearLog();
  showLog();
  log(`<strong>${escapeHtml(file.name)}</strong> <span class="log-detail">(${formatBytes(originalSize)})</span>`, '', 'INFO');
  log(text.settings(), '', 'INFO');

  // OCF requires the first entry to be stored without a local extra field.
  await addEntry('mimetype', EPUB_MIME, true, true);
  if (files.has('mimetype')) updateProgress();

  // Resolve cover metadata even when the advanced picker was skipped.
  const coverPaths = await findEpubCoverPaths(files, opfPath, opfContent, signal);

  for (const [path, entry] of imageEntries) {
    signal.throwIfAborted();
    const data = await readEpubEntry(entry, signal);
    let result;
    try {
      result = await withEpubImage(data, signal, img => {
        const locked = isEpubCover(path, coverPaths) || isEpubSeparator(img, path);
        const imageState = getImageState(path, img, locked);
        return processImage(img, data.size, imageState, path, signal, locked);
      });
    } catch (error) {
      signal.throwIfAborted();
      // Keep the original name and media type if the browser cannot encode the page.
      console.warn('Could not process EPUB image:', path, error);
      log(text.imageFailed(escapeHtml(path.split('/').pop())), 'warning', 'IMG-ERR');
      conversionStats.errors++;
      await addEntry(path, data, true);
      updateProgress();
      continue;
    }

    const { parts, meta } = result;
    const name = path.split('/').pop();
    const directory = path.slice(0, path.length - name.length);
    const stem = name.replace(/\.[^.]+$/, '');
    const origFormat = name.split('.').pop();
    logImage(escapeHtml(name), meta.origW, meta.origH, origFormat, meta.origSize,
      meta.finalW, meta.finalH, meta.finalSize, meta.wasSplit, meta.splitCount || 0, parts, meta.imageState || 0);

    // Reserve archive paths up front so converting an image cannot overwrite another entry.
    const partPaths = parts.map(({ suffix }) => {
      let output = directory + stem + suffix + '.jpg';
      for (let n = 2; output !== path && reservedPaths.has(output); n++) {
        output = directory + stem + suffix + '-' + n + '.jpg';
      }
      reservedPaths.add(output);
      return output;
    });
    for (let i = 0; i < parts.length; i++) await addEntry(partPaths[i], parts[i].data, true);
    if (parts.length > 1) {
      splitImages[path] = partPaths;
    } else if (partPaths[0] !== path) {
      renamed[path] = partPaths[0];
    }
    updateProgress();
  }

  // Embedded fonts are dropped entirely — CrossPoint never renders with them.
  // Counters cover font files, @font-face rules (incl. data: URI fonts), and
  // encryption.xml shrinkage; they count uncompressed content bytes removed
  // (the final archive-size delta is reported separately by logSummary).
  const fontPaths = text.removeFonts ? collectFontPaths(files, opfContent, opfPath) : new Set();
  let removedFontCount = 0;
  let removedFontBytes = 0;
  let removedFontFaceRules = 0;

  // Second pass: update XHTML using DOMParser
  for (const [xhtmlPath, entry] of htmlEntries) {
    signal.throwIfAborted();
    const content = await safeReadText(entry, signal);
    let t = content;

    const stripped = stripComments(t);
    if (stripped.count) {
      t = stripped.text;
      logFix(text.comments(stripped.count, formatBytes(stripped.bytes)), escapeHtml(xhtmlPath.split('/').pop()));
    }
    const cover = fixSvgCover(t);
    if (cover.fixed) { t = cover.c; logFix(text.svgCover, escapeHtml(xhtmlPath.split('/').pop())); }
    const wrapped = fixSvgWrappedImages(t);
    if (wrapped.fixed) { t = wrapped.c; logFix(text.svgImages(wrapped.count), escapeHtml(xhtmlPath.split('/').pop())); }

    try {
      const doc = new DOMParser().parseFromString(t, 'application/xhtml+xml');
      if (!doc.querySelector('parsererror')) {
        let modified = false;

        // Remove width/height attributes from ALL img tags (dimensions may have changed)
        // This prevents CrossPoint and other readers from using wrong dimensions
        const allImgElements = doc.querySelectorAll('img');
        for (const img of allImgElements) {
          if (img.hasAttribute('width')) { img.removeAttribute('width'); modified = true; }
          if (img.hasAttribute('height')) { img.removeAttribute('height'); modified = true; }

          const src = img.getAttribute('src');
          if (!src) continue;
          const path = resolveEpubReference(xhtmlPath, src);
          const parts = splitImages[path];
          if (!parts && !renamed[path]) continue;
          modified = true;
          img.setAttribute('src', relativeEpubPath(xhtmlPath, parts ? parts[0] : renamed[path]));
          if (!parts) continue;

          img.removeAttribute('class');
          img.setAttribute('style', 'max-width:100%;height:auto');
          let container = img.parentElement;
          while (container.parentElement && !/^(div|p|figure|aside|section|body)$/i.test(container.localName)) {
            container = container.parentElement;
          }
          // Insert inside the body for chapters with bare image tags.
          const insideBody = container.localName === 'body';
          const parent = insideBody ? container : container.parentElement;
          const reference = insideBody ? img.nextSibling : container.nextSibling;
          if (!insideBody) {
            container.removeAttribute('class');
            container.removeAttribute('style');
          }
          for (const part of parts.slice(1)) {
            const wrapper = doc.createElementNS(img.namespaceURI, 'div');
            const next = doc.createElementNS(img.namespaceURI, 'img');
            next.setAttribute('src', relativeEpubPath(xhtmlPath, part));
            next.setAttribute('alt', '');
            next.setAttribute('style', 'max-width:100%;height:auto');
            wrapper.appendChild(next);
            parent.insertBefore(wrapper, reference);
          }
        }

        for (const image of doc.querySelectorAll('image')) {
          const attr = image.hasAttribute('xlink:href') ? 'xlink:href' : 'href';
          const path = resolveEpubReference(xhtmlPath, image.getAttribute(attr) || '');
          const target = renamed[path] || splitImages[path]?.[0];
          if (target) {
            image.setAttribute(attr, relativeEpubPath(xhtmlPath, target));
            modified = true;
          }
        }

        // Only serialize if we made changes
        if (modified) {
          t = safeSerialize(doc, content);
        }
      }
    } catch (e) {
      console.warn('DOMParser error for', xhtmlPath, e.message);
    }

    // Strip @font-face rules from inline <style> blocks — embedded font files
    // are removed, so leftover rules would reference missing resources
    if (text.removeFonts) t = t.replace(/(<style\b[^>]*>)([\s\S]*?)(<\/style>)/gi, (m, open, css, close) => {
      const ff = stripFontFaceRules(css);
      if (!ff.count) return m;
      removedFontFaceRules += ff.count;
      removedFontBytes += new TextEncoder().encode(css).length - new TextEncoder().encode(ff.css).length;
      logFix('@font-face', `${ff.count} rule(s) removed from ${escapeHtml(xhtmlPath.split('/').pop())}`);
      return open + ff.css + close;
    });

    // Inject universal image constraint — prevents overflow on e-ink displays
    if (t.includes('</head>')) {
      t = t.replace('</head>', DEFENSIVE_STYLE + '</head>');
    }

    await addEntry(xhtmlPath, t);
    updateProgress();
  }

  // Extract main identifier from OPF using DOMParser with regex fallback
  if (opfContent) {
    mainIdentifier = extractIdentifier(opfContent);
  }

  // Third pass: update OPF using fixOPF (DOMParser with regex fallback)
  if (opfContent) {
    const t = fixOPF(opfContent, opfPath, renamed, splitImages, fontPaths);
    if (t !== opfContent) logFix('OPF', text.manifestUpdated);
    await addEntry(opfPath, t);
    updateProgress();
  }

  // Copy other resources one entry at a time, rewriting only text that needs it.
  for (const [path, entry] of entries) {
    signal.throwIfAborted();
    if (path === 'mimetype' || path === opfPath || EPUB_IMAGE.test(path) || EPUB_HTML.test(path)) continue;
    updateProgress();

    if (fontPaths.has(path)) {
      removedFontCount++;
      removedFontBytes += entry.uncompressedSize;
      log(`${escapeHtml(path.split('/').pop())} <span class="log-detail">(${formatBytes(entry.uncompressedSize)})</span>`, 'success', 'REMOVE');
      continue;
    }

    const low = path.toLowerCase();
    let data;
    if (low.endsWith('.css')) {
      let t = await safeReadText(entry, signal);
      t = t.replace(/url\(\s*(["']?)([^"'()]+)\1\s*\)/gi, (original, quote, href) => {
        const ref = resolveEpubReference(path, href.trim());
        const target = renamed[ref] || splitImages[ref]?.[0];
        return target ? `url("${relativeEpubPath(path, target)}")` : original;
      });
      if (text.removeFonts) {
        const ff = stripFontFaceRules(t);
        if (ff.count) {
          removedFontFaceRules += ff.count;
          removedFontBytes += encoder.encode(t).length - encoder.encode(ff.css).length;
          t = ff.css;
          logFix('@font-face', `${ff.count} rule(s) removed from ${escapeHtml(path.split('/').pop())}`);
        }
      }
      data = t;
    } else if (low === 'meta-inf/encryption.xml' && fontPaths.size) {
      const t = await safeReadText(entry, signal);
      const enc = stripFontEncryptionEntries(t, fontPaths);
      if (enc.dropFile) {
        removedFontBytes += entry.uncompressedSize;
        logFix('encryption.xml', 'removed (contained only font obfuscation entries)');
        continue;
      }
      data = enc.modified ? enc.xml : t;
      if (enc.modified) {
        removedFontBytes += Math.max(0, entry.uncompressedSize - encoder.encode(data).length);
        logFix('encryption.xml', 'font obfuscation entries removed');
      }
    } else if (low.endsWith('.ncx')) {
      const t = await safeReadText(entry, signal);
      data = syncNCXIdentifier(t, mainIdentifier);
      if (t !== data) logFix(text.ncxId, text.ncxSynced);
    } else {
      data = await readEpubEntry(entry, signal);
    }
    await addEntry(path, data);
  }

  if (removedFontCount > 0 || removedFontFaceRules > 0) {
    log(`Removed ${removedFontCount} embedded font(s) and ${removedFontFaceRules} @font-face rule(s), ${formatBytes(removedFontBytes)} of font data`, '', 'INFO');
  }

  signal.throwIfAborted();
  const newBlob = await out.close();
  signal.throwIfAborted();
  progressCallback?.(100);
  const newSize = newBlob.size;
  const timeElapsed = (Date.now() - startTime) / 1000;

  // Log completion
  log(text.done, 'success', 'DONE');
  logSummary(originalSize, newSize, timeElapsed);

  // Auto-export only if NOT in batch mode (batch mode exports at the end)
  if (!isBatchMode && exportLogCheckbox && exportLogCheckbox.checked) {
    setTimeout(() => {
      exportLogToFile(null, false); // isBatch = false for single file
    }, 100);
  }

  return newBlob;
}
