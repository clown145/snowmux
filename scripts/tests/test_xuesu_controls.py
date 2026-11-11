"""Exercise shared settings and degraded-library actions with production code."""
import re
import unittest

from test_reading_ui_regressions import ROOT, method, run_cpp


def source(path):
    return (ROOT / path).read_text(encoding='utf-8')


class XuesuControlsTest(unittest.TestCase):
    def test_side_layout_load_migration_and_web_write_keep_both_page_directions(self):
        settings = source('src/CrossPointSettings.cpp')
        listing = source('src/SettingsList.cpp')
        info = source('src/activities/settings/SettingsActivity.h')
        server = source('src/network/CrossPointWebServer.cpp')
        side = listing[listing.index('  static constexpr StrId SIDE_BUTTON_LAYOUTS'):listing.index('#if FREEINK_DEVICE_PICCO')]
        load = settings[settings.index('      const uint8_t fieldDefault'):settings.index('      s.*(info.valuePtr) = v;') + len('      s.*(info.valuePtr) = v;')]
        binary = settings[settings.index('  sideButtonLayout = validated(6,'):settings.index('  if (fileSettingsCount > 7)')]
        post = method(method(server, 'void CrossPointWebServer::handlePostSettings()'), 'case SettingType::ENUM:')
        program = r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>
enum class StrId {STR_PREV_NEXT,STR_NEXT_PREV,STR_DISABLED,STR_NEXT_NEXT,STR_PREV_PREV,STR_SIDE_BTN_LAYOUT,STR_CAT_CONTROLS,STR_NONE_OPT};
enum class SettingType {ENUM,TOGGLE,VALUE};
struct CrossPointSettings {
''' + method(source('src/CrossPointSettings.h'), 'enum SIDE_BUTTON_LAYOUT') + r''';
  static constexpr int COVER_GRID=6;
  uint8_t sideButtonLayout=PREV_NEXT,uiTheme=0;
  bool load(JsonVariantConst doc);
  void loadBinary(uint8_t saved);
} SETTINGS;
struct SettingInfo {
  StrId nameId{},category{};
  SettingType type=SettingType::ENUM;
  uint8_t CrossPointSettings::*valuePtr=nullptr;
  std::span<const StrId> staticEnumValues;
  std::vector<StrId> enumValues;
  std::vector<std::string> enumStringValues;
  std::function<void(uint8_t)> valueSetter;
  uint8_t enumValueCount=0;
  struct {int min=0,max=255;}valueRange;
  const char* key=nullptr;
''' + method(info, '  std::span<const StrId> enumLabels()') + method(info, '  static SettingInfo StaticEnum(') + r'''
};
SettingInfo sideSetting() {
  SettingInfo result;
  const auto emit=[&](const SettingInfo& s){result=s;};
''' + side + r'''
  return result;
}
bool CrossPointSettings::load(JsonVariantConst doc) {
  auto& s=*this;
  const auto info=sideSetting();
  bool needsResave=false;
  auto clamp=[](uint8_t v,uint8_t n,uint8_t fallback){return v<n?v:fallback;};
''' + load + r'''
  return needsResave;
}
void CrossPointSettings::loadBinary(uint8_t saved) {
  const auto validated=[&](int index,uint8_t current,uint8_t count){assert(index==6);return saved<count?saved:current;};
''' + binary + r'''
}
int webWrite(int value) {
  JsonDocument doc;doc["sideButtonLayout"]=value;
  const auto s=sideSetting();
  int applied=0;
  switch(s.type) {
''' + post + r'''
    default:break;
  }
  return applied;
}
int main() {
  const auto s=sideSetting();
  constexpr int count=FREEINK_DEVICE_XUESU_300?2:5;
  assert(s.enumLabels().size()==count);
  for(int value=0;value<256;++value) {
    JsonDocument doc;doc["sideButtonLayout"]=value;
    SETTINGS={};
    bool migrated=SETTINGS.load(doc.as<JsonVariantConst>());
    const int expected=value<count?value:CrossPointSettings::PREV_NEXT;
    assert(SETTINGS.sideButtonLayout==expected);
    if(FREEINK_DEVICE_XUESU_300) assert(migrated==(value>=count));
    SETTINGS={};SETTINGS.loadBinary(value);
    assert(SETTINGS.sideButtonLayout==expected);
    SETTINGS.sideButtonLayout=CrossPointSettings::NEXT_PREV;
    assert(webWrite(value)==int(value<count));
    assert(SETTINGS.sideButtonLayout==(value<count?value:CrossPointSettings::NEXT_PREV));
  }
  SETTINGS={};assert(webWrite(-1)==0);assert(SETTINGS.sideButtonLayout==0);
  JsonDocument absent;SETTINGS={};assert(!SETTINGS.load(absent.as<JsonVariantConst>()));
}
'''
        json_headers = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src'))
        for xuesu in (0, 1):
            with self.subTest(xuesu=xuesu):
                run_cpp(program, include_dirs=[json_headers], defines=[f'FREEINK_DEVICE_XUESU_300={xuesu}'])

    def test_device_and_web_iterators_share_power_key_filter(self):
        listing = source('src/SettingsList.h')
        available = method(listing, 'inline bool isSettingAvailableOnBoard(const SettingInfo& setting) {')
        get = method(listing, 'void forEachSetting(')
        post = method(listing, 'inline void forEachSettingsListEntry(')
        ids = sorted(set(re.findall(r'StrId::(STR_\w+)', available + get + post)) | {'STR_SHORT_PWR_BTN', 'STR_PWR_BTN_FOOTNOTE_BACK', 'STR_SIDE_BTN_LAYOUT'})
        fields = sorted(set(re.findall(r'&CrossPointSettings::(\w+)', available)) | {'sideButtonLayout'})
        program = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
enum class StrId {''' + ','.join(ids) + r'''};
struct CrossPointSettings {uint8_t ''' + ','.join(f'{f}=0' for f in fields) + r''';};
struct SettingInfo {
  StrId nameId{},category{};
  uint8_t CrossPointSettings::*valuePtr=nullptr;
  const char* key=nullptr;
  template<class... Args> static SettingInfo StaticEnum(Args...) {return {};}
};
namespace BoardConfig {bool isX4Pro(){return false;}bool hasTouch(){return false;}bool hasHomeKey(){return false;}}
struct {bool isAvailable(){return false;}} halTiltSensor;
struct {bool present(){return false;}} Frontlight;
struct SdCardFontRegistry {};
struct DictionaryEntry {};
namespace home_button {const char* KEYS[3]={};StrId GESTURE_LABELS[3]={},ACTION_LABELS[3]={};int FIELDS[3]={};}
const char* DICTIONARY_SETTING_KEY="dictionary";
SettingInfo buildDictionarySetting(const std::vector<DictionaryEntry>&){return {};}
SettingInfo buildFontFamilySetting(const SdCardFontRegistry*){return {};}
SettingInfo buildFontSizeSetting(const SdCardFontRegistry*){return {};}
bool isHiddenOnDevice(const SettingInfo&){return false;}
void buildFontOptions(SettingInfo&,const SdCardFontRegistry*){}
template<class Fn>void forEachBaseSetting(Fn fn) {
  SettingInfo entries[]={
    {StrId::STR_SHORT_PWR_BTN,StrId::STR_CAT_CONTROLS,&CrossPointSettings::shortPwrBtn,"shortPwrBtn"},
    {StrId::STR_PWR_BTN_FOOTNOTE_BACK,StrId::STR_CAT_CONTROLS,&CrossPointSettings::pwrBtnFootnoteBack,"pwrBtnFootnoteBack"},
    {StrId::STR_SIDE_BTN_LAYOUT,StrId::STR_CAT_CONTROLS,&CrossPointSettings::sideButtonLayout,"sideButtonLayout"}};
  for(auto& entry:entries)fn(entry);
}
''' + available + '\ntemplate<class Wants,class Visit>\n' + get + '\ntemplate<class Callback>\n' + post + r'''
int main() {
  std::vector<std::string> visible,writable;
  // Device and GET /api/settings share this iterator; POST uses the second.
  forEachSetting(nullptr,nullptr,[](const char*){return true;},[&](const SettingInfo& s){visible.emplace_back(s.key);});
  forEachSettingsListEntry(nullptr,[&](const SettingInfo& s){writable.emplace_back(s.key);});
  assert(visible==writable);
  if(FREEINK_DEVICE_XUESU_300)assert((visible==std::vector<std::string>{"sideButtonLayout"}));
  else assert((visible==std::vector<std::string>{"shortPwrBtn","pwrBtnFootnoteBack","sideButtonLayout"}));
}
'''
        for xuesu in (0, 1):
            with self.subTest(xuesu=xuesu):
                run_cpp(program, defines=[f'FREEINK_DEVICE_XUESU_300={xuesu}', 'FREEINK_CAP_WARMLIGHT=0'])

    def test_degraded_library_actions_cancel_rebuild_and_restore_sorting(self):
        library = source('src/activities/library/LibraryListActivity.cpp')
        popup = source('src/components/OptionPopup.h')
        program = r'''
#include <cassert>
#include <functional>
#include <iterator>
#include <vector>
enum class StrId {STR_XUESU_LIBRARY_ACTIONS,STR_XUESU_REVERSE_SORT,STR_LIBRARY_REBUILD,STR_DELETE,STR_XUESU_COLLAPSE_GROUPS};
constexpr auto STR_XUESU_LIBRARY_ACTIONS=StrId::STR_XUESU_LIBRARY_ACTIONS;
const char* tr(StrId){return "Library actions";}
bool isRecentSort(int sort){return sort==0;}
struct Popup {
  bool active=false;
  int selectedIndex=0;
  std::vector<StrId> ownedStrings;
  std::function<void(int)>onSelectCallback;
  bool isActive()const{return active;}
  void show(StrId,const StrId* ids,int count,int selected,std::function<void(int)>callback){
    ownedStrings.assign(ids,ids+count);selectedIndex=selected;onSelectCallback=callback;active=true;
  }
''' + method(popup, '  bool handleButtons(') + r'''
};
struct LibraryListActivity {
  Popup optionPopup;
  bool degraded=false,groupsCollapsed=false,navigationStartedOnTabs=true;
  int count=3,sortOrder=1,sorts=0,rebuilds=0,deletes=0,collapses=0,recentMenus=0,deleted=-1;
  struct {bool opened=true,degraded=false;bool isOpen()const{return opened;}bool ranksDegraded()const{return degraded;}}index;
  struct Nav {int selected=1;bool followOnBuild=false;} nav;
  Nav& activeNav(){return nav;}
  bool tabsFocused()const{return nav.selected==0;}
  int selectedEntry()const{return nav.selected-1;}
  int listCount()const{return count;}
  bool deleteEligible()const{return degraded;}
  void applyFilter(){groupsCollapsed=false;}
  void resolvePinned(){}
  void promptRebuildIndex(){++rebuilds;resetAfterRebuild();}
  void toggleSortDirection(){++sorts;}
  void showRecentBookOptions(int entry){assert(entry>=0&&entry<count);++recentMenus;}
  void promptDeleteBook(int entry){++deletes;deleted=entry;}
  void collapseGroups(int){++collapses;}
  void requestUpdate(){}
  const char* xuesuQuickActionLabel()const;
  void runXuesuQuickAction();
  void resetAfterRebuild();
};
''' + '\n'.join(method(library, signature) for signature in (
            'const char* LibraryListActivity::xuesuQuickActionLabel()',
            'void LibraryListActivity::runXuesuQuickAction()',
            'void LibraryListActivity::resetAfterRebuild()')) + r'''
int main(){
  for(int count:{0,3})for(int selected:{0,1}){
    if(!count&&selected)continue;
    LibraryListActivity lib;lib.count=count;lib.nav.selected=selected;lib.degraded=true;
    assert(lib.xuesuQuickActionLabel());lib.runXuesuQuickAction();
    const bool row=count&&selected;
    const std::vector<StrId> expected=row?std::vector<StrId>{StrId::STR_DELETE,StrId::STR_LIBRARY_REBUILD}:std::vector<StrId>{StrId::STR_LIBRARY_REBUILD};
    assert(lib.optionPopup.ownedStrings==expected);
    assert(!lib.xuesuQuickActionLabel());lib.runXuesuQuickAction(); // Modal owns input.
    lib.optionPopup.handleButtons(false,false,false,true,[]{});
    assert(lib.rebuilds==0&&lib.deletes==0&&lib.sorts==0);
    lib.runXuesuQuickAction();
    if(row)lib.optionPopup.handleButtons(false,true,false,false,[]{});
    lib.optionPopup.handleButtons(false,false,true,false,[]{});
    assert(lib.rebuilds==1&&!lib.degraded&&!lib.navigationStartedOnTabs);
    assert(lib.nav.followOnBuild&&lib.sorts==0);
    lib.nav.selected=0;lib.runXuesuQuickAction();
    assert(lib.optionPopup.ownedStrings.front()==StrId::STR_XUESU_REVERSE_SORT);
  }
  LibraryListActivity lib;lib.degraded=true;lib.nav.selected=2;lib.runXuesuQuickAction();
  lib.optionPopup.handleButtons(false,false,true,false,[]{});
  assert(lib.deletes==1&&lib.deleted==1&&lib.rebuilds==0);
  lib.sortOrder=0;lib.runXuesuQuickAction();assert(lib.recentMenus==1);
  // A rebuild can stay degraded, shrink the list, or lose its index.
  lib.index.degraded=true;lib.nav.selected=0;lib.resetAfterRebuild();
  assert(lib.degraded&&lib.nav.selected==1);
  lib.nav.selected=9;lib.count=2;lib.resetAfterRebuild();assert(lib.nav.selected==2);
  lib.count=0;lib.resetAfterRebuild();assert(lib.nav.selected==0&&lib.xuesuQuickActionLabel());
  lib.index.opened=false;lib.resetAfterRebuild();assert(!lib.degraded);
}
'''
        run_cpp(program, defines=['FREEINK_DEVICE_XUESU_300=1'])

    def test_unsorted_shelf_buttons_do_not_operate_hidden_sort_tabs(self):
        library = source('src/activities/library/LibraryListActivity.cpp')
        program = r'''
#include <cassert>
constexpr int LONG_PRESS_MS=1000;
bool isRecentSort(int sort){return sort==0;}
struct MappedInputManager {
  enum class Button {Confirm,Back,NavNext,NavPrevious};
  int direction=0;
  bool confirm=false,back=false;
  bool wasLongPressed(Button,int)const{return false;}
  bool wasReleased(Button b)const{return b==Button::Confirm?confirm:b==Button::Back?back:false;}
  bool wasPressed(Button b)const{return b==Button::NavNext?direction==1:b==Button::NavPrevious?direction==-1:false;}
};
struct ButtonNavigator {
  int direction=0;
  bool held=false;
  template<class F>void onNextPress(F fn){if(direction==1)fn();}
  template<class F>void onPreviousPress(F fn){if(direction==-1)fn();}
  template<class F>void onPreviousRelease(F){}
  template<class F>void onNextContinuous(F fn){if(direction==1&&held)fn();}
  template<class F>void onPreviousContinuous(F fn){if(direction==-1&&held)fn();}
  static int nextPageIndex(int,int count,int){return count-1;}
  static int previousPageIndex(int,int,int){return 0;}
};
struct LibraryListActivity {
  bool degraded=true,groupsCollapsed=false,navigationStartedOnTabs=false;
  int count=3,sortOrder=1,tabSteps=0,opens=0,backs=0;
  struct Nav {int selected=0;int pageRows()const{return 2;}}nav;
  MappedInputManager mappedInput;
  ButtonNavigator buttonNavigator;
  Nav& activeNav(){return nav;}
  int ringPos()const{return nav.selected;}
  bool tabsFocused()const{return ringPos()==0;}
  int listCount()const{return count;}
  int selectedEntry()const{return ringPos()>0?ringPos()-1:0;}
  bool groupable()const{return false;}
  bool deleteEligible()const{return false;}
  void toggleSortDirection(){++tabSteps;}
  void stepTab(int){++tabSteps;}
  void showRecentBookOptions(int){}
  void promptDeleteBook(int){}
  void collapseGroups(int){}
  void openSearch(){assert(!degraded);}
  void activateIndex(int entry){assert(entry>=0&&entry<count);++opens;}
  void handleBackAction(){++backs;}
  void moveRingTo(int pos){nav.selected=pos;}
  bool handleButtons();
  void navigateButtons();
};
''' + method(library, 'bool LibraryListActivity::handleButtons()') + method(library, 'void LibraryListActivity::navigateButtons()') + r'''
int main(){
  for(int count=0;count<4;++count)for(int selected=0;selected<=count;++selected){
    LibraryListActivity lib;lib.count=count;lib.nav.selected=selected;lib.mappedInput.confirm=true;
    assert(lib.handleButtons());assert(lib.tabSteps==0&&lib.opens==int(count>0));
    lib.mappedInput.confirm=false;lib.mappedInput.back=true;
    assert(lib.handleButtons());assert(lib.backs==1);
    for(int direction=-1;direction<=1;direction+=2){
      lib.nav.selected=selected;lib.mappedInput.direction=direction;
      lib.buttonNavigator.direction=direction;lib.buttonNavigator.held=true;
      lib.navigateButtons();
      assert(lib.tabSteps==0&&!lib.navigationStartedOnTabs);
      assert(count?lib.nav.selected>=1&&lib.nav.selected<=count:lib.nav.selected==0);
    }
  }
  LibraryListActivity sorted;sorted.degraded=false;sorted.mappedInput.confirm=true;
  sorted.handleButtons();assert(sorted.tabSteps==1&&sorted.opens==0);
}
'''
        run_cpp(program)


if __name__ == '__main__':
    unittest.main()
