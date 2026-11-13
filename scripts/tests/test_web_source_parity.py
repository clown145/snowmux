"""Run the production bilingual web scripts and build their actual gzip assets."""
import gzip
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HTML = ROOT / "src/network/html"


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def run_node(program):
    subprocess.run(["node"], input=program, text=True, check=True)


class WebSourceParityTest(unittest.TestCase):
    def test_opds_header_slots_preserve_change_clear_and_cancel(self):
        for locale in ["en", "zh-CN"]:
            with self.subTest(locale=locale):
                source = (HTML / locale / "SettingsPage.html").read_text()
                functions = "\n".join(function(source, name) for name in [
                    "function escapeHtml(", "function renderOpdsHeaderRows(",
                    "async function saveOpdsServer(", "async function deleteOpdsServer("])
                run_node(r'''
const assert = require('node:assert/strict');
const fields = Object.create(null);
const document = {getElementById: id => fields[id] || (fields[id] = {value: ''})};
const OPDS_MAX_CUSTOM_HEADERS = 2;
let requests = [], confirmResult = false;
const fetch = async (url, options) => { requests.push({url, body: JSON.parse(options.body)}); return {ok: true}; };
const showWebToast = () => {};
const showWebConfirm = async () => confirmResult;
const loadOpdsServers = async () => {};
''' + functions + r'''
(async () => {
  const markup = renderOpdsHeaderRows({headers:[{name:'X-Access',hasValue:true},{name:'X-Other',hasValue:false}]}, 2);
  assert(markup.includes('type="password"'));
  assert(markup.includes('opds-header0-name-2') && markup.includes('opds-header1-value-2'));
  assert(!markup.includes('secret'));
  const set = (id, value) => document.getElementById(id).value = value;
  set('opds-name-2','Library'); set('opds-url-2','https://example.test'); set('opds-user-2','reader');
  set('opds-header0-name-2',' X-Access '); set('opds-header0-value-2','');
  set('opds-header1-name-2',''); set('opds-header1-value-2','');
  await saveOpdsServer(2);
  assert.deepEqual(requests[0].body.headers,[{name:'X-Access'},{name:'',value:''}]);
  assert.equal(requests[0].body.index,2); assert(!('password' in requests[0].body));
  set('opds-header0-value-2','new-secret'); set('opds-header1-name-2','X-Other');
  await saveOpdsServer(2);
  assert.deepEqual(requests[1].body.headers,[{name:'X-Access',value:'new-secret'},{name:'X-Other'}]);
  requests=[]; await deleteOpdsServer(2); assert.equal(requests.length,0);
  confirmResult=true; await deleteOpdsServer(2); assert.equal(requests.length,1);
  assert.equal(requests[0].url,'/api/opds/delete');
})().catch(error => {console.error(error); process.exitCode=1;});
''')
                if locale == "zh-CN":
                    self.assertIn(">请求头 ", source)
                    self.assertIn(" 名称</span>", source)
                    self.assertIn(" 值</span>", source)
                    self.assertIn("（保持不变）", source)

    def test_localized_dialog_queue_cancel_confirm_and_toast(self):
        for locale, suffix, title, confirm, notice in [
            ("en", "", "Confirm action", "Confirm", "Notice"),
            ("zh-CN", ".zh-CN", "确认操作", "确认", "提示")
        ]:
            with self.subTest(locale=locale):
                footer = (HTML / "shared" / ("Footer"+suffix+".html.inc")).read_text()
                script = re.search(r"<script>(.*?)</script>", footer, re.S).group(1)
                run_node(r'''
const assert = require('node:assert/strict');
const elements = Object.create(null);
const document = {getElementById: id => elements[id] || (elements[id]={
  textContent:'',style:{}, listeners:[], returnValue:'', opens:0,
  addEventListener(type, fn) {this.listeners.push(fn);},
  showModal() {this.opens++;},
  close(value) {this.returnValue=value;this.listeners.shift()();}
})};
let timers=0, cleared=[];
const setTimeout=()=>++timers, clearTimeout=id=>cleared.push(id);
''' + script + "\nconst expected="+repr([title, confirm, notice]).replace("'", '"')+";\n"+r'''
(async () => {
  const first=showWebConfirm('book?'), second=showWebConfirm('next?');
  await Promise.resolve(); await Promise.resolve();
  assert.equal(webMessageDialog.opens,1);
  assert.equal(webDialogTitle.textContent,expected[0]);
  assert.equal(webDialogConfirm.textContent,expected[1]);
  assert.equal(webDialogCancel.hidden,false);
  webMessageDialog.close(''); assert.equal(await first,false);
  await Promise.resolve(); await Promise.resolve();
  assert.equal(webMessageDialog.opens,2);
  webMessageDialog.close('confirm'); assert.equal(await second,true);
  const alert=showWebAlert('message');
  await Promise.resolve(); await Promise.resolve();
  assert.equal(webDialogTitle.textContent,expected[2]);assert.equal(webDialogCancel.hidden,true);
  webMessageDialog.close('confirm');await alert;
  showWebToast('one',false); showWebToast('two',true);
  assert.equal(webToast.textContent,'two');assert.equal(webToast.className,'toast error');
  assert.equal(cleared.at(-1),1);
})().catch(error=>{console.error(error);process.exitCode=1;});
''')

    def test_file_dialog_cancellation_cleanup_and_nfc_names(self):
        for locale in ["en", "zh-CN"]:
            with self.subTest(locale=locale):
                source = (HTML / locale / "FilesPage.html").read_text()
                for name in ["uploadModal","folderModal","deleteModal","renameModal","moveModal","imagePreviewModal"]:
                    self.assertIn('<dialog class="modal-overlay" id="'+name+'"',source)
                    self.assertIn('id="'+name+'Title"',source)
                self.assertNotRegex(source, r"\balert\(")
                self.assertIn("modal.addEventListener('cancel'",source)
                self.assertIn("event.preventDefault()",source)
                picker = (HTML / "shared/EpubImagePicker.js.inc").read_text(encoding="utf-8")
                functions="\n".join(function(source, name) for name in [
                    "function openWebModal(", "function closeWebModal(", "function closeUploadModal(",
                    "function handleCancelUploadModal(", "function reserveAvailableUploadFilename("])
                run_node(r'''
const assert=require('node:assert/strict');
const elements=Object.create(null);
const document={getElementById:id=>elements[id]||(elements[id]={
  open:false,style:{},value:'pending',classList:{remove(){},add(){}},
  showModal(){this.open=true;},close(){this.open=false;},replaceChildren(){}
}),querySelector:()=>({classList:{remove(){}}})};
let isUploadInProgress=false,operationCancelled=false,currentUploadWs=null,currentUploadXhr=null;
let logSection=null;
const applyUploadSettings=()=>{};
''' + picker + functions + r'''
const modal=document.getElementById('uploadModal');
openWebModal('uploadModal');assert(modal.open);
let closed=0,aborted=0;
isUploadInProgress=true;
activeEpubConversion=new AbortController();
epubImagesCache=['old'];
imageStates.set('old', {state: 1, revision: 1});
currentUploadWs={close(){closed++;}};currentUploadXhr={abort(){aborted++;}};
closeUploadModal();assert(modal.open);assert.equal(closed,0);
handleCancelUploadModal();assert(modal.open);assert(operationCancelled);
assert.equal(closed,1);assert.equal(aborted,1);
assert(activeEpubConversion.signal.aborted);
isUploadInProgress=false;handleCancelUploadModal();assert(!modal.open);
assert.equal(document.getElementById('fileInput').value,'');assert.deepEqual(epubImagesCache,[]);
assert.equal(imageStates.size,0);
const names=new Set(['café.epub']);
assert.equal(reserveAvailableUploadFilename('cafe\u0301.epub',names),'café (2).epub');
assert.equal(reserveAvailableUploadFilename('another.epub',names),'another.epub');
''')

    def test_epub_target_waits_for_connected_screen_and_honors_manual_choice(self):
        profiles = (HTML / 'shared/FileTransferProfiles.js.inc').read_text(encoding='utf-8')
        run_node(r'''
const assert = require('node:assert/strict');
const XUESU_DEVICE_LABEL = 'Xuesu 3';
const summary = {dataset: {maxLabel: 'Max', unknown: 'No screen size'}};
const auto = {dataset: {label: 'Auto'}};
const document = {
  getElementById: id => ({convertSizeSummary: summary, deviceAutoLabel: auto})[id],
  querySelectorAll: () => [],
};
const image = {width: 390, height: 500};
const epubImagesCache = [image];
const isImagePickerVisible = () => true;
const renderImageGrid = () => {};
let isUploadInProgress = false;
const updateUploadSettingsPersistence = () => {};
let respond, requests = 0;
const fetch = url => {
  assert.equal(url, '/api/status');
  ++requests;
  return new Promise(resolve => { respond = data => resolve({ok: true, json: async () => data}); });
};
''' + profiles + r'''
(async () => {
  const initial = fetchVersion();
  const upload = ensureDeviceTarget();
  assert.equal(requests, 1);
  assert.equal(MAX_WIDTH, 0);
  respond({device: 'xuesu-300-std', screenWidth: 300, screenHeight: 400});
  await initial;
  assert.equal(await upload, true);
  assert.equal(auto.textContent, 'Auto (Xuesu 3)');
  assert.equal(summary.textContent, 'Max 300×400 px');
  assert.deepEqual([image.fitsScreen, image.canHSplit, image.canVSplit], [false, false, true]);

  setDeviceTarget('X4');
  assert.equal(summary.textContent, 'Max 480×800 px');
  assert.deepEqual([image.fitsScreen, image.canHSplit, image.canVSplit], [true, false, false]);
  const refresh = fetchVersion();
  respond({device: 'xuesu-300-std'});
  await refresh;
  assert.equal(summary.textContent, 'Max 480×800 px');
  setDeviceTarget('auto');
  assert.equal(summary.textContent, 'Max 300×400 px');

  const unknown = fetchVersion();
  respond({device: 'unknown-board'});
  await unknown;
  assert.equal(summary.textContent, 'No screen size');
  const retry = ensureDeviceTarget();
  respond({device: 'unknown-board'});
  assert.equal(await retry, false);
  setDeviceTarget('Xuesu');
  assert.equal(await ensureDeviceTarget(), true);
  assert.equal(summary.textContent, 'Max 300×400 px');
})().catch(error => { console.error(error); process.exitCode = 1; });
''')

    def test_epub_preview_reads_one_page_and_keeps_whole_book_choices(self):
        picker = (HTML / "shared/EpubImagePicker.js.inc").read_text(encoding="utf-8")
        profiles = (HTML / "shared/FileTransferProfiles.js.inc").read_text(encoding="utf-8")
        run_node(r'''
const assert = require('node:assert/strict');
const elements = new Map();
const makeElement = () => ({
  style: {}, classList: {add() {}, remove() {}}, children: [],
  replaceChildren() { this.children = []; },
  appendChild(child) { this.children.push(child); },
});
const document = {
  getElementById(id) {
    if (!elements.has(id)) elements.set(id, makeElement());
    return elements.get(id);
  },
  querySelector: makeElement,
  createElement: makeElement,
};
const epubPickerText = {
  loading: '', empty: '', failed: '', range: () => '', states: ['', '', '', ''],
  cover: '', separator: '', parts: () => '',
};
const XUESU_DEVICE_LABEL = 'Xuesu 3';
const ENABLE_AUTO_CROP = false, HANDEDNESS = 'right';
const escapeHtml = value => value;
const epubSplitCount = () => 2;
const paths = Array.from({length: 53}, (_, index) => ({path: String(index), name: String(index), isCover: index === 0}));
let closes = 0, reads = [], pause = false, release;
const openEpub = async () => ({
  files: new Map(paths.map(image => [image.path, image.path])),
  reader: {async close() { closes++; }},
});
const listEpubImages = async () => paths;
const readEpubEntry = async path => {
  reads.push(path);
  if (pause) await new Promise(resolve => { release = resolve; });
};
const withEpubImage = async (data, signal, callback) => {
  signal.throwIfAborted();
  return callback({});
};
const makeEpubPreview = () => ({ width: 800, height: 500, dataUrl: 'data:image/jpeg;base64,preview' });
''' + picker + profiles + r'''
(async () => {
  MAX_WIDTH = 300; MAX_HEIGHT = 400;
  ensureDeviceTarget = async () => true;
  ensureEpubTools = async () => {};
  await showImagePicker({});
  assert.deepEqual(reads, paths.slice(0,24).map(image => image.path));
  assert.equal(epubImagesCache.length,24);
  epubImagesCache[1].card.onclick();
  assert.equal(getImageState('1', epubImagesCache[1], false),1);
  applyStateToAll(2);
  assert.equal(getImageState('52', {width:800,height:500}, false),2);
  assert.equal(getImageState('0', {width:800,height:500}, true),0);
  await showEpubPreviewPage(2);
  assert.deepEqual(reads.slice(24),['48','49','50','51','52']);
  assert.equal(epubImagesCache.length,5);
  assert(document.getElementById('epubPreviewNext').disabled);
  assert.equal(getImageState('1', {width:800,height:500}, false),2);
  MAX_WIDTH=1000; MAX_HEIGHT=1000;
  assert.equal(getImageState('52', {width:800,height:500}, false),2);
  pause = true;
  const pending = showEpubPreviewPage(1);
  await Promise.resolve();
  clearImagePicker();
  release();
  await pending;
  assert.equal(epubImagesCache.length,0);
  assert.equal(document.getElementById('imageGrid').children.length,0);
  assert.equal(getImageState('52', {width:800,height:500}, false),0);
  assert.equal(closes,4);
})().catch(error => {console.error(error); process.exitCode=1;});
''')

    def test_xmpic_preview_preserves_orientation_download_and_cancellation(self):
        preview = (HTML / 'shared/FileImagePreview.js.inc').read_text(encoding='utf-8')
        run_node(r'''
const assert = require('node:assert/strict');
const img = {removeAttribute() { delete this.src; }, dataset: {loadError: 'Invalid wallpaper'}};
const download = {}, title = {};
let rgba, toasts = [], resolve;
const document = {
  getElementById: id => ({imagePreviewImg: img, imagePreviewName: title, imagePreviewDownload: download})[id],
  createElement(type) {
    assert.equal(type, 'canvas');
    return {getContext: () => ({
      createImageData(w, h) {
        assert.deepEqual([w, h], [300, 400]);
        return {data: new Uint8ClampedArray(w * h * 4)};
      },
      putImageData(image) { rgba = image.data; },
    }), toDataURL: () => 'data:image/png;base64,preview'};
  },
};
const openWebModal = () => {}, closeWebModal = () => {};
const showWebToast = (...args) => toasts.push(args);
const fetch = () => new Promise(r => { resolve = r; });
const reply = (bytes, length = bytes.length) => resolve({ok: true,
  headers: {get: () => String(length)}, arrayBuffer: async () => bytes.buffer});
''' + preview + r'''
(async () => {
  const bytes = new Uint8Array(38 * 400);
  bytes[0] = 0x80;
  bytes[38 * 399 + 37] = 0x10;
  assert(isImageFile('Wall.XMPIC'));
  const ready = openImagePreview('/download?path=Wall.XMPIC', 'Wall.XMPIC');
  reply(bytes);
  await ready;
  assert.equal(title.textContent, 'Wall.XMPIC');
  assert.equal(download.href, '/download?path=Wall.XMPIC');
  assert.equal(img.src, 'data:image/png;base64,preview');
  const pixel = (x, y) => [...rgba.slice((y * 300 + x) * 4, (y * 300 + x) * 4 + 4)];
  assert.deepEqual(pixel(0, 0), [0, 0, 0, 255]);
  assert.deepEqual(pixel(299, 399), [0, 0, 0, 255]);
  assert.deepEqual(pixel(299, 0), [255, 255, 255, 255]);
  assert.deepEqual(pixel(0, 399), [255, 255, 255, 255]);

  const bad = openImagePreview('/bad', 'bad.xmpic');
  reply(bytes.subarray(0, 20));
  await bad;
  assert.deepEqual(toasts, [['Invalid wallpaper', true]]);
  assert.equal(download.href, '/bad');
  assert.equal(img.src, undefined);

  const stale = openImagePreview('/stale', 'stale.xmpic');
  closeImagePreview();
  await openImagePreview('/new', 'new.png');
  reply(bytes);
  await stale;
  assert.equal(img.src, '/new');
})().catch(error => { console.error(error); process.exitCode = 1; });
''')

    def test_font_upload_groups_styles_and_reports_confirmed_progress(self):
        names = (HTML / "shared/FontUploadNames.js.inc").read_text(encoding="utf-8")
        request = (HTML / "shared/FontUploadRequest.js.inc").read_text(encoding="utf-8")
        for locale in ["en", "zh-CN"]:
            with self.subTest(locale=locale):
                source = (HTML / locale / "FontsPage.html").read_text(encoding="utf-8")
                file_manager = (HTML / locale / "FilesPage.html").read_text(encoding="utf-8")
                self.assertIn('href="/fonts#upload-fonts"', file_manager)
                self.assertIn('id="upload-fonts"', source)
                self.assertNotIn("webkitdirectory", re.search(r'<input[^>]+id="fontFiles"[^>]+>', source).group())
                self.assertNotIn('id="fontFolder"', source)
                self.assertIn('<progress id="fontUploadBar" max="100"', source)
                upload = source[source.index("    const fontFiles ="):source.rindex("    loadFonts();")]
                run_node(r'''
const assert = require('node:assert/strict');
const elements = Object.create(null);
const document = {getElementById: id => elements[id] ||= {
  events: {}, files: [], disabled: false, textContent: '', dataset: {},
  addEventListener(event, fn) { this.events[event] = fn; },
  get value() { return this.currentValue; },
  set value(value) { this.currentValue = value; if (value === '') this.files = []; },
}};
const requests = [];
class FormData {
  data = {};
  append(key, value, name) { this.data[key] = name ?? value; }
}
class XMLHttpRequest {
  upload = {};
  open(method, url) { this.method = method; this.url = url; }
  send(body) { this.data = body.data; requests.push(this); }
  progress(loaded, total) { this.upload.onprogress({loaded, total, lengthComputable: true}); }
  reply(status, ok) { this.status = status; this.response = {ok}; this.onload(); }
}
let allowVector = true, refreshes = 0;
const showWebToast = () => {};
const loadFonts = async () => { refreshes++; };
''' + function(source, "function fontExtRe(") + "\n" +
                         function(source, "function supportedExts(") + "\n" + names + request + upload + r'''
(async () => {
  assert.deepEqual(fontUploadNames('My-Reading-Font.ttf'),
    {family: 'My-Reading-Font', filename: 'My-Reading-Font.ttf'});
  assert.deepEqual(fontUploadNames('Bookerly-BoldItalic.TTF'),
    {family: 'Bookerly', filename: 'Bookerly-BoldItalic.ttf'});
  assert.deepEqual(fontUploadNames('\u9605\u8bfb 14.CPFONT'),
    {family: '\u9605\u8bfb', filename: '\u9605\u8bfb_14.cpfont'});
  assert.equal(fontUploadNames('Broken.cpfont'), null);
  const submit = () => elements.uploadForm.events.submit({preventDefault() {}});
  fontFiles.files = [{name:'\u6977\u4f53.ttc', size: 100}];
  fontFiles.events.change();
  assert.equal(uploadFonts.disabled, false);
  const rejected = submit();
  assert.equal(fontUploadProgress.hidden, false);
  assert.equal(fontUploadBar.value, 0);
  assert.equal(fontFiles.disabled, true);
  requests[0].progress(25, 100);
  assert.equal(fontUploadPercent.textContent, '25%');
  const sending = fontUploadStatus.textContent;
  requests[0].upload.onload();
  assert.equal(fontUploadBar.value, 99);
  assert.notEqual(fontUploadStatus.textContent, sending);
  requests[0].reply(400, false);
  await rejected;
  assert.equal(fontUploadProgress.dataset.failed, 'true');
  assert.equal(fontUploadBar.value, 99);
  assert.equal(uploadFonts.disabled, false);

  const retry = submit();
  assert.equal(fontUploadBar.value, 0);
  assert.equal(fontUploadProgress.dataset.failed, 'false');
  assert.equal(requests[1].method, 'POST');
  assert.equal(requests[1].url, '/api/fonts/upload');
  assert.deepEqual(requests[1].data, {family:'\u6977\u4f53', file:'\u6977\u4f53.ttc'});
  requests[1].upload.onload();
  assert.equal(fontUploadBar.value, 99);
  requests[1].reply(200, true);
  await retry;
  assert.equal(fontUploadBar.value, 100);
  assert.equal(uploadFonts.disabled, true);

  fontFiles.files = [{name:'Bookerly-Regular.ttf', size: 100}, {name:'Bookerly-Bold.ttf', size: 300}];
  fontFiles.events.change();
  assert.equal(fontUploadProgress.hidden, true);
  const batch = submit();
  requests[2].progress(50, 100);
  assert.equal(fontUploadBar.value, 12);
  requests[2].upload.onload();
  assert.equal(fontUploadBar.value, 25);
  requests[2].reply(200, true);
  await new Promise(setImmediate);
  requests[3].progress(50, 100);
  assert.equal(fontUploadBar.value, 62);
  requests[3].upload.onload();
  assert.equal(fontUploadBar.value, 99);
  requests[3].reply(200, true);
  await batch;
  assert.equal(fontUploadBar.value, 100);
  assert.deepEqual(requests.slice(2).map(({data}) => data.family), ['Bookerly', 'Bookerly']);
  assert.equal(refreshes, 3);
  allowVector = false;
  fontFiles.files = [{name:'Bookerly.ttf'}];
  fontFiles.events.change();
  assert.equal(uploadFonts.disabled, true);
})().catch(error => {console.error(error); process.exitCode = 1;});
''')

    def test_build_assets_include_localized_helpers_and_valid_scripts(self):
        with tempfile.TemporaryDirectory(prefix="web-source-parity-") as directory:
            temp=Path(directory)
            target=temp/"src/network/html"
            shutil.copytree(HTML,target,ignore=shutil.ignore_patterns("*.generated.h"))
            result=subprocess.run([sys.executable,str(ROOT/"scripts/build_html.py")],cwd=temp,text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            for locale in ["en","zh-CN"]:
                for name in ["FilesPage","FontsPage","HomePage","SettingsPage"]:
                    header=(target/locale/(name+"Html.generated.h")).read_text()
                    array=header[header.index("{")+1:header.index("};")]
                    raw=gzip.decompress(bytes(int(token,16) for token in re.findall(r"0x([0-9a-f]{2})",array))).decode()
                    self.assertNotIn("<!--#include",raw)
                    self.assertIn("function showWebConfirm(",raw)
                    if name == "FilesPage":
                        self.assertIn("function xmpicPreviewUrl(",raw)
                        self.assertIn('href="/fonts#upload-fonts"', raw)
                    if name == "FontsPage":
                        self.assertIn("function fontUploadNames(", raw)
                        self.assertIn("xhr.upload.onprogress", raw)
                    if locale=="zh-CN":
                        self.assertIn("确认操作",raw)
                        self.assertNotIn("'Confirm action'",raw)
                    scripts=re.findall(r"<script(?:\s[^>]*)?>(.*?)</script>",raw,re.S)
                    for script in scripts:
                        checked=subprocess.run(["node","--check"],input=script,text=True,capture_output=True)
                        self.assertEqual(checked.returncode,0,checked.stderr)


if __name__=="__main__":
    unittest.main()
