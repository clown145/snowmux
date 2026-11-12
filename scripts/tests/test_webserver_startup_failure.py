"""Exercise production startup/cleanup against allocation and socket failures."""
from pathlib import Path
import re
import unittest
from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class WebServerStartupFailureTest(unittest.TestCase):
    def test_failed_start_cleans_up_and_can_retry(self):
        source = (ROOT / 'src/network/CrossPointWebServer.cpp').read_text()
        begin = method(source, 'bool CrossPointWebServer::begin()')
        stop = method(source, 'void CrossPointWebServer::stop()')
        handlers = sorted(set(re.findall(r'\b(handle\w+)\(', begin)))
        declarations = '\n'.join('template<class... T> void '+name+'(T&&...) {}' for name in handlers)
        run_cpp(r'''
#include <cassert>
#include <algorithm>
#include <functional>
#include <vector>
#include <memory>
#include <new>
#include <string>
#include <utility>
template<class... T> void logMessage(T&&...) {}
#define LOG_DBG(...) logMessage(__VA_ARGS__)
#define LOG_ERR(...) logMessage(__VA_ARGS__)
using String = std::string;
using wifi_mode_t = int;
constexpr int WIFI_MODE_STA=1, WIFI_MODE_AP=2, WL_CONNECTED=3, HTTP_GET=0, HTTP_POST=1, HTTP_HEAD=2;
constexpr int LOCAL_UDP_PORT=8134, FILE_LIST_BATCH_CAPACITY=1400;
int allocation=0, failAt=0, httpLive=0, wsLive=0, httpListening=0, wsListening=0;
bool udpSuccess=true;
void delay(int) {}
struct Address { String toString() const { return "192.168.4.1"; } };
struct {
 int mode=WIFI_MODE_AP;
 int getMode() { return mode; }
 int status() { return 0; }
 int softAPgetStationNum() { return 0; }
 void setSleep(bool) {}
 void setAutoReconnect(bool) {}
 Address softAPIP() { return {}; }
 Address localIP() { return {}; }
} WiFi;
struct { int getFreeHeap() { return 50000; } } ESP;
struct WebDAVHandler {
 static void* operator new(size_t size, const std::nothrow_t&) noexcept {
  return ++allocation == failAt ? nullptr : ::operator new(size, std::nothrow);
 }
 static void operator delete(void* p) noexcept { ::operator delete(p); }
};
struct Middleware {using Callback=std::function<bool()>;};
namespace UrlUtils {bool isForeignHost(const char*) {return false;}}
struct WebServer {
 std::vector<std::unique_ptr<WebDAVHandler>> handlers;
 int method(){return HTTP_GET;} String header(const char*){return {};}
 void sendHeader(const char*, const String&, bool){} void send(int,const char*,const char*){}
 template<class T> void addMiddleware(T) {}
 explicit WebServer(int) { ++httpLive; }
 ~WebServer() { --httpLive; }
 template<class... T> void on(T&&...) {}
 template<class T> void onNotFound(T) {}
 void enableCORS(bool) {}
 void collectHeaders(const char**, int) {}
 void addHandler(WebDAVHandler* p) { handlers.emplace_back(p); }
 void begin() { ++httpListening; }
 void stop() { httpListening=0; }
};
using CrossPointHttpServer = WebServer;
// Static route payloads are independent of the startup failure injections.
constexpr char webCss[]="css", iconsSvg[]="icons", crosspoint_iconSvg[]="icon";
constexpr char webCssETag[]="css-tag", iconsSvgETag[]="icons-tag", crosspoint_iconSvgETag[]="icon-tag";
void sendStaticContent(WebServer*, const char*, size_t, const char*, const char*) {}
struct WebSocketsServer {
 explicit WebSocketsServer(int) { ++wsLive; }
 ~WebSocketsServer() { --wsLive; }
 void begin() { ++wsListening; }
 void onEvent(void(*)()) {}
 void close() { wsListening=0; }
};
template<class T, class... A> std::unique_ptr<T> makeUniqueNoThrow(A&&... args) {
 if (++allocation == failAt) return nullptr;
 return std::make_unique<T>(std::forward<A>(args)...);
}
using Buffer=std::unique_ptr<unsigned char[]>;
Buffer makeWebBuffer(size_t size) {
 if (++allocation == failAt) return nullptr;
 return std::make_unique<unsigned char[]>(size);
}
bool wsUploadInProgress=false, wsUploadFile=false;
class CrossPointWebServer;
CrossPointWebServer* wsInstance=nullptr;
struct UploadState { static constexpr int UPLOAD_BUFFER_SIZE=4096; Buffer buffer; };
struct FontUploadState { static constexpr int BUFFER_SIZE=4096; Buffer buffer; };
class CrossPointWebServer {
 public:
 struct RouteHandler : WebDAVHandler {explicit RouteHandler(CrossPointWebServer&) {}};
 unsigned long wsLastActivity[2]{};
 int discoverySocket=-1;
 void openDiscoverySocket(){discoverySocket=udpSuccess?3:-1;}
 void closeDiscoverySocket(){discoverySocket=-1;}
 bool running=false, apMode=false, udpActive=false;
 int port=80, wsPort=81;
 std::unique_ptr<WebServer> server;
 std::unique_ptr<WebSocketsServer> wsServer;
 Buffer fileListBatch;
 UploadState upload;
 FontUploadState fontUpload;
 struct {
  bool listening=false, bufferAllocated=false;
  bool begin(int) { bufferAllocated=true; return listening=udpSuccess; }
  void stop() { listening=false; bufferAllocated=false; }
 } udp;
 static void wsEventCallback() {}
 void abortWsUpload(const char*) { assert(false); }
 void discardFontUpload() {}
 bool begin();
 void stop();
''' + declarations + '\n};\n' + begin + '\n' + stop + r'''
void assertStopped(const CrossPointWebServer& web) {
 assert(!web.running && !web.server && !web.wsServer && !web.fileListBatch);
 assert(!web.upload.buffer && !web.fontUpload.buffer && web.discoverySocket==-1);
 assert(!wsInstance && httpLive==0 && wsLive==0 && httpListening==0 && wsListening==0);
}
int main() {
 CrossPointWebServer web;
 WiFi.mode=0;
 assert(!web.begin());
 assertStopped(web);
 WiFi.mode=WIFI_MODE_AP;
 // WebServer, both buffers, WebDAV, WebSocket and file-list buffer allocations.
 for (int failure=1; failure<=7; ++failure) {
  allocation=0; failAt=failure;
  assert(!web.begin());
  assertStopped(web);
  allocation=0; failAt=0;
  assert(web.begin());
  assert(web.running && web.discoverySocket>=0 && wsInstance==&web);
  const int before=allocation;
  assert(web.begin() && allocation==before);
  web.stop(); web.stop();
  assertStopped(web);
 }
 allocation=0; udpSuccess=false;
 assert(!web.begin());
 assertStopped(web);
 udpSuccess=true;
 assert(web.begin());
 // Transfer suspension can destroy the listener before stop; clear its callback owner too.
 web.wsServer->close();
 web.wsServer.reset();
 web.stop();
 assertStopped(web);
}
''')

    def test_web_buffers_prefer_psram_and_fall_back(self):
        source = (ROOT / 'src/network/CrossPointWebServer.cpp').read_text()
        body = method(source, 'memory::ByteBuffer makeWebBuffer(')
        run_cpp(r'''
#include <cassert>
#include <cstddef>
#include <memory>
namespace memory {
using ByteBuffer = std::unique_ptr<unsigned char[]>;
bool psramAvailable = true, internalAvailable = true;
int internalCalls = 0;
ByteBuffer makePsramByteBufferNoThrow(size_t size) {
 return psramAvailable ? std::make_unique<unsigned char[]>(size) : nullptr;
}
ByteBuffer makeInternalByteBufferNoThrow(size_t size) {
 ++internalCalls;
 return internalAvailable ? std::make_unique<unsigned char[]>(size) : nullptr;
}
}
''' + body + r'''
int main() {
 assert(makeWebBuffer(4096) && memory::internalCalls == 0);
 memory::psramAvailable = false;
 assert(makeWebBuffer(4096) && memory::internalCalls == 1);
 memory::internalAvailable = false;
 assert(!makeWebBuffer(1400) && memory::internalCalls == 2);
}
''')

    def test_font_upload_reuses_buffer_and_publishes_only_completed_files(self):
        source = (ROOT / 'src/network/CrossPointWebServer.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'src/network/CrossPointWebServer.h').read_text(encoding='utf-8')
        installer = (ROOT / 'src/FontInstaller.cpp').read_text(encoding='utf-8')
        state = re.search(r'struct FontUploadState \{.*?\} fontUpload;', header, re.S).group()
        upload = '\n'.join(method(source, 'void CrossPointWebServer::' + name + '(') for name in [
            'discardFontUpload', 'handleFontUploadData', 'handleFontUpload'])
        names = '\n'.join(method(installer, name) for name in [
            'bool isFontNameCharacter(', 'bool FontInstaller::isValidFamilyName(',
            'static bool isValidFontFilenameWithExt(', 'bool FontInstaller::isValidCpfontFilename(',
            'bool FontInstaller::isValidVectorFontFilename('])
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
template<class... T> void logMessage(T&&...) {}
#define LOG_DBG(...) logMessage(__VA_ARGS__)
#define LOG_ERR(...) logMessage(__VA_ARGS__)
constexpr bool CROSSPOINT_VECTOR_FONTS = true;
struct String : std::string {
  using std::string::string;
  void replace(char from, char to) { std::replace(begin(), end(), from, to); }
};
struct { char sdFontFamilyName[32]; } SETTINGS;
struct { bool psram = true; } memoryState;
struct HalMemory {
  struct Heap { size_t totalBytes; };
  static Heap getPsramHeap() { return {memoryState.psram ? 8192u : 0u}; }
};
namespace memory { using ByteBuffer = std::unique_ptr<uint8_t[]>; }
struct HalFile {
  std::string* data = nullptr;
  bool isOpen() const { return data != nullptr; }
  bool close() { data = nullptr; return true; }
  size_t write(const uint8_t* bytes, size_t size) {
    data->append(reinterpret_cast<const char*>(bytes), size);
    return size;
  }
};
struct {
  std::map<std::string, std::string> files;
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    file.data = &files[path]; file.data->clear(); return true;
  }
  bool exists(const char* path) { return files.contains(path); }
  void remove(const char* path) { files.erase(path); }
  bool replaceFile(const char* part, const char* path) {
    files[path] = std::move(files.at(part)); files.erase(part); return true;
  }
} Storage;
struct {
  int installs = 0;
  int registry() { return 0; }
  void markRegistryDirty() { installs++; }
} sdFontSystem;
struct FontInstaller {
  explicit FontInstaller(int) {}
  bool ensureFamilyDir(const char*) { return true; }
  static void buildFontPath(const char* family, const char* name, char* out, size_t size) {
    std::snprintf(out, size, "/.fonts/%s/%s", family, name);
  }
  static bool isValidFamilyName(const char*);
  static bool isValidCpfontFilename(const char*);
  static bool isValidVectorFontFilename(const char*);
};
enum Status { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload {
  Status status;
  String filename = "Bookerly_14.cpfont";
  const uint8_t* buf = nullptr;
  size_t currentSize = 0, totalSize = 0;
};
struct Server {
  HTTPUpload incoming;
  String family = "Bookerly";
  int result = 0;
  HTTPUpload& upload() { return incoming; }
  String arg(const char*) { return family; }
  void send(int status, const char*, const char*) { result = status; }
};
void resetTaskWatchdogIfSubscribed() {}
struct CrossPointWebServer {
  Server* server;
  bool dropUploadIfCancelled() const { return false; }
  void discardFontUpload();
  void handleFontUploadData();
  void handleFontUpload();
''' + state + '\n};\n' + names + '\n' + upload + r'''
int main() {
  Server server;
  CrossPointWebServer web{};
  web.server = &server;
  auto& request = server.incoming;
  web.fontUpload.buffer = std::make_unique<uint8_t[]>(web.fontUpload.BUFFER_SIZE);
  const auto* buffer = web.fontUpload.buffer.get();
  const auto step = [&](Status status) { request.status = status; web.handleFontUploadData(); };
  const auto write = [&](const char* bytes, size_t size) {
    request.buf = reinterpret_cast<const uint8_t*>(bytes);
    request.currentSize = size;
    step(UPLOAD_FILE_WRITE);
  };
  const auto finish = [&](size_t size) {
    request.totalSize = size; step(UPLOAD_FILE_END); web.handleFontUpload();
  };
  const char* path = "/.fonts/Bookerly/Bookerly_14.cpfont";
  const char* part = "/.fonts/Bookerly/Bookerly_14.cpfont.part";
  const std::string original = "original";
  Storage.files[path] = original;
  step(UPLOAD_FILE_START);
  write("CPFONT", 6);
  write("\0", 1);
  finish(7);
  assert(server.result == 400 && Storage.files.at(path) == original && !Storage.exists(part));
  step(UPLOAD_FILE_START);
  write("INVALID!", 8);
  finish(8);
  assert(server.result == 400 && Storage.files.at(path) == original && !Storage.exists(part));
  step(UPLOAD_FILE_START);
  write("CPFONT\0\0", 8);
  web.handleFontUpload();
  assert(server.result == 400 && Storage.files.at(path) == original && !Storage.exists(part));
  step(UPLOAD_FILE_START);
  write("CPF", 3);
  write("ONT\0\0", 5);
  std::string payload(4700, 'f');
  write(payload.data(), payload.size());
  assert(Storage.files.at(path) == original);
  finish(8 + payload.size());
  assert(server.result == 200 && Storage.files.at(path) == std::string("CPFONT\0\0", 8) + payload);
  assert(!Storage.exists(part) && web.fontUpload.buffer.get() == buffer && sdFontSystem.installs == 1);
  step(UPLOAD_FILE_START);
  write("CPFONT\0\0", 8);
  step(UPLOAD_FILE_ABORTED);
  web.handleFontUpload();
  assert(server.result == 400 && !Storage.exists(part) && sdFontSystem.installs == 1);
  request.filename = "Family.ttc";
  memoryState.psram = false;
  step(UPLOAD_FILE_START);
  write("ttcf1234", 8);
  finish(8);
  assert(server.result == 400);
  memoryState.psram = true;
  server.family = "\xE6\xA5\xB7\xE4\xBD\x93";
  request.filename = "\xE6\xA5\xB7\xE4\xBD\x93.ttc";
  step(UPLOAD_FILE_START);
  write("ttcf1234", 8);
  finish(8);
  assert(server.result == 200 && Storage.exists("/.fonts/\xE6\xA5\xB7\xE4\xBD\x93/\xE6\xA5\xB7\xE4\xBD\x93.ttc"));
  web.handleFontUpload();
  assert(server.result == 400 && web.fontUpload.buffer.get() == buffer);
}
''')

    def test_failed_ap_init_does_not_start_services(self):
        source = (ROOT / 'src/activities/network/CrossPointWebServerActivity.cpp').read_text()
        body = method(source, 'void CrossPointWebServerActivity::startAccessPoint()')
        run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
constexpr int WIFI_AP = 2, AP_CHANNEL = 1, AP_MAX_CONNECTIONS = 4, DNS_PORT = 53;
const char* AP_PASSWORD = "";
const char* AP_SSID = "reader";
const char* AP_HOSTNAME = "reader";
int apCalls = 0, mdnsCalls = 0, dnsCalls = 0, webCalls = 0;
namespace NetworkStartup {
bool success = false;
bool setMode(int, int) { return success; }
}
void delay(int) {}
struct IPAddress { int operator[](int) const { return 1; } };
struct {
 bool success = true;
 bool softAP(const char*, const char*, int, bool, int) { ++apCalls; return success; }
 IPAddress softAPIP() { return {}; }
} WiFi;
namespace DNSReplyCode { constexpr int NoError = 0; }
struct DNSServer {
 void setErrorReplyCode(int) {}
 void start(int, const char*, IPAddress) { ++dnsCalls; }
};
DNSServer* dnsServer = nullptr;
void stopDnsServer() { delete dnsServer; dnsServer = nullptr; }
void restartMdns(const char*, const char*) { ++mdnsCalls; }
class CrossPointWebServerActivity {
 public:
 int renderer = 0, homeCalls = 0;
 std::string connectedIP, connectedSSID;
 void onGoHome() { ++homeCalls; }
 void startWebServer() { ++webCalls; }
 void startAccessPoint();
};
''' + body + r'''
int main() {
 CrossPointWebServerActivity activity;
 activity.startAccessPoint();
 assert(activity.homeCalls == 1 && apCalls == 0);
 assert(mdnsCalls == 0 && dnsCalls == 0 && webCalls == 0);
 NetworkStartup::success = true;
 WiFi.success = false;
 activity.startAccessPoint();
 assert(activity.homeCalls == 2 && apCalls == 1);
 assert(mdnsCalls == 0 && dnsCalls == 0 && webCalls == 0);
 WiFi.success = true;
 activity.startAccessPoint();
 assert(activity.homeCalls == 2 && apCalls == 2);
 assert(mdnsCalls == 1 && dnsCalls == 1 && webCalls == 1);
 stopDnsServer();
}
''')


if __name__ == '__main__':
    unittest.main()
