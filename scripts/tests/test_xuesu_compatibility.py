from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class XuesuCompatibilityTest(unittest.TestCase):
    def test_webdav_put_marks_index_for_successful_book_writes(self):
        dav = (ROOT / 'src/network/WebDAVHandler.cpp').read_text(encoding='utf-8')
        run_cpp(r'''
#include <cassert>
#include <string>
#include <string_view>

#define LOG_DBG(...)
struct WebServer {
  int status = 0;
  void send(int code, const char* = "", const char* = "") { status = code; }
};
using String = std::string;
struct WebDAVHandler {
  String path;
  bool _putOk;
  bool _putExisted;
  String getRequestPath(WebServer&) const { return path; }
  bool isProtectedPath(const String& value) const { return value.starts_with("/."); }
  void handlePut(WebServer&);
};
struct {
  void remove(const char*) {}
} Storage;
int cacheClears = 0;
int indexMarks = 0;
void clearBookCache(const char*) { ++cacheClears; }
namespace library {
bool isLibraryBookFile(std::string_view path) { return path.ends_with(".txt"); }
void markLibraryIndexDirty() { ++indexMarks; }
}
''' + method(dav, 'void WebDAVHandler::handlePut(') + r'''
int main() {
  WebServer server;
  WebDAVHandler newBook{"/Books/New book.txt", true, false};
  newBook.handlePut(server);
  assert(server.status == 201 && indexMarks == 1 && cacheClears == 1);

  WebDAVHandler replacedBook{"/Books/New book.txt", true, true};
  replacedBook.handlePut(server);
  assert(server.status == 204 && indexMarks == 2 && cacheClears == 2);

  WebDAVHandler image{"/cover.png", true, false};
  image.handlePut(server);
  assert(server.status == 201 && indexMarks == 2);

  WebDAVHandler failedBook{"/Books/Another book.txt", false, false};
  failedBook.handlePut(server);
  assert(server.status == 500 && indexMarks == 2);
}
''')

    def test_docx_reflow_preserves_nonbreaking_hyphens(self):
        source = (ROOT / 'src/bringup/XuesuBookImport.cpp').read_text(encoding='utf-8')
        write_text = method(source, 'bool writeText(') + method(source, 'bool writeEscaped(')
        parser = source[source.index('std::string_view wordElement('):source.index('class DocxRelationships')]
        parser += source[source.index('class DocxText final'):source.index('bool convertDocx(')]
        expat = ROOT / 'lib/expat'
        defines = ('XML_STATIC', 'XML_CONTEXT_BYTES=0', 'XML_GE=0')
        with TemporaryDirectory(prefix='docx-reader-') as directory:
            objects = []
            for name in ('xmlparse', 'xmlrole', 'xmltok'):
                obj = Path(directory) / f'{name}.o'
                subprocess.run(['cc', '-std=gnu11', *('-D' + flag for flag in defines),
                                '-c', str(expat / f'{name}.c'), '-o', str(obj)], check=True)
                objects.append(obj)
            run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <expat.h>
struct Print {
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t*, size_t) = 0;
  virtual ~Print() = default;
};
struct HalFile {
  std::string bytes;
  size_t write(const char* data, size_t length) { bytes.append(data, length); return length; }
};
uint32_t millis() { return 0; }
void delay(int) {}
''' + write_text + parser + r'''
int main() {
  auto parser = XML_ParserCreateNS(nullptr, '|');
  assert(parser);
  HalFile file;
  DocxText body(parser, file, nullptr, 0);
  constexpr std::string_view xml = R"(<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
    <w:body><w:p><w:r><w:t>Wi</w:t><w:noBreakHyphen/><w:t>Fi &amp; text</w:t><w:br/><w:t>next</w:t></w:r></w:p>
    </w:body></w:document>)";
  for (const uint8_t byte : xml) assert(body.write(byte) == 1);
  assert(body.finish());
  assert(file.bytes == "<p>Wi&#8209;Fi &amp; text<br/>next</p>\n");
  XML_ParserFree(parser);
}
''', include_dirs=[expat], defines=defines, extra_sources=objects)

    def test_usb_drive_persists_library_marker_before_unmount(self):
        drive = (ROOT / 'src/activities/network/UsbDriveActivity.cpp').read_text(encoding='utf-8')
        run_cpp(r'''
#include <cassert>
bool mounted = true, dirtyMarker = false;
namespace bleinput {
bool allowed = true;
bool stop() { return allowed; }
}
namespace library {
bool markLibraryIndexDirty() {
  assert(mounted);
  return dirtyMarker = true;
}
}
struct {
  bool beginUsbDrive() {
    assert(mounted && dirtyMarker);
    mounted = false;
    return true;
  }
} Storage;
struct Activity { void onEnter() {} };
struct UsbDriveActivity : Activity {
  enum class State { IoError, WaitingForHost } state{};
  bool preparing = true, startFailed = false;
  int startFailureStartedAt = 0, hostWaitStartedAt = 0;
  struct { void setScreen(int*, UsbDriveActivity*) {} } app;
  static inline int driveScreen = 0;
  void resetUi() {}
  void requestUpdateAndWait() {}
  void requestUpdate() {}
  void onEnter();
};
int millis() { return 0; }
#define LOG_ERR(...)
''' + method(drive, 'void UsbDriveActivity::onEnter()') + r'''
int main() {
  UsbDriveActivity rejected;
  bleinput::allowed = false;
  rejected.onEnter();
  assert(rejected.startFailed && mounted && !dirtyMarker);

  UsbDriveActivity drive;
  bleinput::allowed = true;
  drive.onEnter();
  assert(drive.state == UsbDriveActivity::State::WaitingForHost && !mounted && dirtyMarker);
}
''')

    def test_sd_warning_uses_repeated_latency_and_prioritizes_errors(self):
        run_cpp(r'''
#include <cassert>
#include "SdHealthMonitor.h"
int main() {
  SdHealthMonitor card;
  card.observe(100, 150, true);
  card.observe(1000, 149, true);
  card.observe(2000, 900, true);
  assert(card.takeWarning() == SdHealthWarning::None);
  card.observe(15101, 150, true);
  assert(card.takeWarning() == SdHealthWarning::None);
  card.observe(15102, 150, true);
  card.observe(15103, 150, true);
  assert(card.takeWarning() == SdHealthWarning::Slow);
  card.observe(15104, 1, false);
  assert(card.takeWarning() == SdHealthWarning::IoError);
  card.observe(16000, 300, false);
  assert(card.takeWarning() == SdHealthWarning::None);

  SdHealthMonitor wrapping;
  wrapping.observe(UINT32_MAX - 100, 500, true);
  wrapping.observe(30, 500, true);
  wrapping.observe(40, 500, true);
  wrapping.observe(41, 1, false);
  assert(wrapping.takeWarning() == SdHealthWarning::IoError);
  assert(wrapping.takeWarning() == SdHealthWarning::None);
}
''', include_dirs=[ROOT / 'lib/hal'])

    def test_snow_png_accepts_4096_rgba_and_keeps_a_width_guard(self):
        converter = (ROOT / 'lib/Epub/Epub/converters/PngToFramebufferConverter.cpp').read_text(encoding='utf-8')
        profile = (ROOT / 'platformio.ini').read_text(encoding='utf-8').split('[env:xuesu_300]', 1)[1]
        flag = next(line for line in profile.splitlines() if line.startswith('  -DPNG_MAX_BUFFERED_PIXELS='))
        limit = int(flag.split('=', 1)[1])
        helpers = '\n'.join(method(converter, name) for name in (
            'int bytesPerPixelFromType(', 'int packedRowBytes(', 'int requiredPngInternalBufferBytes(',
        ))
        run_cpp(r'''
#include <cassert>
enum { PNG_PIXEL_GRAYSCALE = 0, PNG_PIXEL_TRUECOLOR = 2, PNG_PIXEL_INDEXED = 3,
       PNG_PIXEL_GRAY_ALPHA = 4, PNG_PIXEL_TRUECOLOR_ALPHA = 6 };
''' + helpers + f'''
int main() {{
  assert(requiredPngInternalBufferBytes(4096, PNG_PIXEL_TRUECOLOR_ALPHA, 8) <= {limit});
  assert(requiredPngInternalBufferBytes(4097, PNG_PIXEL_TRUECOLOR_ALPHA, 8) > {limit});
}}
''')


if __name__ == '__main__':
    unittest.main()
