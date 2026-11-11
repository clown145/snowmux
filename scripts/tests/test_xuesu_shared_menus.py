"""Run the legacy Classic and Lyra button menus in small app viewports."""
import re
import unittest

from test_reading_ui_regressions import ROOT, method, run_cpp


class XuesuSharedMenusTest(unittest.TestCase):
    def test_app_menu_keeps_the_focused_row_visible(self):
        themes = ROOT / 'src/components/themes'
        base = (themes / 'BaseTheme.h').read_text(encoding='utf-8')
        lyra = (themes / 'lyra/LyraTheme.h').read_text(encoding='utf-8')
        sources = [(themes / path).read_text(encoding='utf-8')
                   for path in ('BaseTheme.cpp', 'lyra/LyraTheme.cpp')]
        metrics = method(base, 'struct ThemeMetrics') + ';\n'
        for name, header in (('Base', base), ('Lyra', lyra)):
            metrics += method(header, f'namespace {name}Metrics') + '\n'
        constants = '\n'.join(re.search(rf'constexpr int {name} = [^;]+;', sources[1]).group()
                              for name in ('cornerRadius', 'mainMenuIconSize', 'hPaddingInSelection'))
        subpage = (ROOT / 'src/components/SubpageLayout.h').read_text(encoding='utf-8')
        program = r'''
#include "components/Rect.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
constexpr int UI_10_FONT_ID=10, UI_12_FONT_ID=12;
enum class Color { LightGray };
enum class UIIcon { Book };
@METRICS@
@SUBPAGE@
@CONSTANTS@
struct GfxRenderer {
  Rect bounds;
  mutable std::vector<Rect> selected;
  mutable std::vector<std::string> drawn;
  mutable int scrollStart=-1, scrollCapacity=0;
  void check(int x,int y,int w,int h) const {
    if (!FREEINK_DEVICE_XUESU_300) return;
    assert(x>=bounds.x && y>=bounds.y && w>0 && h>0);
    assert(x+w<=bounds.x+bounds.width && y+h<=bounds.y+bounds.height);
  }
  void fillRect(int x,int y,int w,int h) const {check(x,y,w,h); selected.emplace_back(x,y,w,h);}
  void fillRoundedRect(int x,int y,int w,int h,int,Color) const {fillRect(x,y,w,h);}
  void drawRect(int x,int y,int w,int h) const {check(x,y,w,h);}
  void drawIcon(const uint8_t*,int x,int y,int size) const {check(x,y,size,size);}
  int getLineHeight(int id) const {return id==UI_10_FONT_ID?23:29;}
  int getTextWidth(int,const char* text) const {return static_cast<int>(std::string(text).size())*9;}
  std::string truncatedText(int,const char* text,int width) const {return std::string(text).substr(0,width/9);}
  void drawText(int id,int x,int y,const char* text,bool) const {
    check(x,y,getTextWidth(id,text),getLineHeight(id)); drawn.emplace_back(text);
  }
};
struct BaseTheme {
  @ROWS@;
  using Labels=std::function<std::string(int)>;
  using Icons=std::function<UIIcon(int)>;
  static MenuRowGeometry buttonMenuRows(const Rect&,const ThemeMetrics&,int,int,int,int);
  virtual MenuRowGeometry getMenuRowGeometry(const GfxRenderer&,const Rect&,int,int) const;
  virtual void drawButtonMenu(GfxRenderer&,Rect,int,int,const Labels&,const Icons&,int=-1) const;
  void drawSideScrollBar(GfxRenderer& renderer,Rect,int count,int start,int capacity) const {
    if(capacity>0 && count>capacity) { renderer.scrollStart=start; renderer.scrollCapacity=capacity; }
  }
};
struct LyraTheme : BaseTheme {
  MenuRowGeometry getMenuRowGeometry(const GfxRenderer&,const Rect&,int,int) const override;
  void drawButtonMenu(GfxRenderer&,Rect,int,int,const Labels&,const Icons&,int=-1) const override;
  const uint8_t* iconForName(UIIcon,int) const {static constexpr uint8_t bits=0; return &bits;}
};
@IMPLEMENTATIONS@
int main() {
  BaseTheme classic;
  LyraTheme lyra;
  for (bool isLyra : {false,true}) for (bool landscape : {false,true}) for (bool manage : {false,true}) {
    const BaseTheme& theme=isLyra?static_cast<const BaseTheme&>(lyra):classic;
    const auto& metrics=isLyra?LyraMetrics::values:BaseMetrics::values;
    const Rect safe{0,0,landscape?400:300,landscape?300:400};
    const auto bounds=SubpageLayout::contentRect(safe,metrics,manage);
    const int count=manage?3:4;
    for (int focus=-1;focus<count;++focus) {
      GfxRenderer renderer{bounds,{}, {}};
      const auto rows=theme.getMenuRowGeometry(renderer,bounds,focus,count);
      theme.drawButtonMenu(renderer,bounds,count,focus,
                           [](int index) {return std::to_string(index)+std::string(40,'x');},
                           [](int) {return UIIcon::Book;});
      assert(static_cast<int>(renderer.drawn.size())==rows.pageCount);
      for (int i=0;i<rows.pageCount;++i) assert(renderer.drawn[i].front()=='0'+rows.pageStart+i);
      if(focus>=0) {
        assert(focus>=rows.pageStart && focus<rows.pageStart+rows.pageCount);
        assert(renderer.selected.size()==1);
        const auto& selected=renderer.selected.front();
        assert(selected.y==rows.firstRowY+(focus-rows.pageStart)*rows.rowStep);
        assert(selected.height==rows.rowHeight && selected.x==rows.xStart);
      }
      if (FREEINK_DEVICE_XUESU_300 && (landscape || (isLyra && !manage))) assert(rows.pageCapacity<count);
      if (FREEINK_DEVICE_XUESU_300 && rows.pageCapacity<count) {
        assert(renderer.scrollStart==rows.pageStart && renderer.scrollCapacity==rows.pageCapacity);
      }
      if (!FREEINK_DEVICE_XUESU_300) assert(rows.pageStart==0 && rows.pageCount==count);
    }
  }
}
'''
        methods = [method(sources[0], 'BaseTheme::MenuRowGeometry BaseTheme::buttonMenuRows(')]
        for name, cpp in zip(('Base', 'Lyra'), sources):
            methods.append(method(cpp, f'{name}Theme::MenuRowGeometry {name}Theme::getMenuRowGeometry('))
            methods.append(method(cpp, f'void {name}Theme::drawButtonMenu('))
        replacements = {'METRICS': metrics, 'CONSTANTS': constants, 'IMPLEMENTATIONS': '\n'.join(methods),
                        'SUBPAGE': method(subpage, 'namespace SubpageLayout'),
                        'ROWS': method(base, 'struct MenuRowGeometry')}
        for key, value in replacements.items():
            program = program.replace('@' + key + '@', value)
        for xuesu in (0, 1):
            with self.subTest(xuesu=xuesu):
                run_cpp(program, include_dirs=[ROOT / 'src'], defines=[f'FREEINK_DEVICE_XUESU_300={xuesu}'])
