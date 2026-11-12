from pathlib import Path
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class XuesuCompatibilityTest(unittest.TestCase):
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
