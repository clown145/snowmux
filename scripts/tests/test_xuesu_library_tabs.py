"""Exercise production library sorting tabs with the SDK and firmware font metrics."""

import json
import re
import unittest

from test_reading_ui_regressions import ROOT, method, run_cpp


class LibraryTabsTest(unittest.TestCase):
    def test_library_tab_labels_and_confirm_cycle(self):
        library = (ROOT / 'src/activities/library/LibraryListActivity.cpp').read_text(encoding='utf-8')
        tab_ui = (ROOT / 'src/activities/UiTabListActivity.cpp').read_text(encoding='utf-8')
        theme_ui = (ROOT / 'src/components/UITheme.cpp').read_text(encoding='utf-8')
        zh = (ROOT / 'lib/I18n/translations/chinese.yaml').read_text(encoding='utf-8')
        names = ('STR_LIBRARY_TAB_RECENT', 'STR_LIBRARY_TAB_TITLE', 'STR_LIBRARY_TAB_AUTHOR',
                 'STR_XUESU_LIBRARY_ACTIONS_HINT', 'STR_LIBRARY_POSITION', 'STR_LIBRARY_GROUP_POSITION')
        labels = [json.dumps(re.search(rf'^{name}: "([^"]+)"', zh, re.M)[1]) for name in names]
        program = r'''
#include <FreeInkApp.h>
#include <Utf8.h>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 16
#pragma GCC diagnostic ignored "-Wnon-c-typedef-for-linkage"
#endif
#include "EpdFont.cpp"
#include "EpdFontFamily.h"
#include "builtinFonts/notosans_cjk_8.h"
#include "builtinFonts/notosans_cjk_10.h"
#include "builtinFonts/notosans_cjk_12.h"
#include "builtinFonts/ubuntu_8_regular.h"
#include "builtinFonts/ubuntu_10_regular.h"
#include "builtinFonts/ubuntu_12_regular.h"
#pragma GCC diagnostic pop
#include "components/themes/inx/InxTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

namespace fui = freeink::ui;
namespace library {
enum class SortOrder { RecentAsc, RecentDesc, TitleAsc, TitleDesc, AuthorAsc, AuthorDesc };
}
enum StrId { STR_LIBRARY_TAB_RECENT, STR_LIBRARY_TAB_TITLE, STR_LIBRARY_TAB_AUTHOR,
             STR_XUESU_LIBRARY_ACTIONS_HINT, STR_LIBRARY_POSITION, STR_LIBRARY_GROUP_POSITION };
constexpr const char* labels[]={@LABELS@};
const char* tr(StrId id){return labels[id];}

// This fixture measures Han and ASCII numerals only.
bool BidiUtils::isTransparentMark(uint32_t){return false;}
constexpr int16_t TOUCH_TAB_BAR_HEIGHT=50;
@SIDE_PADDING@
int bottomCornerClearance(){return 0;}
struct GfxRenderer;
struct UITheme {
  enum class TextVerticalAlignment { TOP, CENTER, BOTTOM };
  ThemeMetrics metrics = BaseMetrics::values;
  bool inx = false;
  bool hasMainTabs() const {return inx;}
  const ThemeMetrics& getMetrics() const {return metrics;}
  static UITheme& getInstance(){static UITheme theme;return theme;}
  static Rect getContentArea(const GfxRenderer&);
  static void drawCenteredText(const GfxRenderer&, Rect, int, int, const char*, bool=true,
                               EpdFontFamily::Style=EpdFontFamily::REGULAR);
  static void drawCenteredWrappedText(const GfxRenderer&,Rect,int,const char*,int,bool=true,
                                      EpdFontFamily::Style=EpdFontFamily::REGULAR,
                                      TextVerticalAlignment=TextVerticalAlignment::CENTER);
};
struct MappedInputManager {
  enum class Button { Confirm, Back };
  bool confirm=false;
  bool hasTouch() const {return false;}
  bool wasLongPressed(Button,unsigned long) const {return false;}
  bool wasReleased(Button button) const {return confirm && button==Button::Confirm;}
};
struct GfxRenderer {
  int width=300,height=400;
  struct Run {Rect rect;std::string text;};
  mutable std::vector<Run> runs;
  int getScreenWidth() const {return width;}
  int getScreenHeight() const {return height;}
  int getLineHeight(int) const {return ubuntu_8_regular.advanceY;}
  int getTextWidth(int,const char* text,EpdFontFamily::Style=EpdFontFamily::REGULAR) const {
    int w=0,h=0;
    EpdFont(&notosans_cjk_8).getTextDimensions(text,&w,&h);
    return w;
  }
  void drawText(int font,int x,int y,const char* text,bool,EpdFontFamily::Style style=EpdFontFamily::REGULAR) const {
    runs.push_back({Rect{x,y,getTextWidth(font,text,style),getLineHeight(font)},text});
  }
  std::vector<std::string> wrappedText(int,const char*,int,int,EpdFontFamily::Style) const {
    assert(false && "Library footer hint should fit on one line");
    return {};
  }
};
Rect UITheme::getContentArea(const GfxRenderer& r){return Rect{0,0,r.width,r.height};}
using UiScreen=fui::Screen<24>;
struct UiTabListActivity {
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  int16_t tabPillMaxPad=0;
  fui::ListNav nav;
  static constexpr fui::ActionId ACTION_TAB=11;
  UiTabListActivity(const char*,GfxRenderer& renderer,MappedInputManager& input,bool)
    : renderer(renderer), mappedInput(input) {}
  virtual int activeTab() const=0;
  virtual int tabCount() const=0;
  virtual const char* tabLabel(int) const=0;
  virtual fui::TabIndicator tabIndicator(int) const=0;
  int ringPos() const {return nav.selected;}
  void buildTabBar(UiScreen&,bool=false);
};
struct LibraryListActivity:UiTabListActivity {
  LibraryListActivity(GfxRenderer&,MappedInputManager&);
  int activeTabIndex=0;
  library::SortOrder sortOrder=library::SortOrder::RecentDesc;
  uint8_t descendingTabs=1;
  bool degraded=false,groupsCollapsed=false;
  int filterUpdates=0;
  int rows=3;
  int activeTab() const override;
  int tabCount() const override;
  const char* tabLabel(int) const override;
  fui::TabIndicator tabIndicator(int) const override;
  void selectTab(int,bool);
  void stepTab(int);
  bool handleButtons();
  bool tabsFocused() const {return ringPos()==0;}
  fui::ListNav& activeNav(){return nav;}
  int listCount() const {return rows;}
  void applyFilter(){++filterUpdates;}
  void refreshOverlap(){}
  void requestUpdate(){}
  void toggleSortDirection(){assert(false);}
  int selectedEntry() const {return rows-1;}
  const char* xuesuQuickActionLabel() const {return "Actions";}
  int drawPositionReadout() const;
  void drawHoldHelp(int) const;
  void showRecentBookOptions(int){assert(false);}
  bool deleteEligible() const {return false;}
  void promptDeleteBook(int){assert(false);}
  bool groupable() const {return false;}
  void collapseGroups(int){assert(false);}
  void activateIndex(int){assert(false);}
  void handleBackAction(){assert(false);}
};

struct RecordingTarget: fui::DrawTarget {
  bool inx;
  struct TextRun { fui::Rect bounds; std::string text; fui::Color ink; };
  std::vector<TextRun> runs;
  int triangles=0;
  explicit RecordingTarget(bool inx):inx(inx){}
  fui::Size measureText(fui::FontId font,const char* text,fui::TextStyle) const override {
    // makeUiTarget uses UI_10 for non-INX small text and INX body, UI_12
    // for RoundedRaff body. Han glyphs resolve to the corresponding Noto CJK.
    const EpdFont fallback{font==fui::FONT_SLOT_SMALL||inx?&notosans_cjk_10:&notosans_cjk_12};
    const auto* p=reinterpret_cast<const uint8_t*>(text);
    while (const uint32_t cp=utf8NextCodepoint(&p)) assert(fallback.hasCodepoint(cp));
    int w=0,h=0;
    fallback.getTextDimensions(text,&w,&h);
    return {static_cast<int16_t>(w),static_cast<int16_t>(h)};
  }
  int16_t lineHeight(fui::FontId font) const override {
    return font==fui::FONT_SLOT_SMALL||inx?ubuntu_10_regular.advanceY:ubuntu_12_regular.advanceY;
  }
  void fill(fui::Rect,fui::Paint,uint8_t,uint8_t) override {}
  void stroke(fui::Rect,fui::Paint,uint8_t,uint8_t,uint8_t) override {}
  void line(fui::Point,fui::Point,uint8_t,fui::Paint) override {}
  void triangle(fui::Point,fui::Point,fui::Point,fui::Paint) override {++triangles;}
  void text(fui::Rect rect,const char* text,fui::TextStyle style) override {
    const auto measured=measureText(style.font,text,style);
    // GfxRendererTarget would invoke ellipsis truncation above this width.
    assert(measured.width<=rect.width && lineHeight(style.font)<=rect.height);
    runs.push_back({rect,text,style.color});
  }
  void bitmap(fui::Rect,fui::BitmapRef,fui::BitmapMode,fui::Paint,fui::Rotation) override {}
};
@PRODUCTION@

int main() {
  const ThemeMetrics variants[]={BaseMetrics::values,LyraMetrics::values,Lyra3CoversMetrics::values,
                                  LyraCarouselMetrics::values,RoundedRaffMetrics::values,InxMetrics::values,
                                  LyraMetrics::values /* Cover Grid shares Lyra */};
  for (int theme=0;theme<7;++theme) for (bool landscape:{false,true}) {
    auto& ui=UITheme::getInstance();
    ui.metrics=variants[theme];
    ui.inx=theme==5;
    GfxRenderer renderer;
    renderer.width=landscape?400:300;
    renderer.height=landscape?300:400;
    MappedInputManager keys;
    LibraryListActivity lib(renderer,keys);
    constexpr library::SortOrder orders[]={library::SortOrder::RecentDesc,library::SortOrder::TitleAsc,
                                            library::SortOrder::AuthorAsc};
    for (int selected=0;selected<3;++selected) {
      assert(lib.activeTab()==selected && lib.sortOrder==orders[selected]);
      for (bool focused:{false,true}) {
        lib.nav.selected=focused?0:1;
        RecordingTarget target(ui.inx);
        fui::DeviceContext device;
        device.width=landscape?400:300;
        device.height=landscape?300:400;
        fui::InputSnapshot input;
        fui::InteractionBuffer<24> interactions;
        fui::Frame<24> frame(target,device,input,interactions);
        auto tokens=fui::themeTokensForLineHeight(target.lineHeight(fui::FONT_SLOT_BODY));
        tokens.listRowRadius=ui.metrics.listRowRadius;
        UiScreen screen(frame,tokens);
        const auto top=static_cast<int16_t>(ui.metrics.topPadding+ui.metrics.headerHeight);
        screen.setContentMarginFromScreen(fui::Insets{top,0,24,0});
        lib.buildTabBar(screen);
        interactions.publish();
        assert(target.runs.size()==3 && interactions.publishedCount()==3);
        assert(target.triangles==(ui.inx?0:1));
        const auto* hits=interactions.publishedData();
        for (int tab=0;tab<3;++tab) {
          const auto& run=target.runs[tab];
          const auto& hit=hits[tab];
          assert(hit.action==UiTabListActivity::ACTION_TAB && hit.value==tab);
          assert(run.text==labels[tab]);
          assert(run.bounds.x>=hit.rect.x && run.bounds.right()<=hit.rect.right());
          assert(run.bounds.y>=top && run.bounds.bottom()<=screen.contentRect().y);
          if (tab>0) {
            assert(hits[tab-1].rect.right()<=hit.rect.x);
            assert(target.runs[tab-1].bounds.right()+(ui.inx?0:6)<=run.bounds.x);
          }
          if (focused) assert(run.ink==(tab==selected?fui::Color::White:fui::Color::Black));
        }
      }
      keys.confirm=true;
      assert(lib.handleButtons());
      keys.confirm=false;
    }
    assert(lib.activeTab()==0 && lib.sortOrder==orders[0] && lib.filterUpdates==3);
    ui.metrics.buttonHintsHeight=0;
    for (int count:{0,56,65535}) {
      renderer.runs.clear();
      lib.rows=count;
      lib.drawHoldHelp(lib.drawPositionReadout());
      assert(renderer.runs.size()==(count==0?1:2));
      const auto& help=renderer.runs.back();
      assert(help.text==labels[STR_XUESU_LIBRARY_ACTIONS_HINT]);
      assert(help.rect.x>=SIDE_PADDING && help.rect.x+help.rect.width<=renderer.width-SIDE_PADDING);
      if (count>0) assert(help.rect.x+help.rect.width+6<=renderer.runs.front().rect.x);
    }
  }
}
'''
        helpers = library[library.index('constexpr unsigned long LONG_PRESS_MS'):
                          library.index('}  // namespace')]
        methods = ('LibraryListActivity::LibraryListActivity(', 'void LibraryListActivity::stepTab(',
                   'void LibraryListActivity::selectTab(', 'int LibraryListActivity::activeTab(',
                   'int LibraryListActivity::tabCount(', 'const char* LibraryListActivity::tabLabel(',
                   'fui::TabIndicator LibraryListActivity::tabIndicator(', 'bool LibraryListActivity::handleButtons(',
                   'int LibraryListActivity::drawPositionReadout(', 'void LibraryListActivity::drawHoldHelp(')
        implementation = '\n'.join([helpers, *(method(library, marker) for marker in methods),
                                    method(tab_ui, 'void UiTabListActivity::buildTabBar('),
                                    method(theme_ui, 'void UITheme::drawCenteredText('),
                                    method(theme_ui, 'void UITheme::drawCenteredWrappedText(')])
        side_padding = re.search(r'constexpr int SIDE_PADDING = \d+;', library)[0]
        sdk = ROOT / 'freeink-sdk/libs/ui/FreeInkUI'
        font = ROOT / 'lib/EpdFont'
        utf8 = ROOT / 'lib/Utf8'
        run_cpp(program.replace('@LABELS@', ','.join(labels)).replace('@PRODUCTION@', implementation)
                .replace('@SIDE_PADDING@', side_padding),
                include_dirs=[ROOT / 'src', sdk / 'include', font, utf8, ROOT / 'lib/MiniBidi'],
                defines=['FREEINK_DEVICE_XUESU_300=1', 'FREEINK_CAP_TOUCH=0'],
                extra_sources=[utf8 / 'Utf8.cpp', sdk / 'src/FreeInkUI.cpp'])
