"""Run the production pagination, cursor and bounded keyboard paths on a host."""
import unittest

from test_reading_ui_regressions import ROOT, method, run_cpp


def source(path):
    return (ROOT / path).read_text(encoding='utf-8')


class XuesuTouchlessTest(unittest.TestCase):
    def test_paginated_feeds_keep_list_navigation_and_offer_only_valid_links(self):
        browser = source('src/activities/browser/OpdsBookBrowserActivity.cpp')
        popup = source('src/components/OptionPopup.h')
        program = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
enum class StrId {STR_XUESU_CATALOG_PAGES,STR_PREV_PAGE,STR_NEXT_PAGE,STR_XUESU_FIRST_PAGE,STR_XUESU_LAST_PAGE};
constexpr auto STR_XUESU_CATALOG_PAGES=StrId::STR_XUESU_CATALOG_PAGES;
const char* tr(StrId) {return "Pages";}
constexpr int16_t PAGE_FIRST=0,PAGE_PREV=1,PAGE_NEXT=2,PAGE_LAST=3;
namespace fui {struct ActionEvent {int value=0;};}
struct MappedInputManager {int direction=0;};
struct Popup {
  bool active=false;
  int selectedIndex=0;
  std::vector<StrId> ownedStrings;
  std::function<void(int)> onSelectCallback;
  bool isActive() const {return active;}
  void show(StrId,const StrId* labels,int count,int index,std::function<void(int)> fn) {
    ownedStrings.assign(labels,labels+count);selectedIndex=index;onSelectCallback=fn;active=true;
  }
  template<class F> bool handleInput(MappedInputManager&,F) {return active;}
''' + method(popup, '  bool handleButtons(') + r'''
};
struct CatalogActivity {
  int delegated=0;
  bool handleCustomInput() {++delegated;return false;}
};
struct OpdsBookBrowserActivity : CatalogActivity {
  enum class State {BROWSING,LOADING};State state=State::BROWSING;
  MappedInputManager mappedInput;
  Popup paginationPopup;
  struct {void clearTapFlash() {}} app;
  std::string pageFirstHref,pagePrevHref,pageNextHref,pageLastHref,followed;
  int fetches=0,selection=0;
  void requestUpdate() {}
  void followPageLink(const std::string& link) {followed=link;++fetches;}
  void tick() {if(!handleCustomInput()) selection+=mappedInput.direction;}
  bool handleCustomInput();
  const char* xuesuQuickActionLabel() const;
  void runXuesuQuickAction();
  static void onPageEvent(const fui::ActionEvent&,void*);
};
''' + '\n'.join(method(browser, signature) for signature in (
            'bool OpdsBookBrowserActivity::handleCustomInput()',
            'const char* OpdsBookBrowserActivity::xuesuQuickActionLabel()',
            'void OpdsBookBrowserActivity::runXuesuQuickAction()',
            'void OpdsBookBrowserActivity::onPageEvent(')) + r'''
int main() {
  for(int mask=0;mask<16;++mask) {
    OpdsBookBrowserActivity page;
    if(mask&1) page.pageFirstHref="first";
    if(mask&2) page.pagePrevHref="prev";
    if(mask&4) page.pageNextHref="next";
    if(mask&8) page.pageLastHref="last";
    page.mappedInput.direction=1;page.tick();page.tick();page.tick();
    assert(page.selection==3 && page.delegated==3 && page.fetches==0);
    page.mappedInput.direction=-1;page.tick();assert(page.selection==2);
    assert(bool(page.xuesuQuickActionLabel())==bool(mask));
    std::vector<std::string> expected;
    if(mask&2) expected.emplace_back("prev");
    if(mask&4) expected.emplace_back("next");
    if(mask&1) expected.emplace_back("first");
    if(mask&8) expected.emplace_back("last");
    page.runXuesuQuickAction();assert(page.paginationPopup.ownedStrings.size()==expected.size());
    for(size_t choice=0;choice<expected.size();++choice) {
      page.paginationPopup.active=false;page.runXuesuQuickAction();
      assert(!page.xuesuQuickActionLabel());
      page.tick();assert(page.selection==2); // The menu owns all input while open.
      for(size_t i=0;i<choice;++i) page.paginationPopup.handleButtons(false,true,false,false,[]{});
      page.paginationPopup.handleButtons(false,false,true,false,[]{});
      assert(page.followed==expected[choice] && page.fetches==int(choice)+1);
    }
    page.runXuesuQuickAction();
    const int fetches=page.fetches;
    page.paginationPopup.handleButtons(false,false,false,true,[]{});
    assert(!page.paginationPopup.active && page.fetches==fetches);
    page.state=OpdsBookBrowserActivity::State::LOADING;
    assert(!page.xuesuQuickActionLabel());page.runXuesuQuickAction();
    assert(!page.paginationPopup.active);
  }
}
'''
        run_cpp(program, defines=['FREEINK_DEVICE_XUESU_300=1'])

    def test_long_text_viewport_and_keyboard_fit_both_orientations(self):
        keyboard = source('src/activities/util/KeyboardEntryActivity.cpp')
        # Exercise real theme values, including their large header/hint bands.
        import re
        render = method(keyboard, 'void KeyboardEntryActivity::render(')
        input_render = render[render.index('  const int lineHeight ='):render.index('  const int fieldWidth =')]
        base = source('src/components/themes/BaseTheme.h')
        metrics = re.search(r'struct ThemeMetrics \{.*?\n};', base, re.S).group()
        metrics += re.search(r'namespace UiHighDpiProfile \{.*?namespace UiHighDpiProfile', base, re.S).group() + '\n'
        for path, name in [('BaseTheme.h', 'Base'), ('lyra/LyraTheme.h', 'Lyra'), ('inx/InxTheme.h', 'Inx')]:
            theme = source('src/components/themes/' + path)
            start = theme.index('namespace ' + name + 'Metrics {')
            metrics += theme[start:theme.index('\nclass ', start)]
        program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#pragma GCC diagnostic ignored "-Wsign-compare"
struct Rect {int x,y,width,height;};
namespace fui {struct Rect {int16_t x,y,width,height;};}
constexpr int SMALL_FONT_ID=0,UI_12_FONT_ID=1,KEYBOARD_MIN_GAP=6,KEYBOARD_PANEL_PADDING=4;
constexpr int BUTTON_KEY_HEIGHT=42,BUTTON_KEYBOARD_HINT_GAP=4;
constexpr int KEYBOARD_LABEL_PADDING=4;
''' + metrics + r'''
struct EpdFontFamily {static constexpr int REGULAR=0;};
struct Renderer {
  int width=300,height=400;
  std::vector<int> textRows;
  int getScreenWidth() const {return width;}
  int getScreenHeight() const {return height;}
  int getLineHeight(int font) const {return font==SMALL_FONT_ID?23:29;}
  int getTextAdvanceX(int,const char* s,int) const {
    int n=0;for(;*s;++s) n+=(static_cast<uint8_t>(*s)&0xc0)!=0x80;return n*12;
  }
  int getTextWidth(int font,const char* s) const {return getTextAdvanceX(font,s,0);}
  void drawText(int,int,int y,const char*) {textRows.push_back(y);}
};
struct CrossPointSettings {enum UI_THEME {CLASSIC,LYRA,INX};UI_THEME uiTheme=INX;} SETTINGS;
struct UITheme {
  static UITheme& getInstance() {static UITheme t;return t;}
  const ThemeMetrics& getMetrics() const {
    switch(SETTINGS.uiTheme) {
      case CrossPointSettings::CLASSIC:return BaseMetrics::values;
      case CrossPointSettings::LYRA:return LyraMetrics::values;
      case CrossPointSettings::INX:return InxMetrics::values;
    }
    return BaseMetrics::values;
  }
  static Rect getContentArea(const Renderer& r) {return {0,0,r.width,r.height};}
  Rect getScreenSafeArea(const Renderer& r) const {return getContentArea(r);}
};
int keyboardGap(const ThemeMetrics& m,bool compact=false) {return compact?6:std::max(m.keyboardKeySpacing,6);}
int keyboardKeysHeight(const ThemeMetrics&,int,bool) {return 0;}
struct KeyboardEntryActivity {
  enum class InputType {Text,Password};InputType inputType=InputType::Text;
  Renderer renderer;
  struct {bool deviceIsX3() const {return false;}} gpio;
  struct {bool hasTouch() const {return false;}} mappedInput;
  struct Layout {int rowCount=5;} layout;
  const Layout& currentLayout() const {return layout;}
  size_t cursorPos=0;
  std::string text;
  bool cursorMode=false,togglePos=false,passwordVisible=false;
  std::string displayTextForCurrentState() const;
  bool rangeIsRtl(std::string&,int,int) const {return false;}
  static size_t utf8Prev(const std::string&,size_t);
  static size_t utf8Next(const std::string&,size_t);
  int measureRange(std::string&,int,int) const;
  int lineBreakEnd(std::string&,int,int) const;
  int inputWindowStart(std::string&,int) const;
  int inputLineCount() const;
  int inputStartY() const;
  fui::Rect keyboardRect() const;
  fui::Rect keyboardKeysRect() const;
  void renderInput() {
    renderer.textRows.clear();
    const auto& metrics=UITheme::getInstance().getMetrics();
    const auto content=UITheme::getContentArea(renderer);
''' + input_render + r'''
    assert(cursorDrawn && cursorPixelX>=textLeft);
    assert(cursorLineY>=inputY && cursorLineY<inputY+inputLineCount()*lineHeight);
    assert(inputHeight<inputLineCount()*lineHeight);
    assert(inputY+inputHeight+lineHeight+metrics.verticalSpacing+4+6<=keyboardRect().y);
    for(int y:renderer.textRows) assert(y>=inputY && y<inputY+inputLineCount()*lineHeight);
  }
};
''' + '\n'.join(method(keyboard, signature) for signature in (
            'size_t KeyboardEntryActivity::utf8Prev(', 'size_t KeyboardEntryActivity::utf8Next(',
            'std::string KeyboardEntryActivity::displayTextForCurrentState()',
            'int KeyboardEntryActivity::measureRange(', 'int KeyboardEntryActivity::lineBreakEnd(',
            'int KeyboardEntryActivity::inputWindowStart(', 'int KeyboardEntryActivity::inputLineCount()',
            'int KeyboardEntryActivity::inputStartY()', 'fui::Rect KeyboardEntryActivity::keyboardRect()',
            'fui::Rect KeyboardEntryActivity::keyboardKeysRect()')) + r'''
int main() {
  for(auto theme:{CrossPointSettings::CLASSIC,CrossPointSettings::LYRA,CrossPointSettings::INX})
  for(bool landscape:{false,true}) for(int rows:{4,5}) {
    SETTINGS.uiTheme=theme;KeyboardEntryActivity kb;kb.layout.rowCount=rows;
    kb.renderer.width=landscape?400:300;kb.renderer.height=landscape?300:400;
    const auto rect=kb.keyboardRect(), keys=kb.keyboardKeysRect();
    const auto& m=UITheme::getInstance().getMetrics();
    const int inputBottom=kb.inputStartY()+kb.inputLineCount()*29+m.verticalSpacing+4;
    assert(rect.y>=inputBottom+6 && keys.y==rect.y && keys.height==rect.height);
    assert(rect.y+rect.height<=kb.renderer.height-m.buttonHintsHeight);
    assert((keys.height-8-(rows-1)*6)/rows>=23+4); // Include the SDK button's text padding.
    std::string unicode;for(int i=0;i<60;++i) unicode+="中é";
    for(std::string text:{std::string(128,'a'),std::string("https://server.example/")+std::string(106,'x'),unicode,std::string()}) {
      kb.text=text;
      for(size_t cursor=0;cursor<=text.size();cursor=KeyboardEntryActivity::utf8Next(text,cursor)) {
        kb.cursorPos=cursor;
        int start=kb.inputWindowStart(text,180),end=start;
        assert(start==0 || (static_cast<uint8_t>(text[start])&0xc0)!=0x80);
        for(int line=0;line<kb.inputLineCount();++line) end=kb.lineBreakEnd(text,end,180);
        assert(size_t(start)<=cursor && (cursor<size_t(end) || size_t(end)==text.size()));
        for(bool password:{false,true}) for(bool editing:{false,true}) for(bool visible:{false,true}) {
          kb.inputType=password?KeyboardEntryActivity::InputType::Password:KeyboardEntryActivity::InputType::Text;
          kb.cursorMode=editing;kb.passwordVisible=visible;kb.renderInput();
        }
        if(cursor==text.size()) break;
      }
    }
  }
}
'''
        run_cpp(program, include_dirs=[ROOT / 'src'], defines=['FREEINK_DEVICE_XUESU_300=1'])
        other_profiles = program[:program.rindex('int main() {')] + r'''
int main() {
  for(bool landscape:{false,true}) {
    KeyboardEntryActivity kb;
    kb.renderer.width=landscape?1216:684;kb.renderer.height=landscape?684:1216;
    std::string text;for(int i=0;i<50;++i)text+="a中é";
    for(size_t cursor=0;cursor<=text.size();cursor=KeyboardEntryActivity::utf8Next(text,cursor)) {
      kb.cursorPos=cursor;int start=kb.inputWindowStart(text,180),end=start;
      if(UiHighDpiProfile::enabled) {
        assert(kb.inputLineCount()==(landscape?1:2));
        for(int row=0;row<kb.inputLineCount();++row)end=kb.lineBreakEnd(text,end,180);
        assert(size_t(start)<=cursor && (cursor<size_t(end) || size_t(end)==text.size()));
      } else {
        assert(start==0 && kb.inputLineCount()==2);
      }
      if(cursor==text.size())break;
    }
  }
}
'''
        for extra in ([], ['CROSSMUX_UI_PROFILE_HIGH_DPI=1']):
            run_cpp(other_profiles, include_dirs=[ROOT / 'src'], defines=['FREEINK_DEVICE_XUESU_300=0', *extra])


if __name__ == '__main__':
    unittest.main()
