"""Check actual compact home layout, cover painting and three-key selection."""
import re
import unittest

from test_reading_ui_regressions import ROOT, method, run_cpp


def source(path):
    return (ROOT / path).read_text(encoding='utf-8')


class XuesuHomeThemesTest(unittest.TestCase):
    def test_settings_is_drawn_and_reachable_in_both_directions(self):
        base = 'src/components/themes/'
        themes = [('Base', 'Base'), ('lyra/Lyra', 'Lyra'), ('lyra/Lyra3Covers', 'Lyra3Covers'),
                  ('roundedraff/RoundedRaff', 'RoundedRaff'), ('lyra/LyraCarousel', 'LyraCarousel')]
        headers = {name: source(base + path + 'Theme.h') for path, name in themes}
        metrics = method(headers['Base'], 'struct ThemeMetrics') + ';\n'
        for name, header in headers.items():
            metrics += header[header.index(f'namespace {name}Metrics'):header.index('\nclass ', header.index(f'namespace {name}Metrics'))] + '\n'
        drawing = ''
        for path, name in themes:
            theme = source(base + path + 'Theme.cpp')
            if name == 'LyraCarousel':
                for key in ('kCenterCoverWidth', 'kCenterCoverHeight', 'kCornerRadius', 'kActiveOutlineWidth',
                            'kDotSize', 'kDotGap', 'kMenuIconSize', 'kMenuIconPadding', 'kMenuHighlightPadding'):
                    drawing += re.search(r'constexpr int ' + key + r' = [^;]+;', theme).group() + '\n'
                drawing += method(theme, 'struct CompactCarousel') + ';\n'
                for marker in ('CompactCarousel compactCarousel(', 'void drawCover(', 'void drawAppsMenuIcon('):
                    drawing += method(theme, marker) + '\n'
            elif name == 'Lyra3Covers':
                for key in ('hPaddingInSelection', 'cornerRadius'):
                    drawing += re.search(r'constexpr int ' + key + r' = [^;]+;', theme).group() + '\n'
                for marker in ('int compactTileWidth(', 'int coverHeightFor('):
                    drawing += method(theme, marker) + '\n'
            elif name == 'Base':
                for marker in ('drawCompactHomeText(', 'drawCompactBookPreview(', 'drawCompactHomeMenu('):
                    drawing += method(theme, f'void BaseTheme::{marker}') + '\n'
            elif name == 'Lyra':
                drawing += method(theme, 'void LyraTheme::drawEmptyRecents(') + '\n'
            if name != 'Lyra3Covers':
                drawing += method(theme, f'void {name}Theme::drawHomeMenu(') + '\n'
            drawing += method(theme, f'int {name}Theme::homeCoverThumbHeight(') + '\n'
            drawing += method(theme, f'void {name}Theme::drawRecentBookCover(') + '\n'

        home = source('src/activities/home/HomeActivity.cpp')
        entries = home[home.index('namespace {'):home.index('int HomeActivity::getMenuItemCount()')]
        loop = method(home, 'void HomeActivity::loop()').split('  const auto swipe')[0] + '}\n'
        render = method(home, 'void HomeActivity::render(')
        geometry = render[render.index('  Rect headerRect'):render.index('  auto drawHeader')]
        menu = render[render.index('  auto drawMenu'):render.index('  const CarouselUpdateScope')]
        ids = sorted(set(re.findall(r'StrId::(STR_\w+)', entries + menu)) | {'STR_NO_OPEN_BOOK', 'STR_START_READING'})
        font_sizes = {}
        for size in (8, 10, 12):
            name = f'ubuntu_{size}_regular'
            font = source(f'lib/EpdFont/builtinFonts/{name}.h')
            data = method(font, f'EpdFontData {name} =')
            font_sizes[size] = data[data.index('{') + 1:].split(',')[4].strip()
        program = r'''
#include "components/CompactHomeLayout.h"
#include <FreeInkUICore.h>
#include <cassert>
#include <climits>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
enum class Color { White, Black, LightGray };
struct EpdFontFamily { enum Style { REGULAR, BOLD }; };
constexpr int UI_10_FONT_ID = 10, UI_12_FONT_ID = 12, SMALL_FONT_ID = 8;
enum class StrId { @IDS@ };
enum class HomeMenuItem { NONE, FILE_BROWSER, RECENTS, LIBRARY, OPDS_BROWSER, FILE_TRANSFER, SETTINGS_MENU, APPS };
enum UIIcon { Folder, Library, Blocks, Transfer, Settings, Apps, Recent, Book, Plugins };
constexpr auto STR_NO_OPEN_BOOK=StrId::STR_NO_OPEN_BOOK, STR_START_READING=StrId::STR_START_READING;
constexpr auto STR_CONTINUE_READING=StrId::STR_CONTINUE_READING;
struct {
  const char* get(StrId id) const {
    if (id == StrId::STR_SETTINGS_TITLE) return "Settings";
    if (id == StrId::STR_FILE_TRANSFER) return "Really long file transfer menu label across the width of the screen";
    if (id == STR_NO_OPEN_BOOK) return "No open book";
    if (id == STR_START_READING) return "Start reading";
    return "Entry";
  }
} I18N;
const char* tr(StrId id){return I18N.get(id);}
struct RecentBook { std::string path,title,author,coverBmpPath; };
struct HalFile {};
struct {
  bool openFileForRead(const char*, const std::string& path, HalFile&) const { return path == "cover"; }
} Storage;
enum class BmpReaderError { Ok };
struct Bitmap {
  explicit Bitmap(const HalFile&) {}
  BmpReaderError parseHeaders() const {return BmpReaderError::Ok;}
  int getWidth() const {return 80;}
  int getHeight() const {return 120;}
};
constexpr uint8_t CoverIcon[]={0};
struct GfxRenderer {
  int w=300,h=400;
  Rect menu;
  bool brokenBitmap=false;
  mutable bool settingsDrawn=false;
  mutable int placeholders=0, art=0, artReads=0, masks=0, outlines=0;
  mutable std::vector<Rect> hanLines;
  mutable Rect clip;
  struct ClipScope {
    const GfxRenderer& renderer;
    Rect prior;
    ClipScope(const GfxRenderer& r,int x,int y,int w,int h):renderer(r),prior(r.clip) {r.clip=Rect{x,y,w,h};}
    ~ClipScope(){renderer.clip=prior;}
  };
  void check(Rect r) const {
    assert(r.x>=menu.x && r.y>=menu.y && r.width>0 && r.height>0);
    assert(r.x+r.width<=menu.x+menu.width && r.y+r.height<=menu.y+menu.height);
  }
  int getScreenWidth() const {return w;}
  int getScreenHeight() const {return h;}
  int getLineHeight(int font) const {return font==UI_12_FONT_ID?@FONT12@:font==UI_10_FONT_ID?@FONT10@:@FONT8@;}
  int getTextWidth(int font,const char* s,EpdFontFamily::Style=EpdFontFamily::REGULAR) const {
    int width=0;
    for (const auto* p=reinterpret_cast<const unsigned char*>(s); *p; ++p) {
      if (*p < 0x80) width += 8;
      else if ((*p & 0xc0) == 0xc0) width += getLineHeight(font)-3;
    }
    return width;
  }
  void drawText(int font,int x,int y,const char* s,bool=true,EpdFontFamily::Style style=EpdFontFamily::REGULAR) const {
    if (!*s) return;
    Rect run{x,y,getTextWidth(font,s,style),getLineHeight(font)};
    check(run);
    assert(run.x>=clip.x && run.y>=clip.y && run.x+run.width<=clip.x+clip.width && run.y+run.height<=clip.y+clip.height);
    if (static_cast<unsigned char>(*s)>=0x80) hanLines.push_back(run);
    settingsDrawn |= std::string(s)=="Settings";
  }
  void fillRect(int x,int y,int w,int h,bool=true) const {check(Rect{x,y,w,h});}
  void drawRect(int x,int y,int w,int h) const {fillRect(x,y,w,h);}
  void fillRoundedRect(int x,int y,int w,int h,int,Color) const {fillRect(x,y,w,h);}
  void fillRoundedRect(int x,int y,int w,int h,int,bool,bool,bool,bool,Color) const {fillRect(x,y,w,h);}
  void drawRoundedRect(int x,int y,int w,int h,int,int,bool) const {fillRect(x,y,w,h);++outlines;}
  void drawIcon(const uint8_t*,int x,int y,int size) const {fillRect(x,y,size,size);}
  void drawIconInverted(const uint8_t* bits,int x,int y,int size) const {drawIcon(bits,x,y,size);}
  void maskRoundedRectOutsideCorners(int x,int y,int w,int h,int,Color) const {fillRect(x,y,w,h);++masks;}
  bool drawBitmap(const Bitmap&,int x,int y,int w,int h) const {
    check(Rect{x,y,w,h}); ++artReads;
    if (brokenBitmap) return false;
    ++art;
    return true;
  }
};
namespace freeink::ui {
struct GfxRendererTarget:DrawTarget {
  static constexpr FontId FONT_BODY=1;
  const GfxRenderer& renderer;
  int fontId=0;
  explicit GfxRendererTarget(const GfxRenderer& renderer):renderer(renderer) {}
  void setFont(FontId,int font){fontId=font;}
  Size measureText(FontId,const char* text,TextStyle style) const override {
    return {static_cast<int16_t>(renderer.getTextWidth(fontId,text,style.bold?EpdFontFamily::BOLD:EpdFontFamily::REGULAR)),
            lineHeight(FONT_BODY)};
  }
  int16_t lineHeight(FontId) const override {return renderer.getLineHeight(fontId);}
  void fill(Rect,Paint,uint8_t,uint8_t) override {}
  void stroke(Rect,Paint,uint8_t,uint8_t,uint8_t) override {}
  void line(Point,Point,uint8_t,Paint) override {}
  void triangle(Point,Point,Point,Paint) override {}
  void text(Rect,const char*,TextStyle) override {}
  void bitmap(Rect,BitmapRef,BitmapMode,Paint,Rotation) override {}
};
}
struct BaseTheme {
  enum class CompactHomeStyle { Classic, Lyra, RoundedRaff };
  using Labels=std::function<std::string(int)>;
  using Icons=std::function<UIIcon(int)>;
  virtual void drawHomeMenu(GfxRenderer&,Rect,int,int,const Labels&,const Icons&) const;
  virtual void drawRecentBookCover(GfxRenderer&,Rect,const std::vector<RecentBook>&,int,bool&,bool&,bool&,
                                   std::function<bool()>) const;
  virtual int homeCoverThumbHeight(const GfxRenderer&,Rect) const;
  static void drawCompactHomeMenu(GfxRenderer&,Rect,int,int,const Labels&,const Icons&,CompactHomeStyle,
                                  const uint8_t* (*iconBitmap)(UIIcon,int)=nullptr);
  static void drawCompactHomeText(const GfxRenderer&,Rect,int,const char*,int=1,bool=false,bool=false,bool=true);
  static void drawCompactBookPreview(GfxRenderer&,Rect,const std::vector<RecentBook>&,bool,bool&,bool&,bool,
                                     const std::function<bool()>&,CompactHomeStyle);
  static void drawCoverPlaceholder(const GfxRenderer& r,Rect cover){r.check(cover);++r.placeholders;}
  static bool drawCoverThumbFill(const GfxRenderer& r,const Bitmap& bmp,Rect cover){
    r.drawBitmap(bmp,cover.x,cover.y,cover.width,cover.height);return true;
  }
  void drawSideScrollBar(GfxRenderer&,Rect,int,int,int) const {}
};
struct LyraTheme:BaseTheme {
  void drawHomeMenu(GfxRenderer&,Rect,int,int,const Labels&,const Icons&) const override;
  void drawRecentBookCover(GfxRenderer&,Rect,const std::vector<RecentBook>&,int,bool&,bool&,bool&,
                           std::function<bool()>) const override;
  int homeCoverThumbHeight(const GfxRenderer&,Rect) const override;
  void drawEmptyRecents(const GfxRenderer&,Rect) const;
  static const uint8_t* iconForName(UIIcon,int) {static constexpr uint8_t icon=0;return &icon;}
};
struct Lyra3CoversTheme:LyraTheme {
  void drawRecentBookCover(GfxRenderer&,Rect,const std::vector<RecentBook>&,int,bool&,bool&,bool&,
                           std::function<bool()>) const override;
  int homeCoverThumbHeight(const GfxRenderer&,Rect) const override;
};
struct RoundedRaffTheme:BaseTheme {
  void drawHomeMenu(GfxRenderer&,Rect,int,int,const Labels&,const Icons&) const override;
  void drawRecentBookCover(GfxRenderer&,Rect,const std::vector<RecentBook>&,int,bool&,bool&,bool&,
                           std::function<bool()>) const override;
  int homeCoverThumbHeight(const GfxRenderer&,Rect) const override;
};
struct LyraCarouselTheme:LyraTheme {
  mutable int lastCenterIndex_=-1;
  void drawHomeMenu(GfxRenderer&,Rect,int,int,const Labels&,const Icons&) const override;
  void drawRecentBookCover(GfxRenderer&,Rect,const std::vector<RecentBook>&,int,bool&,bool&,bool&,
                           std::function<bool()>) const override;
  int homeCoverThumbHeight(const GfxRenderer&,Rect) const override;
};
@METRICS@
struct UITheme {
  const ThemeMetrics* metrics=&BaseMetrics::values;
  const BaseTheme* theme=nullptr;
  bool hasCarouselHome() const {return metrics==&LyraCarouselMetrics::values;}
  static UITheme& getInstance(){static UITheme singleton;return singleton;}
  static Rect getContentArea(const GfxRenderer& r){return Rect{0,0,r.w,r.h};}
  Rect getScreenSafeArea(const GfxRenderer& r) const {return getContentArea(r);}
  const ThemeMetrics& getMetrics() const{return *metrics;}
  static std::string getCoverThumbPath(const std::string& path,int height){assert(height>0);return path;}
};
@DRAWING@
#define GUI (*UITheme::getInstance().theme)
struct {bool standbyShortcutEnabled=false;} SETTINGS;
struct MappedInputManager {
  enum class Button {Confirm,Back,Left,Right,Up,Down};
  bool confirm=false;
  bool wasPressed(Button) const {return false;}
  bool wasReleased(Button b) const {return confirm && b==Button::Confirm;}
};
struct ButtonNavigator {
  int step=0;
  template<class F> void onNext(F f) const {if(step==1) f();}
  template<class F> void onPrevious(F f) const {if(step==-1) f();}
  template<class F> void onPressAndContinuous(std::initializer_list<MappedInputManager::Button>,F) const {}
  static int nextIndex(int,int);
  static int previousIndex(int,int);
};
struct CoverGrid {int selectedAction(const MappedInputManager&) const {return -1;}};
struct HomeActivity {
  enum class CarouselUpdateScope {Full,MenuOnly};
  GfxRenderer renderer;
  std::vector<RecentBook> recentBooks;
  CoverGrid* coverGridUi=nullptr;
  MappedInputManager mappedInput;
  ButtonNavigator buttonNavigator;
  bool hasContinueReading=false,hasPlugins=false;
  int selectorIndex=0,lastCarouselBookIndex=0,settingsOpens=0;
  bool hasLibrarySlot() const {return hasPlugins;}
  void onSettingsOpen(){++settingsOpens;}
  void onRecentBookSelected(const std::string&){}
  void onFileBrowserOpen(){} void onRecentsOpen(){} void onLibraryOpen(){} void onPluginsOpen(){}
  void onOpdsBrowserOpen(){} void onFileTransferOpen(){} void onAppsOpen(){}
  void requestUpdate(){} void requestCarouselUpdate(CarouselUpdateScope){}
  int getMenuItemCount() const;
  void loop();
  void drawMenuForTest();
};
@HOME@
void HomeActivity::drawMenuForTest() {
  const auto& metrics=UITheme::getInstance().getMetrics();
  const int pageWidth=renderer.w, pageHeight=renderer.h;
  const auto content=UITheme::getContentArea(renderer);
  const bool isCarousel=UITheme::getInstance().hasCarouselHome();
  const bool showContinueReading=metrics.homeContinueReadingInMenu&&!recentBooks.empty();
  const int menuItemCount=kHomeMenuItemCount-!hasLibrarySlot()+showContinueReading;
@GEOMETRY@
  assert(headerRect.y+headerRect.height+6<=coverRect.y);
  assert(coverRect.height>0 && coverRect.y+coverRect.height+6<=menuRect.y);
  assert(menuRect.y+menuRect.height<=pageHeight);
  if (!isCarousel) assert(menuPage.capacity==(pageWidth>pageHeight?3:4));
  renderer.menu=menuRect;
  drawMenu();

  renderer.menu=coverRect;
  assert(GUI.homeCoverThumbHeight(renderer, coverRect)>0);
  bool rendered=false, stored=false, restored=false;
  int stores=0;
  GUI.drawRecentBookCover(renderer, coverRect, recentBooks, 0, rendered, stored, restored,
                         [&stores] {++stores;return true;});
  if (recentBooks.empty()) return;

  // The long unbroken Chinese title gets two complete lines in each tile.
  const size_t tiles = UITheme::getInstance().metrics==&Lyra3CoversMetrics::values ? recentBooks.size() : 1;
  assert(renderer.hanLines.size()==2*tiles);
  const int art=renderer.art, placeholders=renderer.placeholders;
  const int expectedArt = recentBooks.front().coverBmpPath=="cover"?1:0;
  assert(art==expectedArt);
  if (isCarousel && expectedArt) {
    GfxRenderer corrupt;
    corrupt.w=renderer.w;corrupt.h=renderer.h;corrupt.menu=coverRect;corrupt.brokenBitmap=true;
    rendered=stored=restored=false;
    GUI.drawRecentBookCover(corrupt, coverRect, recentBooks, 0, rendered, stored, restored, []{return true;});
    assert(corrupt.artReads==1 && corrupt.art==0 && corrupt.masks==0 && corrupt.outlines==2);
  } else if (!isCarousel) {
    assert(stores==1 && stored && rendered);
    assert(placeholders==static_cast<int>(tiles)-expectedArt);
    restored=true;
    GUI.drawRecentBookCover(renderer, coverRect, recentBooks, selectorIndex, rendered, stored, restored,
                           [&stores] {++stores;return true;});
    assert(stores==1 && renderer.art==art && renderer.placeholders==placeholders);
  }
}
int main() {
  BaseTheme classic; LyraTheme lyra; RoundedRaffTheme raff; LyraCarouselTheme carousel; Lyra3CoversTheme trio;
  struct {const ThemeMetrics* metrics; const BaseTheme* theme;} variants[]={
    {&BaseMetrics::values,&classic},{&LyraMetrics::values,&lyra},{&Lyra3CoversMetrics::values,&trio},
    {&RoundedRaffMetrics::values,&raff},{&LyraCarouselMetrics::values,&carousel}};
  for(const auto variant:variants) for(bool landscape:{false,true}) for(bool books:{false,true})
  for(bool missingCover:{false,true}) for(bool plugins:{false,true}) for(int direction:{-1,1}) {
    auto& ui=UITheme::getInstance(); ui.theme=variant.theme;ui.metrics=variant.metrics;
    HomeActivity home; home.hasPlugins=plugins;
    if(books) {
      const std::string title = "\u4e66\u540d\u6d4b\u8bd5\u7684\u957f\u4e2d\u6587\u5b57\u4e32";
      for (int i=0;i<variant.metrics->homeRecentBooksCount;++i) {
        const char* cover = i==0&&!missingCover?"cover":"missing";
        home.recentBooks.push_back({"book",title+title+title+title,"A long author name",cover});
      }
    }
    if(landscape) {home.renderer.w=400;home.renderer.h=300;}
    home.buttonNavigator.step=direction;
    const int settingsIndex=static_cast<int>(home.recentBooks.size())+
        menuItemToIndex(HomeMenuItem::SETTINGS_MENU,plugins,ui.hasCarouselHome());
    for(int i=0;home.selectorIndex!=settingsIndex && i<home.getMenuItemCount();++i) home.loop();
    assert(home.selectorIndex==settingsIndex);
    home.drawMenuForTest();
    assert(home.renderer.settingsDrawn);
    home.buttonNavigator.step=0;home.mappedInput.confirm=true;home.loop();
    assert(home.settingsOpens==1);
  }
  // Grid home uses the same menu order and confirm action as the list home.
  UITheme::getInstance().metrics=&LyraMetrics::values;
  for(int bookCount:{0,7}) {
    HomeActivity home; CoverGrid grid;home.coverGridUi=&grid;home.recentBooks.resize(bookCount);
    home.buttonNavigator.step=1;
    for(int i=0;i<bookCount+3;++i) home.loop();
    home.buttonNavigator.step=0;home.mappedInput.confirm=true;home.loop();
    assert(home.settingsOpens==1);
  }
}
'''
        navigator = source('src/util/ButtonNavigator.cpp')
        replacements = {
            'IDS': ','.join(ids), 'METRICS': metrics, 'DRAWING': drawing,
            'GEOMETRY': geometry + menu,
            'HOME': entries + method(home, 'int HomeActivity::getMenuItemCount()') + loop + '\n'.join(
                method(navigator, f'int ButtonNavigator::{direction}Index(') for direction in ('next', 'previous')),
        }
        replacements.update({f'FONT{size}': height for size, height in font_sizes.items()})
        for name, value in replacements.items():
            program = program.replace('@' + name + '@', value)
        sdk = ROOT / 'freeink-sdk/libs/ui/FreeInkUI/include'
        run_cpp(program, include_dirs=[ROOT / 'src', sdk], defines=['FREEINK_DEVICE_XUESU_300=1'])

    def test_display_picker_stays_selected_as_theme_specific_rows_change(self):
        settings = source('src/activities/settings/SettingsActivity.cpp')
        run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>
enum class SettingsSection { End, Display };
struct CrossPointSettings {uint8_t uiTheme=5,other=0;};
struct UITheme {
  bool hasMainTabs() const {return inx;}
  void reload(){inx=!inx;}
  bool inx=true;
  static UITheme& getInstance(){static UITheme singleton;return singleton;}
};
struct RenderLock {explicit RenderLock(auto&) {}};
struct {
  void goToSettings(SettingsSection) {assert(false);}
} activityManager;
struct SettingsActivity {
  struct Row {uint8_t CrossPointSettings::*valuePtr;};
  SettingsSection section=SettingsSection::Display;
  std::vector<Row> rows;
  const std::vector<Row>* currentSettings=&rows;
  int expandedCategories=0;
  struct {int selected=-1;void reset(){selected=0;}} nav;
  void clearContentSelection(){}
  void resetUi(){}
  void rebuildSettingsLists(){
    rows.assign(UITheme::getInstance().hasMainTabs()?10:7,Row{&CrossPointSettings::other});
    rows.push_back({&CrossPointSettings::uiTheme});
  }
  void moveSelectionTo(int row){nav.selected=row;}
  void applyUiSettingChange(uint8_t CrossPointSettings::*);
};
''' + method(settings, 'void SettingsActivity::applyUiSettingChange(') + r'''
int main() {
  SettingsActivity display;
  for(int i=0;i<12;++i) {
    display.applyUiSettingChange(&CrossPointSettings::uiTheme);
    assert(display.nav.selected==static_cast<int>(display.rows.size())-1);
    assert(display.rows.at(display.nav.selected).valuePtr==&CrossPointSettings::uiTheme);
  }
}
''', defines=['FREEINK_DEVICE_XUESU_300=1'])
