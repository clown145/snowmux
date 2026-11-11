"""Production phone form validation, SD transactions and portal lifecycle."""
import unittest

from test_reading_ui_regressions import ROOT, method, run_cpp

JSON = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src'))


def source(path):
    return (ROOT / path).read_text(encoding='utf-8')


class PhoneSetupTest(unittest.TestCase):
    def test_secret_redaction_validation_preservation_and_clear(self):
        run_cpp(r'''
#include <cassert>
#include "network/PhoneSetupForm.h"
int main() {
  using Form=PhoneSetupForm;
  Form form;form.title="Server";form.count=3;
  form.fields[0]={"URL","https://old.example",128,Form::Kind::Url,true};
  form.fields[1]={"Password","hidden-secret",64,Form::Kind::Password};
  form.fields[2]={"Name","",6};
  JsonDocument description;form.describe(description);
  std::string wire;serializeJson(description,wire);
  assert(wire.find("hidden-secret")==std::string::npos);
  assert(description["fields"][1]["hasValue"]==true);
  assert(description["fields"][1]["value"].isNull());
  int calls=0;Form::Values accepted;
  Form::Result outcome=Form::Result::SaveFailed;
  form.submit=[&](const auto& values){++calls;accepted=values;return outcome;};
  JsonDocument doc;
  assert(!deserializeJson(doc,R"({"values":["https://new.example",null,"中文"]})"));
  assert(form.accept(doc)==Form::Result::SaveFailed);
  assert(accepted[1]=="hidden-secret" && form.fields[0].value=="https://old.example");
  outcome=Form::Result::Saved;assert(form.accept(doc)==outcome && calls==2);
  doc["values"][1]="";assert(form.accept(doc)==outcome && accepted[1].empty());
  for(const char* url:{"", "https://", "https:///x", "ftp://server", "http://a\tb"}) {
    doc["values"][0]=url;const int before=calls;
    assert(form.accept(doc)==Form::Result::Invalid && calls==before);
  }
  doc["values"][0]="http://192.168.1.2:8080/opds";
  doc["values"][2]="中文a";assert(form.accept(doc)==Form::Result::Invalid);
  doc["values"][2]="";assert(form.accept(doc)==Form::Result::Saved);
  doc["values"][2]=nullptr;assert(form.accept(doc)==Form::Result::Invalid);
  doc["values"][2]=12;assert(form.accept(doc)==Form::Result::Invalid);
  assert(!deserializeJson(doc,R"({"values":["http://server","x\u0000y",""]})"));
  assert(form.accept(doc)==Form::Result::Invalid);
  doc["values"].as<JsonArray>().add("extra");assert(form.accept(doc)==Form::Result::Invalid);
  doc["values"].as<JsonArray>().remove(3);
  doc["values"][1]=nullptr;form.fields[1].value.assign(513,'x');
  assert(form.accept(doc)==Form::Result::Invalid);
  form.fields[1].value.clear();form.fields[1].required=true;
  assert(form.accept(doc)==Form::Result::Invalid);
}
''', include_dirs=[ROOT / 'src', JSON])

    def test_save_failures_do_not_publish_partial_connection_settings(self):
        opds = source('src/OpdsServerStore.cpp')
        koreader = source('lib/KOReaderSync/KOReaderCredentialStore.cpp')
        program = r'''
#include <ArduinoJson.h>
#include <cassert>
#include <array>
#include <mutex>
#include <string>
#include <vector>
#include <HttpHeader.h>
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
namespace obfuscation {std::string obfuscateToBase64(const std::string& s){return "obf:"+s;}}
bool ioWorks=true;JsonDocument persisted;
struct Store {
  std::mutex storeMutex;
  static const char* getFilePath(){return "test";}
  static bool writeDocToFile(const char*,const JsonDocument& doc,bool keepPrevious) {
    assert(keepPrevious);if(!ioWorks)return false;persisted.set(doc);return true;
  }
};
struct OpdsServer {
  std::string name,url,username,password;
  std::array<HttpHeader,2> customHeaders;
};
struct OpdsServerStore:Store {
  static constexpr size_t MAX_SERVERS=8;
  std::vector<OpdsServer> servers;
  void toJson(JsonDocument&)const;
  bool addServer(const OpdsServer&);
  bool updateServer(size_t,const OpdsServer&);
  bool removeServer(size_t);
};
struct KOReaderCredentialStore:Store {
  static constexpr size_t MAX_CUSTOM_HEADERS=2;
  std::string username="old",password="old-pass",serverUrl="https://old";
  std::array<HttpHeader,2> customHeaders;
  void toJson(JsonDocument& doc)const {doc["syncBehavior"]=1;doc["serverType"]=2;}
  bool saveConnectionSettings(const std::string&,const std::string&,const std::string&,const std::array<HttpHeader,2>&);
};
''' + method(opds, 'void encodeServer(') + '\n'.join(method(opds, signature) for signature in (
            'void OpdsServerStore::toJson(', 'bool OpdsServerStore::addServer(',
            'bool OpdsServerStore::updateServer(', 'bool OpdsServerStore::removeServer(')) + method(
                koreader, 'bool KOReaderCredentialStore::saveConnectionSettings(') + r'''
int main() {
  OpdsServerStore store;OpdsServer a;a.name="a";a.url="https://a";a.password="secret";
  ioWorks=false;assert(!store.addServer(a) && store.servers.empty());
  ioWorks=true;assert(store.addServer(a) && store.servers.size()==1);
  OpdsServer b=a;b.name="b";b.password="updated";b.customHeaders[0]={"X-Token","token"};
  ioWorks=false;assert(!store.updateServer(0,b));assert(store.servers[0].name=="a" && store.servers[0].password=="secret");
  assert(!store.removeServer(0) && store.servers.size()==1);
  ioWorks=true;assert(store.updateServer(0,b));assert(store.servers[0].name=="b");
  assert(persisted["servers"][0]["password_obf"]=="obf:updated");
  assert(persisted["servers"][0]["headers"][0]["value_obf"]=="obf:token");
  assert(store.removeServer(0) && store.servers.empty() && persisted["servers"].size()==0);
  b.customHeaders[0]={"Bad Header","x"};assert(!store.addServer(b));
  for(int i=0;i<8;++i)assert(store.addServer(a));
  assert(!store.addServer(a) && store.servers.size()==8);
  KOReaderCredentialStore sync;std::array<HttpHeader,2> headers{{{"X-Token","new"},{}}};
  ioWorks=false;assert(!sync.saveConnectionSettings("new","new-pass","https://new",headers));
  assert(sync.username=="old" && sync.password=="old-pass" && sync.serverUrl=="https://old" && sync.customHeaders[0].name.empty());
  ioWorks=true;assert(sync.saveConnectionSettings("new","new-pass","https://new",headers));
  assert(sync.username=="new" && sync.customHeaders[0].value=="new");
  assert(persisted["syncBehavior"]==1 && persisted["serverType"]==2 && persisted["password_obf"]=="obf:new-pass");
  headers[0].value="bad\r\nheader";assert(!sync.saveConnectionSettings("bad","", "",headers));
  assert(sync.username=="new");
}
'''
        run_cpp(program, include_dirs=[JSON, ROOT / 'lib/HttpHeader', ROOT / 'lib/AsciiText'],
                extra_sources=[ROOT / 'lib/HttpHeader/HttpHeader.cpp'])

    def test_submitted_forms_are_idempotent_and_failures_can_retry(self):
        portal = source('src/activities/network/WifiProvisionActivity.cpp')
        run_cpp(r'''
#include <cassert>
#include <memory>
#include "network/PhoneSetupForm.h"
using String=std::string;
unsigned long millis(){return 500;}
struct Web {
  std::string contentType="application/json",body=R"({"values":["new"]})";
  int status=0;
  std::string header(const char*){return contentType;}
  std::string arg(const char*){return body;}
  void send(int value){status=value;}
};
struct WifiProvisionActivity {
  enum class State{Ready,Saved,SaveFailed};State state=State::Ready;
  std::unique_ptr<PhoneSetupForm> form=std::make_unique<PhoneSetupForm>();
  std::unique_ptr<Web> web=std::make_unique<Web>();
  unsigned long joinedAt=0;
  void setState(State next){state=next;}
  void submitForm();
};
''' + method(portal, 'void WifiProvisionActivity::submitForm()') + r'''
int main(){
  WifiProvisionActivity page;page.form->count=1;
  int commits=0;bool storage=false;
  page.form->submit=[&](const auto&){++commits;return storage?PhoneSetupForm::Result::Saved:PhoneSetupForm::Result::SaveFailed;};
  page.web->contentType="text/plain";page.submitForm();assert(page.web->status==415 && commits==0);
  page.web->contentType="application/json";page.web->body="{";page.submitForm();assert(page.web->status==400);
  page.web->body=std::string(8193,'x');page.submitForm();assert(page.web->status==400 && commits==0);
  page.web->body=R"({"values":["new"]})";page.submitForm();assert(page.web->status==500 && commits==1);
  storage=true;page.submitForm();assert(page.web->status==200 && commits==2 && page.joinedAt==500);
  page.submitForm();assert(page.web->status==200 && commits==2);
}
''', include_dirs=[ROOT / 'src', JSON])

    def test_phone_portal_restores_radio_and_releases_its_form(self):
        portal = source('src/activities/network/WifiProvisionActivity.cpp')
        run_cpp(r'''
#include <cassert>
#include <memory>
#include <vector>
constexpr int WIFI_OFF=0;
struct {std::vector<int> modes;int apStops=0,reconnects=0,scans=0;
  void softAPdisconnect(bool){++apStops;}
  void mode(int mode){modes.push_back(mode);}
  void setAutoReconnect(bool){++reconnects;}
  void scanDelete(){++scans;}
} WiFi;
int scanStops=0;void esp_wifi_scan_stop(){++scanStops;}
struct Activity {void onExit(){}};
struct WifiProvisionActivity:Activity {
  struct Dns{bool stopped=false;void stop(){stopped=true;}} portalDns;
  std::unique_ptr<int> form,web=std::make_unique<int>(0);
  bool ownsRadio=false,handoff=false;int previousMode=0;
  void onExit();
};
''' + method(portal, 'void WifiProvisionActivity::onExit()') + r'''
int main(){
  for(int previous:{0,1}) {
    WifiProvisionActivity page;page.form=std::make_unique<int>(0);page.previousMode=previous;page.ownsRadio=true;
    page.onExit();assert(!page.form && !page.web && page.portalDns.stopped);
    assert(WiFi.modes.back()==previous && scanStops==0 && WiFi.scans==0 && WiFi.reconnects==0);
  }
  WifiProvisionActivity failed;failed.form=std::make_unique<int>(0);int count=WiFi.modes.size();
  failed.onExit();assert(int(WiFi.modes.size())==count);
  WifiProvisionActivity wifi;wifi.ownsRadio=true;wifi.handoff=true;wifi.onExit();
  assert(WiFi.reconnects==1 && scanStops==1);
}
''')

    def test_text_entry_delegates_to_phone_and_returns_the_existing_result(self):
        keyboard = source('src/activities/util/KeyboardEntryActivity.cpp')
        enter = method(keyboard, 'void KeyboardEntryActivity::onEnter()')
        branch = enter[enter.index('#if FREEINK_DEVICE_XUESU_300'):enter.index('#endif') + len('#endif')]
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <memory>
#include "network/PhoneSetupForm.h"
#define LOG_ERR(...) ((void)0)
bool allocates=true;
template<class T> std::unique_ptr<T> makeUniqueNoThrow(){return allocates?std::make_unique<T>():nullptr;}
struct ActivityResult{bool isCancelled=false;};
struct WifiProvisionActivity{};
struct KeyboardEntryActivity {
  enum class InputType {Text,Password,Url};InputType inputType=InputType::Text;
  std::string title="Search",text="old",completed;
  size_t maxLength=0;int cancels=0;
  bool launches=true;
  std::unique_ptr<PhoneSetupForm> request;
  std::function<void(const ActivityResult&)> callback;
  template<class T,class F> bool startActivityForResultWith(F fn,std::unique_ptr<PhoneSetupForm> form){
    callback=fn;request=std::move(form);return launches;
  }
  void onCancel(){++cancels;}
  void onComplete(std::string value){completed=std::move(value);}
  void onEnter(){
''' + branch + r'''
  }
};
int main(){
  for(auto type:{KeyboardEntryActivity::InputType::Text,KeyboardEntryActivity::InputType::Password,KeyboardEntryActivity::InputType::Url}) {
    KeyboardEntryActivity page;page.inputType=type;page.maxLength=63;page.onEnter();
    assert(page.request && page.request->count==1 && page.request->fields[0].maxBytes==63);
    assert((page.request->fields[0].kind==PhoneSetupForm::Kind::Password)==(type==KeyboardEntryActivity::InputType::Password));
    page.callback({true});assert(page.cancels==1 && page.completed.empty() && page.text=="old");
    PhoneSetupForm::Values values;values[0]="phone input";
    assert(page.request->submit(values)==PhoneSetupForm::Result::Saved);
    page.callback({false});assert(page.completed=="phone input");
  }
  KeyboardEntryActivity unlimited;unlimited.onEnter();assert(unlimited.request->fields[0].maxBytes==512);
  KeyboardEntryActivity failure;failure.launches=false;failure.onEnter();assert(failure.cancels==1);
  allocates=false;KeyboardEntryActivity oom;oom.onEnter();assert(oom.cancels==1 && !oom.request);
}
''', include_dirs=[ROOT / 'src', JSON], defines=['FREEINK_DEVICE_XUESU_300=1'])


if __name__ == '__main__':
    unittest.main()
