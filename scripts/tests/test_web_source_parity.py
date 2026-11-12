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
                functions="\n".join(function(source, name) for name in [
                    "function openWebModal(", "function closeWebModal(", "function closeUploadModal(",
                    "function handleCancelUploadModal(", "function reserveAvailableUploadFilename("])
                run_node(r'''
const assert=require('node:assert/strict');
const elements=Object.create(null);
const document={getElementById:id=>elements[id]||(elements[id]={
  open:false,style:{},value:'pending',classList:{remove(){},add(){}},
  showModal(){this.open=true;},close(){this.open=false;}
}),querySelector:()=>({classList:{remove(){}}})};
let isUploadInProgress=false,operationCancelled=false,currentUploadWs=null,currentUploadXhr=null;
let epubImagesCache=['old'], imageStates={old:true},autoCropProtectedPaths=new Set(['old']),logSection=null;
const applyUploadSettings=()=>{};
''' + functions + r'''
const modal=document.getElementById('uploadModal');
openWebModal('uploadModal');assert(modal.open);
let closed=0,aborted=0;
isUploadInProgress=true;
currentUploadWs={close(){closed++;}};currentUploadXhr={abort(){aborted++;}};
closeUploadModal();assert(modal.open);assert.equal(closed,0);
handleCancelUploadModal();assert(modal.open);assert(operationCancelled);
assert.equal(closed,1);assert.equal(aborted,1);
isUploadInProgress=false;handleCancelUploadModal();assert(!modal.open);
assert.equal(document.getElementById('fileInput').value,'');assert.deepEqual(epubImagesCache,[]);
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

    def test_font_upload_accepts_files_and_groups_styles_by_name(self):
        names = (HTML / "shared/FontUploadNames.js.inc").read_text(encoding="utf-8")
        for locale in ["en", "zh-CN"]:
            with self.subTest(locale=locale):
                source = (HTML / locale / "FontsPage.html").read_text(encoding="utf-8")
                file_manager = (HTML / locale / "FilesPage.html").read_text(encoding="utf-8")
                self.assertIn('href="/fonts#upload-fonts"', file_manager)
                self.assertIn('id="upload-fonts"', source)
                self.assertNotIn("webkitdirectory", re.search(r'<input[^>]+id="fontFiles"[^>]+>', source).group())
                self.assertIn("webkitdirectory", re.search(r'<input[^>]+id="fontFolder"[^>]+>', source).group())
                upload = source[source.index("    const fontFiles ="):source.rindex("    loadFonts();")]
                run_node(r'''
const assert = require('node:assert/strict');
const elements = Object.create(null);
const document = {getElementById: id => elements[id] ||= {
  events: {}, files: [], disabled: false, textContent: '',
  addEventListener(event, fn) { this.events[event] = fn; },
  set value(value) { if (!value) this.files = []; },
}};
const requests = [], toasts = [];
class FormData {
  data = {};
  append(key, value, name) { this.data[key] = name ?? value; }
}
let allowVector = true, success = false, refreshes = 0;
const fetch = async (url, options) => {
  requests.push([url, options.body.data]);
  return {ok: success, json: async () => ({ok: success})};
};
const showWebToast = (message, error) => toasts.push({message, error});
const loadFonts = async () => { refreshes++; };
''' + function(source, "function fontExtRe(") + "\n" +
                         function(source, "function supportedExts(") + "\n" + names + upload + r'''
(async () => {
  assert.deepEqual(fontUploadNames('My-Reading-Font.ttf'),
    {family: 'My-Reading-Font', filename: 'My-Reading-Font.ttf'});
  assert.deepEqual(fontUploadNames('Bookerly-BoldItalic.TTF'),
    {family: 'Bookerly', filename: 'Bookerly-BoldItalic.ttf'});
  assert.deepEqual(fontUploadNames('\u9605\u8bfb 14.CPFONT'),
    {family: '\u9605\u8bfb', filename: '\u9605\u8bfb_14.cpfont'});
  assert.equal(fontUploadNames('Broken.cpfont'), null);
  const submit = () => elements.uploadForm.events.submit({preventDefault() {}});
  fontFiles.files = [{name:'\u6977\u4f53.ttc'}];
  fontFiles.events.change();
  assert.equal(uploadFonts.disabled, false);
  await submit();
  assert.equal(toasts.at(-1).error, true);
  assert.equal(uploadFonts.disabled, false);
  success = true;
  await submit();
  assert.deepEqual(requests[1], ['/api/fonts/upload', {family:'\u6977\u4f53', file:'\u6977\u4f53.ttc'}]);
  assert.equal(uploadFonts.disabled, true);
  fontFolder.files = [{name:'Bookerly-Regular.ttf'}, {name:'Bookerly-Bold.ttf'}, {name:'LICENSE'}];
  fontFolder.events.change();
  await submit();
  assert.deepEqual(requests.slice(2).map(([, body]) => body.family), ['Bookerly', 'Bookerly']);
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
                    if locale=="zh-CN":
                        self.assertIn("确认操作",raw)
                        self.assertNotIn("'Confirm action'",raw)
                    scripts=re.findall(r"<script(?:\s[^>]*)?>(.*?)</script>",raw,re.S)
                    for script in scripts:
                        checked=subprocess.run(["node","--check"],input=script,text=True,capture_output=True)
                        self.assertEqual(checked.returncode,0,checked.stderr)


if __name__=="__main__":
    unittest.main()
