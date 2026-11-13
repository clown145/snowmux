"""Execute production OTA/SD writers and progress pages with a slow panel double."""
from pathlib import Path
import re
import unittest

from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class FirmwareUpdateRefreshTest(unittest.TestCase):
    def test_verified_https_polls_input_without_response_bytes(self):
        source = (ROOT / 'src/network/HttpDownloader.cpp').read_text()
        program = r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_HTTP_EAGAIN=7, WL_CONNECTED=1;
enum class Stall { None, Connect, Headers, Body };
Stall stall=Stall::None;
unsigned long nowMs=0;
int cleanups=0, writes=0, reads=0, statusCode=200;
bool disconnected=false, complete=true;
unsigned long millis() { return nowMs; }
void delay(int ms) { nowMs+=ms; }
struct { int status() { return disconnected ? 0 : WL_CONNECTED; } } WiFi;
struct WifiPowerSaveGuard { WifiPowerSaveGuard() {} };
template<class T> auto makeUniqueNoThrow(size_t size) { return std::make_unique<T>(size); }
#define LOG_ERR(...) ((void)0)
void esp_crt_bundle_attach() {}
struct esp_http_client_config_t {
  const char* url=nullptr;
  bool is_async=false, disable_auto_redirect=false;
  int timeout_ms=0, buffer_size=0;
  void (*crt_bundle_attach)()=nullptr;
};
using esp_http_client_handle_t = int*;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
  assert(config->is_async && config->disable_auto_redirect && config->crt_bundle_attach==esp_crt_bundle_attach);
  static int handle;
  return &handle;
}
int esp_http_client_cleanup(esp_http_client_handle_t) { ++cleanups; return ESP_OK; }
int esp_http_client_set_timeout_ms(esp_http_client_handle_t,int) { return ESP_OK; }
int esp_http_client_open(esp_http_client_handle_t,int) {
  return stall==Stall::Connect ? ESP_ERR_HTTP_EAGAIN : ESP_OK;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t) {
  if (stall!=Stall::Headers) return 3;
  nowMs+=75;
  return -ESP_ERR_HTTP_EAGAIN;
}
int esp_http_client_get_status_code(esp_http_client_handle_t) { return statusCode; }
int esp_http_client_read(esp_http_client_handle_t,char* buffer,int size) {
  assert(size>=3);
  if (++reads==1) { nowMs+=75; return -ESP_ERR_HTTP_EAGAIN; }
  if (reads==2) { buffer[0]=42; return 3; }
  if (stall!=Stall::Body) return 0;
  nowMs+=75;
  return -ESP_ERR_HTTP_EAGAIN;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t) { return complete; }
struct HttpDownloader {
  using DataCallback=std::function<bool(const uint8_t*,size_t)>;
  using AbortCallback=std::function<bool()>;
  static bool hasMemoryForTls() { return true; }
  static bool fetchVerifiedHttps(const char*,const DataCallback&,int,const AbortCallback&);
};
''' + method(source, 'bool HttpDownloader::fetchVerifiedHttps(') + r'''
int main() {
  const char* url="https://test";
  const auto sink=[](const uint8_t* data,size_t size) {
    assert(data[0]==42 && size==3);
    ++writes;
    return true;
  };
  const auto reset=[] { nowMs=0; reads=0; writes=0; };
  for (auto phase : {Stall::Connect,Stall::Headers,Stall::Body}) {
    stall=phase;
    const int expectedWrites=phase==Stall::Body ? 1 : 0;
    reset();
    assert(!HttpDownloader::fetchVerifiedHttps(url,sink,300,[] { return nowMs>=150; }));
    assert(nowMs==150 && writes==expectedWrites);
    reset();
    assert(!HttpDownloader::fetchVerifiedHttps(url,sink,300,nullptr));
    assert(nowMs==(phase==Stall::Body ? 375u : 300u) && writes==expectedWrites);
  }
  stall=Stall::None;
  reset();
  assert(HttpDownloader::fetchVerifiedHttps(url,sink,300,nullptr) && writes==1);
  reset();
  complete=false;
  assert(!HttpDownloader::fetchVerifiedHttps(url,sink,300,nullptr));
  reset();
  statusCode=302;
  assert(!HttpDownloader::fetchVerifiedHttps(url,sink,300,nullptr) && writes==0);
  statusCode=200;
  reset();
  stall=Stall::Body;
  assert(!HttpDownloader::fetchVerifiedHttps(url,sink,300,[] {
    disconnected=nowMs>=150;
    return false;
  }));
  assert(nowMs==150 && writes==1 && cleanups==10);
}
'''
        run_cpp(program)

    def test_unsafe_destinations_never_validate_allocate_erase_or_write(self):
        flasher = (ROOT / 'src/network/FirmwareFlasher.cpp').read_text()
        sd = (ROOT / 'src/activities/settings/SdFirmwareUpdateActivity.cpp').read_text()
        production = method(sd, 'bool SdFirmwareUpdateActivity::validateFirmware(')
        keys = sorted(set(re.findall(r'\bSTR_[A-Z_]+\b', production)))
        constants = '\n'.join(re.findall(r'constexpr size_t (?:SEC|BLK|CHUNK) = [^;]+;', flasher))
        program = HARNESS + '\nenum { ' + ', '.join(keys) + ' };\n'
        program += 'namespace firmware_flash {\n' + constants + '\n'
        program += method(flasher, 'Result flashFromSdPath(') + '\n}\n' + production
        program += r'''
void SdFirmwareUpdateActivity::render(RenderLock&&) {}
int main() {
  (void)ESP; (void)sdFontSystem;
  for (int unsafe : {0,1,2,3}) {
    reset(Failure::None);
    missingNext=unsafe==0;
    singleSlot=unsafe==1;
    copiedRunning=unsafe==2;
    missingRunning=unsafe==3;
    for (bool alreadyValidated : {false,true}) {
      assert(firmware_flash::flashFromSdPath("/firmware.bin",nullptr,nullptr,alreadyValidated)
             == firmware_flash::Result::NO_PARTITION);
      assert(fileOpens==0 && bufferAllocations==0 && validations==0);
      assert(erases==0 && writes==0 && switches==0);
    }
    SdFirmwareUpdateActivity activity;
    assert(!activity.validateFirmware());
    assert(!activity.errorMessage.empty());
    assert(fileOpens==1 && bufferAllocations==0 && validations==0);
    assert(erases==0 && writes==0 && switches==0);
  }
}
'''
        run_cpp(program)

    def test_picco_direct_routes_and_settings_are_disabled(self):
        ota = (ROOT / 'src/activities/settings/OtaUpdateActivity.cpp').read_text()
        sd = (ROOT / 'src/activities/settings/SdFirmwareUpdateActivity.cpp').read_text()
        settings = (ROOT / 'src/activities/settings/SettingsActivity.cpp').read_text()
        recovery = (ROOT / 'src/main.cpp').read_text()
        # The actual startup condition must continue to exclude Picco; its
        # stock recovery image is outside CrossMux's firmware-update flow.
        guard = re.search(r'#if [^\n]*\n(?:(?!#endif).)*recoveryFirmwareMode = gpio.isPressed',
                          recovery, re.S).group()
        self.assertIn('!FREEINK_DEVICE_PICCO', guard.splitlines()[0])
        rows = '\n'.join(re.findall(r'#if !FREEINK_DEVICE_PICCO(?: && !FREEINK_DEVICE_XUESU_300)?\n'
                                   r'\s*addAction\(SettingsSection::General, '
                                   r'StrId::STR_(?:CHECK_UPDATES|SD_FIRMWARE_UPDATE),.*?#endif', settings, re.S))
        program = r'''
#include <cassert>
#include <vector>
#define LOG_INF(...) ((void)0)
struct Activity {
 int enters=0, finishes=0, updates=0;
 void onEnter(){++enters;} void finish(){++finishes;} void requestUpdate(){++updates;}
};
struct MappedInputManager {
 enum class Button {Confirm}; bool isPressed(Button){return false;}
};
struct OtaUpdater {enum class Channel {Stable,Nightly};};
struct {bool otaNightlyEnabled=false;} SETTINGS;
constexpr int ACTION_RELEASE_PAGE=1,ACTION_INSTALL_UPDATE=2,CHECK_UPDATES_ROW=0;
struct OtaUpdateActivity : Activity {
 enum class State {Ready}; State state=State::Ready;
 OtaUpdater::Channel selectedChannel=OtaUpdater::Channel::Stable;
 const char* failedDetail=nullptr; int selectedReadyRow=-1, resets=0, subscriptions=0;
 bool waitForConfirmRelease=false; MappedInputManager mappedInput;
 struct App {
  int routes=0; template<class... T> void on(T...){++routes;}
  template<class... T> void setScreen(T...){++routes;}
 } app;
 void resetUi(){++resets;}
 void onReleasePage(){} void onInstallUpdate(){} void updateScreen(){}
 void onEnter();
};
struct SdFirmwareUpdateActivity : Activity {
 enum class State {PICKING}; State state=State::PICKING;
 bool recoveryMode=false; int pickers=0;
 void launchPicker(){++pickers;} void onEnter();
};
enum class SettingsSection {General};
enum class StrId {STR_CHECK_UPDATES,STR_SD_FIRMWARE_UPDATE};
enum class SettingAction {CheckForUpdates,SdFirmwareUpdate};
''' + method(ota, 'void OtaUpdateActivity::onEnter(') + '\n'
        program += method(sd, 'void SdFirmwareUpdateActivity::onEnter(') + r'''
int main() {
 OtaUpdateActivity ota; ota.onEnter();
 SdFirmwareUpdateActivity sd; sd.onEnter();
 SdFirmwareUpdateActivity recovery; recovery.recoveryMode=true; recovery.onEnter();
 std::vector<SettingAction> actions;
 const auto addAction=[&](SettingsSection,StrId,SettingAction action){actions.push_back(action);};
 (void)addAction;
''' + rows + r'''
#if FREEINK_DEVICE_PICCO
 assert(ota.enters==1 && ota.finishes==1 && ota.resets==0 && ota.app.routes==0 && ota.updates==0);
 assert(sd.finishes==1 && sd.pickers==0 && recovery.finishes==1 && recovery.pickers==0);
 assert(actions.empty());
#else
 assert(ota.finishes==0 && ota.resets==1 && ota.app.routes==3 && ota.updates==1);
 assert(sd.finishes==0 && sd.pickers==1 && recovery.finishes==0 && recovery.pickers==1);
 assert(actions.size()==2);
#endif
}
'''
        for picco in (0, 1):
            with self.subTest(picco=picco):
                run_cpp(program, defines=(f'FREEINK_DEVICE_PICCO={picco}',))

    def test_picco_backends_reject_before_tls_and_flash(self):
        updater = (ROOT / 'src/network/OtaUpdater.cpp').read_text()
        flasher = (ROOT / 'src/network/FirmwareFlasher.cpp').read_text()
        constants = '\n'.join(re.findall(r'constexpr size_t (?:SEC|BLK|CHUNK) = [^;]+;', flasher))
        program = HARNESS + r'''
constexpr size_t releaseUrlCapacity=192;
namespace CrossMuxEndpoints {
constexpr const char* OTA_MANIFEST_FORMAT="https://%s/%s%s/%s";
const char* host(){return "test";} const char* otaVariant(){return "global";}
}
namespace HalSystem {const char* getDeviceModel(){return "picco";}}
struct ReleaseJsonParser {
 explicit ReleaseJsonParser(size_t){}
 void feed(const char*,size_t){}
 bool foundUnsupportedChannel(){return false;} bool foundTag(){return true;}
 bool foundFirmware(){return true;} const char* getTagName(){return "new";}
 const char* getFirmwareUrl(){return "url";} size_t getFirmwareSize(){return 100;}
 size_t getReleaseNoteCount(){return 0;}
};
'''
        program += method(updater, 'OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate(')
        program += method(updater, 'OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(')
        program += method(updater, 'OtaUpdater::OtaUpdaterError OtaUpdater::installUpdateFromSource(')
        program += 'namespace firmware_flash {\n' + constants + '\n'
        program += method(flasher, 'Result flashFromSdPath(') + r'''
}
int main() {
 (void)ESP; (void)sdFontSystem;
 reset(Failure::None);
 OtaUpdater updater;
 for (auto channel : {OtaUpdater::Channel::Stable,OtaUpdater::Channel::Nightly})
  assert(updater.checkForUpdate(channel)==OtaUpdater::UNSUPPORTED_CHANNEL);
 assert(updater.installUpdate(nullptr,nullptr)==OtaUpdater::UNSUPPORTED_CHANNEL);
 assert(firmware_flash::flashFromSdPath("/firmware.bin",nullptr,nullptr,false)
        ==firmware_flash::Result::NO_PARTITION);
 assert(fetches==0 && fileOpens==0 && bufferAllocations==0 && validations==0);
 assert(erases==0 && writes==0 && switches==0 && aborts==0 && otaBegins==0);
}
'''
        run_cpp(program, defines=('FREEINK_DEVICE_PICCO=1',))

    def test_xuesu_switches_only_after_a_verified_download_to_the_inactive_slot(self):
        source = (ROOT / 'src/network/OtaUpdater.cpp').read_text()
        helpers = source[source.index('constexpr char xuesuManifestUrl'):source.index('#endif\n}  // namespace')]
        program = r'''
#include <algorithm>
#include <ArduinoJson.h>
#include <cassert>
#include <charconv>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>
#include "OtaUpdater.h"
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define CROSSPOINT_VERSION "0.1.0"
constexpr int ESP_OK=0, WIFI_PS_MIN_MODEM=0, ESP_PARTITION_TYPE_APP=0;
constexpr size_t releaseUrlCapacity=192;
constexpr int ESP_PARTITION_SUBTYPE_APP_OTA_0=0x10, ESP_PARTITION_SUBTYPE_APP_OTA_1=0x11;
constexpr size_t OTA_WITH_SEQUENTIAL_WRITES=0xfffffffe;
struct esp_partition_t { size_t size; unsigned address; int subtype; int type=ESP_PARTITION_TYPE_APP; };
using esp_ota_handle_t=int;
using esp_err_t=int;
esp_partition_t ota0{0x730000,0x20000,ESP_PARTITION_SUBTYPE_APP_OTA_0};
esp_partition_t ota1{0x730000,0x750000,ESP_PARTITION_SUBTYPE_APP_OTA_1};
const esp_partition_t *running=&ota1, *selected=&ota1;
int begins=0, aborts=0, switches=0;
bool writeOpen=false;
enum class Response { Complete, Interrupted, BadHash, NoBoard, Oversized, BadImage };
Response response=Response::Complete;
Response activeResponse=Response::Complete;
std::optional<Response> mirrorOverride;
std::vector<std::string> downloads;
const esp_partition_t* esp_ota_get_running_partition() { return running; }
const esp_partition_t* esp_ota_get_boot_partition() { return selected; }
const esp_partition_t* esp_ota_get_next_update_partition(void*) { return running==&ota1 ? &ota0 : &ota1; }
int esp_ota_begin(const esp_partition_t* dest,size_t mode,esp_ota_handle_t*) {
  assert(!writeOpen && dest!=running && selected==running && mode==OTA_WITH_SEQUENTIAL_WRITES);
  writeOpen=true;
  ++begins;
  return ESP_OK;
}
int esp_ota_write(esp_ota_handle_t,const uint8_t*,size_t) { assert(writeOpen); return ESP_OK; }
int esp_ota_end(esp_ota_handle_t) {
  assert(writeOpen);
  writeOpen=false;
  return activeResponse==Response::BadImage ? -1 : ESP_OK;
}
void esp_ota_abort(esp_ota_handle_t) { assert(writeOpen); writeOpen=false; ++aborts; }
int esp_ota_set_boot_partition(const esp_partition_t* dest) {
  assert(!writeOpen);
  selected=dest;
  ++switches;
  return ESP_OK;
}
void esp_wifi_set_ps(int) {}
struct mbedtls_sha256_context {};
void mbedtls_sha256_init(mbedtls_sha256_context*) {}
int mbedtls_sha256_starts(mbedtls_sha256_context*,int) { return 0; }
int mbedtls_sha256_update(mbedtls_sha256_context*,const uint8_t*,size_t) { return 0; }
int mbedtls_sha256_finish(mbedtls_sha256_context*,uint8_t* hash) {
  std::fill_n(hash,32,activeResponse==Response::BadHash ? 0xbb : 0xaa);
  return 0;
}
void mbedtls_sha256_free(mbedtls_sha256_context*) {}
struct { bool hasValidTime(){return true;} } halClock;
constexpr int WL_CONNECTED=1;
struct { int status() const { return connected ? WL_CONNECTED : 0; } bool connected=true; } WiFi;
namespace firmware_flash { uint16_t runningPartitionChipId(){return 9;} }
namespace board_tag {
struct Scanner {
  void feed(const uint8_t*,size_t) {}
  bool mismatch() const { return false; }
  bool matched() const { return activeResponse!=Response::NoBoard; }
  const char* foundName() const { return ""; }
};
}
template<class T,class Arg> auto makeUniqueNoThrow(Arg& arg) { return std::make_unique<T>(arg); }
'''
        program += helpers + r'''
std::string manifestVersion="0.1.1";
std::string assetUrl=std::string(xuesuImageBase)+std::string(64,'a')+".bin";
std::string officialModel="xuesu-300-std";
std::string officialUrl="/XueSu_300OTA/firmware10049.bin";
std::string officialVersion="1.4.9";
bool abortManifest=false;
bool abortDownload=false;
bool cancelRequested=false;
namespace HttpDownloader {
template<class Sink> bool fetchVerifiedHttps(const char* url,Sink sink,int timeout,
                                             const OtaUpdater::AbortCallback& shouldAbort) {
  if (std::string_view(url)==xuesuManifestUrl) {
    assert(timeout==xuesuOtaTimeoutMs);
    if (abortManifest) {
      cancelRequested=true;
      assert(shouldAbort());
      return false;
    }
    const std::string json="{\"tag_name\":\""+manifestVersion+"\",\"assets\":[{\"name\":\"firmware.bin\","
      "\"size\":64,\"browser_download_url\":\""+assetUrl+"\"}]}";
    return sink(reinterpret_cast<const uint8_t*>(json.data()),json.size());
  }
  if (std::string_view(url)==xuesuFactoryManifest) {
    assert(timeout==xuesuOtaTimeoutMs);
    const std::string json="{\"devices\":{\"xuebing-370-std\":{\"version_name\":\"9.0.0\",\"url\":\"/wrong.bin\",\"size\":64},\""
      +officialModel+"\":{\"version_name\":\""+officialVersion+"\",\"version_code\":10049,\"size\":64,\"url\":\""
      +officialUrl+"\"}}}";
    return sink(reinterpret_cast<const uint8_t*>(json.data()),json.size());
  }
  const std::string mirror="https://gh-proxy.com/https://github.com/clown145/snowmux/releases/download/"
                           "xuesu-v"+manifestVersion+"/xuesu-v"+manifestVersion+"-ota1.bin";
  assert(url==mirror || url==assetUrl || url==std::string(xuesuFactoryOrigin)+officialUrl);
  assert(timeout==(url==mirror ? xuesuMirrorTimeoutMs : xuesuOtaTimeoutMs));
  downloads.emplace_back(url);
  activeResponse=url==mirror ? mirrorOverride.value_or(response) : response;
  std::array<uint8_t,65> image{};
  image[0]=0xe9;
  image[12]=9;
  if (abortDownload) {
    assert(sink(image.data(),16));
    cancelRequested=true;
    assert(shouldAbort());
    return false;
  }
  if (activeResponse==Response::Interrupted) { sink(image.data(),16); return false; }
  return sink(image.data(),activeResponse==Response::Oversized ? 65 : 64);
}
}
'''
        program += '\n'.join(method(source, signature) for signature in (
            'OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate(',
            'OtaUpdater::OtaUpdaterError OtaUpdater::checkFactoryRelease(',
            'bool OtaUpdater::isUpdateNewer(',
            'const std::string& OtaUpdater::getLatestVersion(',
            'OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(',
            'OtaUpdater::OtaUpdaterError OtaUpdater::installUpdateFromSource(',
        ))
        program += r'''
int main() {
  OtaUpdater update;
  assert(update.checkForUpdate(OtaUpdater::Channel::Stable)==OtaUpdater::OK);
  assert(update.isUpdateNewer());
  for (auto mode : {Response::Interrupted, Response::BadHash, Response::NoBoard, Response::Oversized}) {
    response=mode;
    downloads.clear();
    assert(update.installUpdate()!=OtaUpdater::OK);
    assert(selected==running && switches==0);
    assert(downloads.size()==2 && downloads[0].starts_with("https://gh-proxy.com/") && downloads[1]==assetUrl);
  }
  assert(aborts==8);
  response=Response::Complete;
  mirrorOverride=Response::BadImage;
  downloads.clear();
  assert(update.installUpdate()==OtaUpdater::INTERNAL_UPDATE_ERROR && selected==running);
  assert(downloads.size()==1 && downloads[0].starts_with("https://gh-proxy.com/"));
  mirrorOverride=Response::Interrupted;
  downloads.clear();
  struct { OtaUpdater& updater; std::vector<size_t> bytes; bool finalizing=false; } progress{update, {}};
  const auto reportProgress=[](void* context,OtaUpdater::InstallPhase phase) {
    auto& state=*static_cast<decltype(progress)*>(context);
    if (phase==OtaUpdater::InstallPhase::Finalizing) {
      assert(writeOpen && selected==running);
      state.finalizing=true;
      return;
    }
    state.bytes.push_back(state.updater.getProcessedSize());
  };
  assert(update.installUpdate(reportProgress,&progress)==OtaUpdater::OK && selected==&ota0 && switches==1);
  assert(downloads.size()==2 && downloads[1]==assetUrl && aborts==9);
  assert((progress.bytes==std::vector<size_t>{16,0,64}) && progress.finalizing);
  assert(update.installUpdate()!=OtaUpdater::OK && begins==11);
  running=&ota0;
  mirrorOverride.reset();
  downloads.clear();
  assert(update.installUpdate()==OtaUpdater::OK && selected==&ota1 && switches==2);
  assert(downloads.size()==1 && downloads[0].starts_with("https://gh-proxy.com/"));
  manifestVersion="0.1.0";
  assert(update.checkForUpdate(OtaUpdater::Channel::Stable)==OtaUpdater::OK && !update.isUpdateNewer());
  assetUrl="https://github.com/firmware.bin";
  assert(update.checkForUpdate(OtaUpdater::Channel::Stable)==OtaUpdater::JSON_PARSE_ERROR);
  assert(!update.isUpdateNewer());
  running=&ota1;
  assert(update.checkForUpdate(OtaUpdater::Channel::Factory)==OtaUpdater::OK);
  assert(update.getLatestVersion()=="1.4.9" && update.getOtaSize()==64 && update.getReleaseNotes().empty());
  response=Response::BadImage;
  assert(update.installUpdate()!=OtaUpdater::OK && selected==running && switches==2);
  response=Response::NoBoard;
  downloads.clear();
  assert(update.installUpdate()==OtaUpdater::OK && selected==&ota0 && switches==3);
  assert(downloads.size()==1 && downloads[0]==std::string(xuesuFactoryOrigin)+officialUrl);
  officialVersion="1.5.0";
  officialUrl="/XueSu_300OTA/firmware10050.bin";
  assert(update.checkForUpdate(OtaUpdater::Channel::Factory)==OtaUpdater::OK);
  assert(update.getLatestVersion()=="1.5.0");
  officialUrl="http://xiaomoreader.top/insecure.bin";
  assert(update.checkForUpdate(OtaUpdater::Channel::Factory)==OtaUpdater::JSON_PARSE_ERROR);
  officialModel="other-device";
  officialUrl="/XueSu_300OTA/firmware10050.bin";
  assert(update.checkForUpdate(OtaUpdater::Channel::Factory)==OtaUpdater::JSON_PARSE_ERROR);

  officialModel=xuesuFactoryModel;
  manifestVersion="0.1.1";
  assetUrl=std::string(xuesuImageBase)+std::string(64,'a')+".bin";
  running=selected;
  const auto shouldAbort=[] { return cancelRequested; };
  abortManifest=true;
  assert(update.checkForUpdate(OtaUpdater::Channel::Stable,shouldAbort)==OtaUpdater::CANCELLED);
  assert(!update.isUpdateNewer());
  abortManifest=false;
  for (auto channel : {OtaUpdater::Channel::Stable,OtaUpdater::Channel::Factory}) {
    cancelRequested=false;
    assert(update.checkForUpdate(channel,shouldAbort)==OtaUpdater::OK);
    abortDownload=true;
    downloads.clear();
    const int previousAborts=aborts;
    assert(update.installUpdate(nullptr,nullptr,shouldAbort)==OtaUpdater::CANCELLED);
    assert(!writeOpen && aborts==previousAborts+1 && downloads.size()==1);
    assert(selected==running && switches==3);
  }
  abortDownload=false;
  assert(update.checkForUpdate(OtaUpdater::Channel::Stable)==OtaUpdater::OK);
  response=Response::Interrupted;
  WiFi.connected=false;
  downloads.clear();
  assert(update.installUpdate()==OtaUpdater::HTTP_ERROR);
  assert(!writeOpen && selected==running && switches==3 && downloads.size()==1);
}
'''
        sax = ROOT / 'freeink-sdk/libs/network/JsonSax'
        parser = ROOT / 'lib/JsonParser'
        arduino_json = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src'))
        run_cpp(program, include_dirs=(ROOT / 'src/network', parser, sax / 'include', arduino_json),
                defines=('FREEINK_DEVICE_XUESU_300=1',),
                extra_sources=(parser / 'ReleaseJsonParser.cpp', sax / 'src/StreamingJsonParser.cpp'))

    def test_flash_ordering_failure_paths_and_complete_frames(self):
        ota = (ROOT / 'src/activities/settings/OtaUpdateActivity.cpp').read_text()
        sd = (ROOT / 'src/activities/settings/SdFirmwareUpdateActivity.cpp').read_text()
        updater = (ROOT / 'src/network/OtaUpdater.cpp').read_text()
        flasher = (ROOT / 'src/network/FirmwareFlasher.cpp').read_text()
        theme = (ROOT / 'src/components/themes/BaseTheme.cpp').read_text()
        production = '\n'.join((
            method(theme, 'int BaseTheme::measureProgressBarHeight('),
            method(theme, 'int BaseTheme::drawProgressBar('),
            method(ota, 'void OtaUpdateActivity::render('),
            method(ota, 'void OtaUpdateActivity::drawCancelHint('),
            method(ota, 'void OtaUpdateActivity::beginCancelPolling('),
            method(ota, 'bool OtaUpdateActivity::pollCancel('),
            method(sd, 'void SdFirmwareUpdateActivity::render('),
            method(ota, 'void OtaUpdateActivity::runUpdateInstall('),
            method(sd, 'void SdFirmwareUpdateActivity::onConfirmationResult('),
            method(sd, 'void SdFirmwareUpdateActivity::performUpdate('),
            method(updater, 'OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate('),
            method(updater, 'OtaUpdater::OtaUpdaterError OtaUpdater::installUpdateFromSource('),
            'namespace firmware_flash {\n' + method(flasher, 'Result flashFromSdPath(') + '\n}',
        ))
        keys = sorted(set(re.findall(r'\bSTR_[A-Z_]+\b', production)))
        constants = re.search(r'constexpr unsigned int PROGRESS_REFRESH_STEP_PERCENT = \d+;', ota).group()
        sd_constants = re.search(r'constexpr unsigned int PROGRESS_REFRESH_STEP_PERCENT = \d+;', sd).group()
        self.assertEqual(constants, sd_constants)
        constants += '\n' + re.search(r'constexpr unsigned long INPUT_POLL_INTERVAL_MS = \d+;', ota).group()
        # Reuse the production SD chunk/erase sizes, not a rewritten writer loop.
        flash_constants = '\n'.join(re.findall(r'constexpr size_t (?:SEC|BLK|CHUNK) = [^;]+;', flasher))
        program = HARNESS + '\nenum { ' + ', '.join(keys) + ' };\n' + constants + '\n'
        program += 'namespace firmware_flash {\n' + flash_constants + '\n}\n'
        program += production + r'''
int main() {
  for (bool tabs : {false, true}) {
    GUI.tabs = tabs;
    for (Failure failure : {Failure::None, Failure::Download, Failure::Write,
                            Failure::Verify, Failure::Switch, Failure::WrongChip,
                            Failure::WrongBoard}) {
      reset(failure);
      OtaUpdateActivity activity;
      activity.updater.totalSize = 1000;
      activity.runUpdateInstall();
      assert(!refreshPending && !locked && activity.renderer.clears == activity.renderer.frames);
      if (failure == Failure::None) {
        assert(activity.state == OtaUpdateActivity::State::ShuttingDown && switches == 1);
        assert((activity.renderer.percentages == std::vector<int>{0,10,20,55,65,99,100}));
        assert(activity.renderer.text.back() == tr(STR_AUTO_RESTART_HINT));
      } else {
        assert(activity.state == OtaUpdateActivity::State::Failed && switches == 0 && restarts == 0);
        assert(activity.deferred && fontReloads == 1);
        const int before = activity.renderer.frames;
        activity.requestUpdateAndWait();
        assert(activity.renderer.frames == before + 1);
        assert(activity.failedDetail == ((failure == Failure::WrongChip || failure == Failure::WrongBoard)
                                        ? tr(STR_FIRMWARE_WRONG_DEVICE) : nullptr));
      }
      if (failure == Failure::Download || failure == Failure::Write ||
          failure == Failure::WrongChip || failure == Failure::WrongBoard) assert(aborts == 1);
    }

    for (Failure failure : {Failure::None, Failure::Read, Failure::Erase,
                            Failure::Write, Failure::Verify, Failure::Switch}) {
      reset(failure);
      SdFirmwareUpdateActivity activity;
      activity.onConfirmationResult(ActivityResult{});
      assert(!refreshPending && !locked && activity.renderer.clears == activity.renderer.frames);
      if (failure == Failure::None) {
        assert(activity.state == SdFirmwareUpdateActivity::State::SUCCESS && switches == 1 && restarts == 1);
        assert((activity.renderer.percentages == std::vector<int>{0,10,20,30,40,50,60,70,80,90,100}));
      } else {
        assert(activity.state == SdFirmwareUpdateActivity::State::FAILED && switches == 0 && restarts == 0);
        assert(activity.deferred && fontReloads == 1);
        activity.requestUpdateAndWait();
      }
    }

    // Repeated external render requests must also submit complete frames. The
    // live updater counters deliberately disagree with the protected snapshot.
    reset(Failure::None);
    OtaUpdateActivity activity;
    activity.state = OtaUpdateActivity::State::UpdateInProgress;
    activity.updater.processedSize = 999;
    activity.updater.totalSize = 1000;
    for (auto [done, total, percent] : {Sample{0,0,0}, Sample{19,100,19},
                                     Sample{110,100,100}, Sample{UINT32_MAX,UINT32_MAX,100}}) {
      activity.progressBytes = done;
      activity.progressTotalBytes = total;
      for (int repeat = 0; repeat < 2; ++repeat) {
        activity.requestUpdateAndWait();
        assert(activity.renderer.percentages.back() == percent);
        assert(activity.renderer.text.back() == std::to_string(done) + " / " + std::to_string(total));
        assert(activity.renderer.clears == activity.renderer.frames);
      }
    }
    SdFirmwareUpdateActivity sdActivity;
    sdActivity.state = SdFirmwareUpdateActivity::State::UPDATING;
    for (auto [done, total, percent] : {Sample{0,0,0}, Sample{19,100,19}, Sample{110,100,100},
                                     Sample{UINT32_MAX,UINT32_MAX,100}}) {
      sdActivity.writtenBytes = done;
      sdActivity.firmwareSize = total;
      for (int repeat = 0; repeat < 2; ++repeat) {
        sdActivity.requestUpdateAndWait();
        assert(sdActivity.renderer.percentages.back() == percent);
        assert(sdActivity.renderer.clears == sdActivity.renderer.frames);
      }
    }
  }

  // Exercise both actual callbacks at boundaries the real transport may skip
  // (duplicate reports, unknown totals, and values beyond the manifest size).
  reset(Failure::None);
  OtaUpdateActivity otaActivity;
  otaActivity.state = OtaUpdateActivity::State::UpdateInProgress;
  SdFirmwareUpdateActivity sdActivity;
  sdActivity.state = SdFirmwareUpdateActivity::State::UPDATING;
  auto otaProgress = ''' + method(ota, '[](void* ctx, OtaUpdater::InstallPhase phase)') + r''';
  auto sdProgress = ''' + method(sd, '[](size_t written, size_t total, void* ctx)') + r''';
  for (auto [done, total, count] : {Sample{20,0,0}, Sample{9,100,0}, Sample{10,100,1},
                                  Sample{10,100,1}, Sample{19,100,1}, Sample{20,100,2},
                                  Sample{55,100,3}, Sample{64,100,3}, Sample{65,100,4},
                                  Sample{99,100,5}, Sample{100,100,6}, Sample{110,100,6}}) {
    otaActivity.updater.processedSize = done;
    otaActivity.updater.totalSize = total;
    otaProgress(&otaActivity, OtaUpdater::InstallPhase::Downloading);
    sdProgress(done, total, &sdActivity);
    assert(otaActivity.renderer.frames == count && sdActivity.renderer.frames == count);
    assert(otaActivity.progressBytes == done && otaActivity.progressTotalBytes == total);
    assert(sdActivity.writtenBytes == done && sdActivity.firmwareSize == total);
  }

  // The transfer consumes the inherited hold and its release. A new Back
  // gesture cancels only once it is released.
  otaActivity.mappedInput.held=true;
  otaActivity.beginCancelPolling();
  assert(!otaActivity.pollCancel());
  nowMs+=20;
  otaActivity.mappedInput.held=false;
  otaActivity.mappedInput.released=true;
  assert(!otaActivity.pollCancel());
  nowMs+=20;
  otaActivity.mappedInput.held=true;
  otaActivity.mappedInput.released=false;
  assert(!otaActivity.pollCancel());
  nowMs+=20;
  otaActivity.mappedInput.held=false;
  otaActivity.mappedInput.released=true;
  assert(otaActivity.pollCancel());
}
'''
        run_cpp(program)


HARNESS = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define CROSSPOINT_VERSION "test"
#define SIMULATOR
#define SPI_FLASH_SEC_SIZE 4096
enum class Failure { None, Download, Read, Erase, Write, Verify, Switch, WrongChip, WrongBoard };
Failure fault = Failure::None;
std::mutex renderMutex;
thread_local bool locked = false, inRender = false;
std::atomic<bool> refreshPending = false;
int switches = 0, restarts = 0, aborts = 0, fontReloads = 0, writes = 0;
int erases=0,fileOpens=0,bufferAllocations=0,validations=0,fetches=0,otaBegins=0;
bool missingNext=false,singleSlot=false,copiedRunning=false,missingRunning=false;
void flashOperation() { assert(!refreshPending && !locked); }
void reset(Failure f) {
  assert(!refreshPending && !locked);
  fault = f;
  switches = restarts = aborts = fontReloads = writes = 0;
  erases=fileOpens=bufferAllocations=validations=fetches=otaBegins=0;
  missingNext=singleSlot=copiedRunning=missingRunning=false;
}
void delay(int) { flashOperation(); }
unsigned long nowMs=0;
unsigned long millis() { return nowMs; }
struct { void restart() { flashOperation(); ++restarts; } } ESP;
const char* tr(int id) { static char labels[64][32]; snprintf(labels[id],32,"label%d",id); return labels[id]; }
struct Rect { int x,y,width,height; };
struct Sample { size_t done,total; int expected; };
constexpr int UI_10_FONT_ID=10, UI_12_FONT_ID=12, SMALL_FONT_ID=8;
struct EpdFontFamily { enum { REGULAR, BOLD }; };
struct GfxRenderer {
  int clears=0, frames=0;
  mutable std::vector<std::string> text;
  mutable std::vector<int> percentages;
  int getScreenWidth() const { return 684; }
  int getScreenHeight() const { return 1216; }
  int getLineHeight(int) const { return 20; }
  void clearScreen() { assert(locked); ++clears; text.clear(); }
  template<class... Args> void drawText(int,int,int,const char* value,Args...) const { text.emplace_back(value); }
  template<class... Args> void drawCenteredText(int,int,const char* value,Args...) const {
    text.emplace_back(value);
    if (strchr(value,'%')) percentages.push_back(std::stoi(value));
  }
  void drawRect(int,int,int,int) const {}
  void fillRect(int,int,int,int) const {}
  void displayBuffer() {
    assert(locked && refreshPending);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ++frames;
  }
  struct ClipScope { ClipScope(GfxRenderer&,int,int,int,int) {} };
};
struct BaseTheme {
  int measureProgressBarHeight(const GfxRenderer&,int,bool=true) const;
  int drawProgressBar(const GfxRenderer&,Rect,size_t,size_t,bool=true) const;
};
struct ThemeMetrics {
  int topPadding=5,headerHeight=40,verticalSpacing=10,contentSidePadding=10,progressBarHeight=20,tabBarHeight=30;
};
struct UITheme : BaseTheme {
  bool tabs=false;
  static UITheme& getInstance() { static UITheme theme; return theme; }
  bool hasMainTabs() const { return tabs; }
  const ThemeMetrics& getMetrics() const { static ThemeMetrics m; return m; }
  Rect getScreenSafeArea(const GfxRenderer&,bool,bool) const { return {5,5,674,1206}; }
  template<class... Args> void drawHeader(Args...) const {}
  template<class... Args> void drawSubHeader(Args...) const {}
  template<class... Args> void drawButtonHints(Args...) const {}
  template<class... Args> void drawList(Args...) const {}
  enum class TextVerticalAlignment { TOP };
  template<class... Args> static void drawCenteredText(GfxRenderer& r,Rect,int,int,const char* s,Args...) {
    r.text.emplace_back(s);
  }
  template<class... Args> static void drawCenteredWrappedText(GfxRenderer& r,Rect,int,const char* s,Args...) {
    r.text.emplace_back(s);
  }
};
#define GUI UITheme::getInstance()
namespace SubpageLayout {
Rect contentRect(Rect r,const ThemeMetrics&) { return r; }
Rect insetHorizontal(Rect r,int) { return r; }
int relatedGap(const ThemeMetrics&) { return 10; }
int sectionGap(const ThemeMetrics&) { return 20; }
int centeredTop(Rect r,int h) { return r.y+(r.height-h)/2; }
}
struct MappedInputManager {
  enum class Button { Back };
  bool held=false,released=false;
  bool isPressed(Button) const { return held; }
  bool wasReleased(Button) const { return released; }
  bool wasHomeGesture() const { return false; }
  void update() {}
  struct Labels { const char *btn1,*btn2,*btn3,*btn4; };
  Labels mapLabels(const char* a,const char* b,const char* c,const char* d) { return {a,b,c,d}; }
};
struct RenderLock;
struct Activity {
  GfxRenderer renderer;
  MappedInputManager mappedInput;
  bool deferred=false;
  std::future<void> frame;
  virtual ~Activity() = default;
  virtual void render(RenderLock&&)=0;
  void requestUpdate(bool immediate=false);
  void requestUpdateAndWait();
  void finish() {}
};
struct RenderLock {
  explicit RenderLock(Activity&) { assert(!locked); renderMutex.lock(); locked=true; }
  ~RenderLock() { locked=false; renderMutex.unlock(); }
};
void Activity::requestUpdate(bool immediate) {
  if (!immediate) { deferred=true; return; }
  assert(!refreshPending.exchange(true));
  frame = std::async(std::launch::async,[this] {
    RenderLock lock(*this);
    inRender=true;
    render(std::move(lock));
    inRender=false;
    refreshPending=false;
  });
}
void Activity::requestUpdateAndWait() {
  assert(!locked);
  requestUpdate(true);
  frame.get();
}
struct { void ensureLoaded(GfxRenderer&,bool) { assert(locked); ++fontReloads; }
         void releaseLoadedFont(GfxRenderer&) { assert(locked); } } sdFontSystem;
namespace NetworkStartup { void prepare(GfxRenderer&) { flashOperation(); } }
struct OptionPopup { bool processRender(GfxRenderer&,MappedInputManager&) { return false; } };
struct OtaUpdater {
  enum class Channel { Stable, Nightly };
  enum OtaUpdaterError { OK, UPDATE_OLDER_ERROR, INTERNAL_UPDATE_ERROR, WRONG_DEVICE_ERROR,
                         HTTP_ERROR,UNSUPPORTED_CHANNEL,JSON_PARSE_ERROR,NO_UPDATE,CANCELLED };
  enum class InstallPhase { Downloading, Finalizing };
  using ProgressCallback=void(*)(void*,InstallPhase);
  using AbortCallback=std::function<bool()>;
  size_t processedSize=0,totalSize=0;
  size_t releaseNotes=0,otaSize=0,releaseNoteCount=0;
  Channel channel=Channel::Stable;
  bool updateAvailable=false;
  std::string latestVersion;
  std::string otaUrl="firmware";
  bool isUpdateNewer() const { return true; }
  size_t getProcessedSize() const { assert(!inRender); return processedSize; }
  size_t getTotalSize() const { assert(!inRender); return totalSize; }
  const std::string& getLatestVersion() const { static std::string version="next"; return version; }
  OtaUpdaterError installUpdate(ProgressCallback,void*,const AbortCallback& = nullptr);
  OtaUpdaterError installUpdateFromSource(const char*,ProgressCallback,void*,const AbortCallback&);
  OtaUpdaterError checkForUpdate(Channel,const AbortCallback& = nullptr);
};
struct OtaUpdateActivity : Activity {
  enum class State { Ready,WifiSelection,CheckingForUpdate,UpdateAvailable,ConfirmingUpdate,
                    UpdateInProgress,Finalizing,NoUpdate,Failed,Finished,ShuttingDown };
  State state=State::Ready;
  OtaUpdater::Channel selectedChannel=OtaUpdater::Channel::Stable;
  int selectedReadyRow=0;
  size_t progressBytes=0,progressTotalBytes=0;
  unsigned int lastProgressRefreshPercent=0;
  OtaUpdater updater;
  OptionPopup updateConfirmation;
  const char* failedDetail=nullptr;
  bool cancelRequested=false,waitForBackRelease=false;
  unsigned long lastInputPoll=0;
  void renderUpdateAvailable(Rect) {}
  void render(RenderLock&&) override;
  void runUpdateInstall();
  void beginCancelPolling();
  bool pollCancel();
  void drawCancelHint(const Rect&,int);
};
enum { CHECK_UPDATES_ROW,NIGHTLY_ROW,READY_ROW_COUNT };
Rect getReadyListRect(const GfxRenderer&) { return {0,0,400,100}; }
struct ActivityResult { bool isCancelled=false; };
struct SdFirmwareUpdateActivity : Activity {
  enum class State { PICKING,VALIDATING,CONFIRMING,UPDATING,SUCCESS,FAILED };
  State state=State::PICKING;
  bool recoveryMode=false;
  std::string firmwarePath="/firmware.bin",errorMessage;
  size_t writtenBytes=0,firmwareSize=409600;
  unsigned int lastProgressRefreshPercent=0;
  void launchPicker() {}
  void render(RenderLock&&) override;
  void onConfirmationResult(const ActivityResult&);
  void performUpdate();
  bool validateFirmware();
};
constexpr int ESP_OK=0, OTA_SIZE_UNKNOWN=-1, WIFI_PS_MIN_MODEM=0;
using esp_err_t=int;
using esp_ota_handle_t=int;
struct esp_partition_t { size_t size=1024*1024; const char* label="ota1"; int address=0x650000; };
const esp_partition_t* esp_ota_get_running_partition() {
  static esp_partition_t p{1024*1024,"ota0",0x10000}; return missingRunning?nullptr:&p;
}
const esp_partition_t* esp_ota_get_next_update_partition(void*) {
  if(missingNext) return nullptr;
  if(singleSlot) return esp_ota_get_running_partition();
  static esp_partition_t copy{1024*1024,"same app, other descriptor",0x10000};
  static esp_partition_t p; return copiedRunning?&copy:&p;
}
const char* esp_err_to_name(int) { return "failure"; }
int esp_ota_begin(const esp_partition_t*,int,esp_ota_handle_t*) { ++otaBegins; flashOperation(); return ESP_OK; }
int esp_ota_write(esp_ota_handle_t,const uint8_t*,size_t) {
  flashOperation(); ++writes; return fault==Failure::Write && writes==5 ? -1 : ESP_OK;
}
void esp_ota_abort(esp_ota_handle_t) { flashOperation(); ++aborts; }
int esp_ota_end(esp_ota_handle_t) { flashOperation(); return fault==Failure::Verify ? -1 : ESP_OK; }
int esp_ota_set_boot_partition(const esp_partition_t*) {
  flashOperation(); if (fault==Failure::Switch) return -1; ++switches; return ESP_OK;
}
void esp_wifi_set_ps(int) {}
namespace board_tag {
struct Scanner {
  void feed(const uint8_t*,size_t) {}
  bool mismatch() const { return fault==Failure::WrongBoard; }
  const char* foundName() const { return "other"; }
};
}
namespace HttpDownloader {
template<class F> bool fetchUrl(const std::string&,F write,const char*,const char*,
                                const std::vector<int>&,const OtaUpdater::AbortCallback& shouldAbort) {
  ++fetches;
  size_t previous=0;
  for (size_t current : {10,90,100,190,200,550,640,650,990,1000}) {
    if (shouldAbort && shouldAbort()) return false;
    std::vector<uint8_t> chunk(current-previous,0);
    if (fault==Failure::WrongChip && previous<=12 && current>12) chunk[12-previous]=1;
    if (!write(chunk.data(),chunk.size())) return false;
    flashOperation();
    previous=current;
    if (fault==Failure::Download && current==550) return false;
  }
  return true;
}
}
template<class T> auto makeUniqueNoThrow(size_t n) { ++bufferAllocations; return std::make_unique<T>(n); }
struct HalFile {
  explicit operator bool() const { return true; }
  size_t fileSize() const { return 409600; }
  int read(uint8_t* data,size_t n) {
    if (fault==Failure::Read && writes>=11) return 0;
    memset(data,0,n); return static_cast<int>(n);
  }
  void close() {}
};
struct { bool openFileForRead(const char*,const char*,HalFile&) { ++fileOpens; return true; } } Storage;
int esp_partition_erase_range(const esp_partition_t*,size_t offset,size_t) {
  ++erases;
  flashOperation(); return fault==Failure::Erase && offset>0 ? -1 : ESP_OK;
}
int esp_partition_write(const esp_partition_t*,size_t,const uint8_t*,size_t) {
  flashOperation(); ++writes; return fault==Failure::Write && writes==15 ? -1 : ESP_OK;
}
namespace ota_boot {
bool switchTo(const esp_partition_t* p) { return esp_ota_set_boot_partition(p)==ESP_OK; }
}
namespace firmware_flash {
enum class Result { OK,NO_PARTITION,OPEN_FAIL,OOM,READ_FAIL,ERASE_FAIL,WRITE_FAIL,OTADATA_FAIL,
                    BAD_CHIP,WRONG_BOARD,BAD_SHA,TOO_LARGE,TOO_SMALL };
using ProgressCb=void(*)(size_t,size_t,void*);
const char* resultName(Result) { return "failure"; }
uint16_t runningPartitionChipId() { return 0; }
Result validateImageFile(const char*,size_t) { ++validations; return fault==Failure::Verify ? Result::BAD_SHA : Result::OK; }
Result flashFromSdPath(const char*,ProgressCb,void*,bool=false);
}
'''


if __name__ == '__main__':
    unittest.main()
