"""Exercise the production three-button actions and small-screen layout paths."""
import re
import unittest

from test_reading_ui_regressions import ROOT, method, run_cpp


def source(path):
    return (ROOT / path).read_text(encoding='utf-8')


class XuesuReviewFixesTest(unittest.TestCase):
    def test_quick_delete_only_offers_selected_books(self):
        recent = source('src/activities/home/RecentBooksActivity.cpp')
        stats = source('src/activities/apps/reading-stats/ReadingStatsActivity.cpp')
        run_cpp(r'''
#include <cassert>
#include <vector>
constexpr const char* STR_REMOVE_FROM_RECENTS="recent", *STR_DELETE_STATS_ENTRY="stats";
const char* tr(const char* text) {return text;}
struct RecentBooksActivity {
  struct {int selected=0;} nav;
  int count=2,prompts=0,prompted=-1;
  bool focused=true;
  bool showMainTabContentSelection() const {return focused;}
  int listCount() const {return count;}
  void onRowLongPress(int index) {++prompts;prompted=index;}
  const char* xuesuQuickActionLabel() const;
  void runXuesuQuickAction();
};
struct {std::vector<int> books{1,2}; const auto& getBooks() const {return books;}} READING_STATS;
struct ReadingStatsActivity {
  int selectedIndex=1,prompts=0;
  bool focused=true;
  bool showMainTabContentSelection() const {return focused;}
  void confirmRemoveSelectedBook() {++prompts;}
  const char* xuesuQuickActionLabel() const;
  void runXuesuQuickAction();
};
''' + '\n'.join(method(text, signature) for text, signature in (
            (recent, 'const char* RecentBooksActivity::xuesuQuickActionLabel()'),
            (recent, 'void RecentBooksActivity::runXuesuQuickAction()'),
            (stats, 'const char* ReadingStatsActivity::xuesuQuickActionLabel()'),
            (stats, 'void ReadingStatsActivity::runXuesuQuickAction()'))) + r'''
int main() {
  for(bool focus:{false,true}) for(int count:{0,2}) for(int selected:{-1,0,1,2,3}) {
    RecentBooksActivity recent; recent.focused=focus;recent.count=count;recent.nav.selected=selected;
    const bool valid=focus && selected>=0 && selected<count;
    assert(bool(recent.xuesuQuickActionLabel())==valid);
    recent.runXuesuQuickAction();assert(recent.prompts==int(valid));
    if(valid) assert(recent.prompted==selected);
    READING_STATS.books.resize(count);
    ReadingStatsActivity stats;stats.focused=focus;stats.selectedIndex=selected;
    const bool book=focus && selected>0 && selected<=count;
    assert(bool(stats.xuesuQuickActionLabel())==book);
    stats.runXuesuQuickAction();assert(stats.prompts==int(book));
  }
}
''')

    def test_cache_return_consumes_physical_hold_release_once(self):
        sdk = source('freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp')
        settings = source('src/activities/settings/SettingsActivity.cpp')
        clear = source('src/activities/settings/ClearCacheActivity.cpp')
        lists = source('src/activities/UiListActivity.cpp')
        callback = method(settings, 'auto resultHandler =') + '; resultHandler({});'
        run_cpp(r'''
#include <cassert>
#include <cstdint>
namespace BoardConfig {struct {struct {int confirm=17;} input;} ACTIVE;}
struct InputManager {
  static constexpr int BTN_BACK=0,BTN_CONFIRM=1,BTN_POWER=6;
  static constexpr unsigned long CONFIRM_BACK_HOLD_MS=650;
  bool physical=false,confirmBackPhysicalPressed=false,confirmBackLongPressActive=false;
  unsigned long confirmBackPressStart=0,buttonPressStart=0,buttonPressFinish=0,powerButtonPressStart=0,powerButtonPressFinish=0;
  uint8_t pressedEvents=0,releasedEvents=0,currentState=0,lastState=0;
  bool isDigitalPressed(int) {return physical;}
  uint8_t getDigitalState() {return 0;}
  void updatePhysicalPressed(uint8_t,unsigned long) {}
  void applyStateChange(uint8_t,unsigned long);
  void updateConfirmBackHold(unsigned long);
};
''' + method(sdk, 'void InputManager::applyStateChange(') + method(sdk, 'void InputManager::updateConfirmBackHold(') + r'''
struct MappedInputManager {
  enum class Button {Back=0,Confirm=1};
  InputManager* input;
  bool wasPressed(Button b) const {return input->pressedEvents & (1u<<int(b));}
  bool wasReleased(Button b) const {return input->releasedEvents & (1u<<int(b));}
  bool isPressed(Button b) const {return input->currentState & (1u<<int(b));}
  bool wasScreenTapped(int&,int&) {return false;}
};
struct Popup {
  bool isActive() const {return false;}
  template<class F> bool handleInput(MappedInputManager&,F) {return false;}
};
struct ClearCacheActivity {
  MappedInputManager mappedInput;
  enum State {WARNING,CLEARING,SUCCESS,FAILED};
  State state=SUCCESS;
  int finishes=0;
  Popup confirmPopup;
  void goBack() {++finishes;}
  void requestUpdate() {}
  void loop();
};
struct UiListActivity {
  MappedInputManager mappedInput;
  int finishes=0,activations=0;
  struct Nav {int selected=0;} nav;
  Nav& activeNav() {return nav;}
  int listCount() const {return 3;}
  void activateIndex(int) {++activations;}
  void onBackButton() {++finishes;}
  bool handleButtons();
};
struct ActivityResult {};
struct {void saveToFile() {}} SETTINGS;
struct SettingsActivity : UiListActivity {
  bool waitForBackRelease=false,waitForConfirmRelease=false;
  Popup optionPopup;
  void requestUpdate() {}
  void rebuildSettingsLists() {}
  bool handleCustomInput();
  void returned() {
''' + callback + r'''
  }
  void tick() {if(!handleCustomInput()) handleButtons();}
};
''' + method(clear, 'void ClearCacheActivity::loop()') + method(lists, 'bool UiListActivity::handleButtons()') + method(settings, 'bool SettingsActivity::handleCustomInput()') + r'''
int main() {
  InputManager input;
  ClearCacheActivity child{{&input}};
  SettingsActivity parent;parent.mappedInput.input=&input;
  input.physical=true;input.updateConfirmBackHold(100);
  input.updateConfirmBackHold(750);child.loop();parent.returned();parent.tick();
  assert(child.finishes==1 && parent.finishes==0);
  input.physical=false;input.updateConfirmBackHold(1000);parent.tick();
  assert(parent.finishes==0 && parent.activations==0);
  input.updateConfirmBackHold(1010);parent.tick();
  input.physical=true;input.updateConfirmBackHold(1100);parent.tick();
  input.updateConfirmBackHold(1750);parent.tick();
  input.physical=false;input.updateConfirmBackHold(1800);parent.tick();
  assert(parent.finishes==1); // A new physical gesture still works.
  input.pressedEvents=input.releasedEvents=0;input.currentState=2;
  parent.returned();parent.tick();input.currentState=0;input.releasedEvents=2;parent.tick();
  assert(parent.activations==0); // A child returning on Confirm press is safe too.
  input.releasedEvents=0;parent.tick();input.releasedEvents=2;parent.tick();
  assert(parent.activations==1);
}
''')

    def test_rename_removal_keeps_file_and_bookmark_actions_aligned(self):
        files = source('src/activities/home/FileBrowserActivity.cpp')
        bookmarks = source('src/activities/reader/EpubReaderBookmarksActivity.cpp')
        program = r'''
#include <cassert>
#include <functional>
#include <iterator>
#include <string>
#include <vector>
enum class StrId {STR_OPEN,STR_DELETE,STR_RENAME,STR_MOVE,STR_FILENAME,STR_BOOKMARKS};
struct Popup {
  std::vector<StrId> labels;
  std::function<void(int)> choose;
  bool isActive() const {return false;}
  template<class F> void show(StrId,const StrId* items,int count,int,F fn) {
    labels.assign(items,items+count);choose=fn;
  }
};
namespace FsHelpers {bool isProtectedPathComponent(const std::string& s) {return s=="protected";}}
std::string cleanEntryName(const std::string& s) {return s;}
struct FileBrowserActivity {
  enum class Mode {Books};Mode mode=Mode::Books;
  enum class BrowserState {Browsing};BrowserState browserState=BrowserState::Browsing;
  enum class EditAction {Open,Delete,Rename,Move};
  struct {int selected=0;} nav;
  std::vector<std::string> files{"book"};
  std::vector<EditAction> calls;
  Popup editPopup;
  int listCount() const {return files.size();}
  void executeEditAction(EditAction a) {calls.push_back(a);}
  void requestUpdate() {}
  void showEditMenu();
};
struct EpubReaderBookmarksActivity {
  std::vector<int> bookmarks{1};
  Popup confirmPopup;
  int opened=0,renamed=0,deleted=0;
  void openSelectedBookmark() {++opened;}
  void startRename() {++renamed;}
  void showDeleteConfirmation() {++deleted;}
  void requestUpdate() {}
  void showBookmarkActions();
};
''' + method(files, 'void FileBrowserActivity::showEditMenu()') + method(bookmarks, 'void EpubReaderBookmarksActivity::showBookmarkActions()') + r'''
int main() {
  FileBrowserActivity files;files.showEditMenu();
  const auto& f=files.editPopup;
  assert(f.labels.size()==(FREEINK_DEVICE_XUESU_300?3:4));
  f.choose(-1);f.choose(f.labels.size());assert(files.calls.empty());
  for(int i=0;i<int(f.labels.size());++i) {
    f.choose(i);
    using Action=FileBrowserActivity::EditAction;
    switch(f.labels[i]) {
      case StrId::STR_OPEN: assert(files.calls.back()==Action::Open);break;
      case StrId::STR_DELETE: assert(files.calls.back()==Action::Delete);break;
      case StrId::STR_MOVE: assert(files.calls.back()==Action::Move);break;
      case StrId::STR_RENAME: assert(!FREEINK_DEVICE_XUESU_300 && files.calls.back()==Action::Rename);break;
      default: assert(false);
    }
  }
  EpubReaderBookmarksActivity bookmarks;bookmarks.showBookmarkActions();
  const auto& b=bookmarks.confirmPopup;
  assert(b.labels.size()==(FREEINK_DEVICE_XUESU_300?2:3));
  b.choose(-1);b.choose(b.labels.size());
  assert(bookmarks.opened+bookmarks.renamed+bookmarks.deleted==0);
  for(int i=0;i<int(b.labels.size());++i) b.choose(i);
  assert(bookmarks.opened==1 && bookmarks.deleted==1 && bookmarks.renamed==!FREEINK_DEVICE_XUESU_300);
}
'''
        for xuesu in (0, 1):
            with self.subTest(xuesu=xuesu):
                run_cpp(program, defines=[f'FREEINK_DEVICE_XUESU_300={xuesu}'])

    def test_recent_list_rows_fit_fonts_and_preserve_pagination(self):
        run_cpp(r'''
#include <cassert>
#include "InxRecentLayout.h"
int main() {
  for(const Rect screen:{Rect{0,0,300,400},Rect{0,0,400,300},Rect{0,0,480,800},Rect{0,0,800,480}}) {
    for(bool bottom:{false,true}) for(int titleHeight:{24,28}) for(int authorHeight:{23,28}) {
      const auto tabs=MainTabs::layout(screen,10,bottom?56:52,bottom,20,true);
      const auto& content=tabs.content;
      const auto list=InxRecentGeometry::listLayout(content,titleHeight,authorHeight);
      assert(list.rows>=1 && list.rows<=5);
      assert(list.titleTop>=0 && list.authorTop-list.titleTop-titleHeight>=6);
      assert(list.progressTop-list.authorTop-authorHeight>=6);
      assert(list.progressTop+6<=list.rowHeight-6);
      assert(list.rows*list.rowHeight<=content.height);
      for(int count:{0,1,2,4,5,6,13}) for(int selected=0;selected<count;++selected) {
        const int start=InxRecentGeometry::pageStart(selected,count,InxRecentLayout::List,list.rows);
        assert(start<=selected && selected<start+list.rows);
        for(int slot=0;slot<list.rows;++slot) {
          const int index=InxRecentGeometry::indexFromPoint(content,content.x+1,content.y+slot*list.rowHeight+1,
                                                          selected,count,InxRecentLayout::List,list.rows);
          assert(index==(start+slot<count?start+slot:-1));
        }
      }
    }
  }
}
''', include_dirs=[ROOT / 'src', ROOT / 'lib/hal'], defines=['FREEINK_DEVICE_XUESU_300=1'])

    def test_extended_stats_render_reaches_charts_without_summary_overlap(self):
        extended = source('src/activities/apps/reading-stats/ReadingStatsExtendedActivity.cpp')
        constants = '\n'.join(re.findall(r'constexpr int [A-Z_]+ = [^;]+;', extended))
        layout = extended[extended.index('struct ExtendedStatsLayout {'):extended.index('void drawReadingChart(')]
        render = method(extended, 'void ReadingStatsExtendedActivity::render(')
        ids = sorted(set(re.findall(r'tr\((STR_\w+)\)', render)))
        program = r'''
#include <cassert>
#include <string>
#include <vector>
#include "components/themes/BaseTheme.h"
struct Draw {char kind;Rect rect;};
struct GfxRenderer {
  int width=300,height=400;
  mutable Rect clip{0,0,300,400};
  mutable std::vector<Draw> draws;
  struct ClipScope {
    const GfxRenderer& r;Rect old;
    ClipScope(const GfxRenderer& r,int x,int y,int w,int h):r(r),old(r.clip) {r.clip=Rect{x,y,w,h};assert(h>0);}
    ~ClipScope(){r.clip=old;}
  };
  int getScreenWidth() const {return width;}
  int getScreenHeight() const {return height;}
  void clearScreen() {draws.clear();clip=Rect{0,0,width,height};}
  void record(char kind,Rect rect) const {
    int right=std::min(rect.x+rect.width,clip.x+clip.width),bottom=std::min(rect.y+rect.height,clip.y+clip.height);
    rect.x=std::max(rect.x,clip.x);rect.y=std::max(rect.y,clip.y);
    rect.width=right-rect.x;rect.height=bottom-rect.y;
    if(rect.width>0 && rect.height>0) draws.push_back({kind,rect});
  }
  void fillRect(int x,int y,int w,int h,bool) {record('M',Rect{x,y,w,h});}
  void displayBuffer() {}
};
struct UITheme {
  ThemeMetrics metrics{};
  static UITheme& getInstance() {static UITheme value;return value;}
  const ThemeMetrics& getMetrics() const {return metrics;}
};
struct {
  void drawSubHeader(const GfxRenderer& r,Rect rect,const char*,const char*) {
    // INX subheaders replace the parent clip while drawing their text.
    const GfxRenderer::ClipScope clip(r,rect.x,rect.y,rect.width,rect.height);
    r.record('H',rect);
  }
  void drawButtonHints(const GfxRenderer&,const char*,const char*,const char*,const char*) {}
} GUI;
struct {
  uint64_t getRecentReadingMs(int) const {return 1;}
  uint64_t getTodayReadingMs() const {return 1;}
  uint64_t getTotalReadingMs() const {return 1;}
  int getCurrentStreakDays() const {return 1;}
  int getMaxStreakDays() const {return 1;}
  int getBooksFinishedCount() const {return 1;}
  int getBooksStartedCount() const {return 1;}
} READING_STATS;
namespace ReadingStatsAnalytics {std::string formatDurationHm(uint64_t) {return "1m";}}
namespace HeaderDateUtils {void drawHeaderWithDate(const GfxRenderer&,const char*,const char*) {}}
const char* tr(int) {return "label";}
uint64_t getDailyReadingGoalMs() {return 1;}
std::vector<int> getAnnualReadingBars(int& year) {year=2026;return {1};}
std::vector<int> getRecentDailyReadingBars() {return {1};}
std::string formatAnnualReadingTitle(int) {return "annual";}
void drawReadingChart(const GfxRenderer& r,Rect rect,const std::vector<int>&,bool daily) {r.record(daily?'D':'A',rect);}
void drawMetricCard(const GfxRenderer& r,Rect rect,const char*,const std::string&,bool=false) {r.record('S',rect);}
void drawRecentWindowCard(const GfxRenderer& r,Rect rect,const char*,const std::string&) {r.record('S',rect);}
struct RenderLock {};
struct ReadingStatsExtendedActivity {
  GfxRenderer renderer;
  int scrollOffset=0;
  struct {
    struct Labels {const char *btn1="",*btn2="",*btn3="",*btn4="";};
    Labels mapLabels(const char*,const char*,const char*,const char*) {return {};}
  } mappedInput;
  void render(RenderLock&&);
};
bool overlaps(Rect a,Rect b) {
  return a.x<b.x+b.width && b.x<a.x+a.width && a.y<b.y+b.height && b.y<a.y+a.height;
}
''' + '\n'.join(f'constexpr int {name}={index};' for index, name in enumerate(ids)) + constants + method(extended, 'int getScrollableContentBottom(') + layout + render + r'''
int main() {
  for(bool inx:{false,true}) for(int height:{300,400,800}) {
    auto& m=UITheme::getInstance().metrics;
    m.topPadding=10;m.headerHeight=inx?66:40;m.verticalSpacing=inx?0:8;
    m.buttonHintsHeight=inx?0:40;m.contentSidePadding=20;
    ReadingStatsExtendedActivity page;
    page.renderer.height=height;page.renderer.width=height==300?400:300;
    const auto layout=extendedStatsLayout(page.renderer,m);
    assert(layout.viewportBottom>layout.viewportTop);
    assert(layout.scrollSummary==(height<800));
    bool daily=false,annual=false,summary=false;
    for(int offset=0;offset<=layout.maxScrollOffset+CHART_SCROLL_STEP;offset+=CHART_SCROLL_STEP) {
      page.scrollOffset=offset;page.render({});
      // Replay paints, including masks, to catch nested theme clips escaping
      // into the spaces between pinned cards (plain rectangle checks miss it).
      const int width=page.renderer.width;
      std::vector<char> pixels(width*height,0);
      for(const auto& a:page.renderer.draws) {
        for(int y=std::max(0,a.rect.y);y<std::min(height,a.rect.y+a.rect.height);++y)
          for(int x=std::max(0,a.rect.x);x<std::min(width,a.rect.x+a.rect.width);++x)
            pixels[y*width+x]=a.kind;
        daily|=a.kind=='D';annual|=a.kind=='A';summary|=a.kind=='S';
        if(a.kind=='D'||a.kind=='A') {
          assert(a.rect.y>=layout.viewportTop && a.rect.y+a.rect.height<=layout.viewportBottom);
          for(const auto& b:page.renderer.draws) if(b.kind=='S'||b.kind=='M') assert(!overlaps(a.rect,b.rect));
        }
      }
      if(!layout.scrollSummary) {
        for(int y=layout.contentTop;y<layout.viewportTop;++y) for(int x=0;x<width;++x)
          assert(pixels[y*width+x]!='H' && pixels[y*width+x]!='D' && pixels[y*width+x]!='A');
      }
    }
    assert(daily && annual && summary);
    assert(page.scrollOffset==layout.maxScrollOffset);
    bool bottomReached=false;
    for(const auto& d:page.renderer.draws) if(d.kind=='A') bottomReached=d.rect.y+d.rect.height==layout.viewportBottom;
    assert(bottomReached);
    page.scrollOffset=-1;page.render({});assert(page.scrollOffset==0);
  }
}
'''
        run_cpp(program, include_dirs=[ROOT / 'src', ROOT / 'lib/hal'])


if __name__ == '__main__':
    unittest.main()
