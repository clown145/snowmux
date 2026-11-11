"""Exercise the compact settings and option geometry with FreeInkUI's real drawing."""
import re
import unittest

from test_reading_ui_regressions import ROOT, method, run_cpp


FONTS = ROOT / 'lib/EpdFont/builtinFonts'
SDK = ROOT / 'freeink-sdk/libs/ui/FreeInkUI'


def line_height(font):
    source = (FONTS / f'{font}.h').read_text(encoding='utf-8')
    return int(re.search(rf'EpdFontData {font} = \{{(?:\s*[^,]+,){{4}}\s*(\d+),', source)[1])


def font_advances(font, intervals_file, characters):
    source = (FONTS / f'{font}.h').read_text(encoding='utf-8')
    glyphs = source.split(f'{font}Glyphs[] = {{', 1)[1].split('};', 1)[0]
    advances = [int(value) for value in re.findall(r'\{\s*\d+,\s*\d+,\s*(\d+),', glyphs)]
    intervals = (FONTS / f'{intervals_file}.h').read_text(encoding='utf-8')
    interval_table = intervals.split('Intervals[] = {', 1)[1].split('};', 1)[0]
    ranges = [tuple(int(value, 16) for value in group) for group in
              re.findall(r'\{\s*(0x[\da-fA-F]+),\s*(0x[\da-fA-F]+),\s*(0x[\da-fA-F]+)', interval_table)]
    result = {}
    for code in sorted(map(ord, characters)):
        for first, last, index in ranges:
            if first <= code <= last:
                result[code] = (advances[index + code - first] + 8) // 16
                break
    return result


class XuesuThemeSettingsTest(unittest.TestCase):
    def test_compact_options_keep_every_focus_target_and_long_label_visible(self):
        popup = (ROOT / 'src/components/OptionPopup.h').read_text(encoding='utf-8')

        program = r'''
#include <FreeInkUI.h>
#include <array>
#include <cassert>
#include <functional>
#include <string>
#include <vector>

#include "components/XuesuOptionLayout.h"

namespace fui = freeink::ui;
using fui::Rect;
constexpr int16_t SMALL_LINE = @SMALL_LINE@;
constexpr int16_t BODY_LINE = @BODY_LINE@;

struct Target : fui::DrawTarget {
  Rect viewport{};
  std::vector<Rect> inkLines;
  int16_t lineHeight(fui::FontId font) const override {
    return font == fui::FONT_SLOT_SMALL ? SMALL_LINE : BODY_LINE;
  }
  fui::Size measureText(fui::FontId font, const char* text, fui::TextStyle) const override {
    int width = 0;
    for (auto p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
      if ((*p & 0xc0) != 0x80) width += *p < 0x80 ? lineHeight(font) / 2 : lineHeight(font) - 4;
    }
    return {static_cast<int16_t>(width), lineHeight(font)};
  }
  void inside(Rect r) const {
    assert(r.x >= viewport.x && r.y >= viewport.y && r.width >= 0 && r.height >= 0);
    assert(r.right() <= viewport.right() && r.bottom() <= viewport.bottom());
  }
  void fill(Rect r, fui::Paint, uint8_t, uint8_t) override { inside(r); }
  void stroke(Rect r, fui::Paint, uint8_t, uint8_t, uint8_t) override { inside(r); }
  void line(fui::Point, fui::Point, uint8_t, fui::Paint) override {}
  void triangle(fui::Point, fui::Point, fui::Point, fui::Paint) override {}
  void bitmap(Rect r, fui::BitmapRef, fui::BitmapMode, fui::Paint, fui::Rotation) override { inside(r); }
  void text(Rect r, const char* label, fui::TextStyle style) override {
    inside(r);
    fui::layoutText(*this, r, label, style, [&](const char*, Rect line) {
      assert(line.bottom() <= r.bottom());
      inside(line);
      inkLines.push_back(line);
    });
  }
};

struct Picker {
  bool active = true;
  std::atomic<int> selectedIndex{0};
  mutable std::atomic<int> firstOption{-1};
  std::vector<std::string> ownedStrings;
  std::function<void(int)> onSelectCallback;
''' + method(popup, '  int windowStart(') + method(popup, '  bool handleButtons(') + r'''
};

void drawPicker(Target& target, const fui::DeviceContext& device, Picker& picker, bool verbose,
                int padding, int gap, int buttonPadding) {
  std::array<fui::DialogOption, 16> options;
  fui::OptionDialogProps props;
  props.title = verbose ? "确实要应用这项会影响整台设备显示外观的设置吗" : "界面主题";
  props.titleText.font = fui::FONT_SLOT_BODY;
  props.titleText.maxLines = 2;
  props.headline = verbose ? "这里是可能有数行的书籍副标题与作者名，这一段故意超过屏幕宽度以测试换行" : nullptr;
  props.headlineText.font = fui::FONT_SLOT_BODY;
  props.headlineText.maxLines = 3;
  props.message = verbose ? "请再次确认当前选中的项目。显示设置将应用到整台设备。需要更改时可以返回显示设置重新选择，也可以在切换界面后返回当前菜单。此说明用于检查横屏的按钮不会覆盖长文字。" : nullptr;
  props.messageText.font = fui::FONT_SLOT_BODY;
  props.messageText.maxLines = 6;
  props.buttonText.font = fui::FONT_SLOT_BODY;
  props.buttonHeight = static_cast<int16_t>(target.lineHeight(props.buttonText.font) + buttonPadding * 2);
  props.gap = static_cast<int16_t>(gap);
  const auto inset = static_cast<int16_t>(padding);
  props.padding = {inset, inset, inset, inset};
  props.verticalOptions = true;
  props.options = options.data();
  props.inputMask = fui::InputTouch;

  const Rect safe = device.safeRect();
  const int16_t width = safe.width - 2 * XuesuOptionLayout::SIDE_MARGIN;
  const int maxHeight = safe.height - 2 * XuesuOptionLayout::VERTICAL_MARGIN;
  const int count = XuesuOptionLayout::fit(target, props, picker.ownedStrings, width, maxHeight, options.size());
  if (!verbose && device.height > device.width) assert(count == 7);
  if (verbose) assert(props.buttonText.maxLines == 2);
  const int selected = picker.selectedIndex.load();
  const int first = picker.windowStart(count, selected);
  assert(first <= selected && selected < first + count);
  for (int i = 0; i < count; ++i) {
    options[i].label = picker.ownedStrings[first+i].c_str();
    options[i].value = first+i;
    options[i].action = 1;
    options[i].state = first+i == selected ? fui::StateFocused : fui::StateNormal;
  }
  props.optionCount = count;
  const int16_t height = fui::optionDialogHeight(target, props, width);
  assert(height <= maxHeight);
  const Rect panel = fui::centeredRect(safe, {width, height});
  target.viewport = panel;
  target.inkLines.clear();
  fui::InteractionBuffer<16> hits;
  const fui::InputSnapshot input{};
  fui::Frame<16> frame(target, device, input, hits);
  fui::optionDialog(frame, panel, props);
  assert(hits.count() == static_cast<size_t>(count));
  int focused = 0;
  for (int i = 0; i < count; ++i) {
    const auto& hit = hits.data()[i];
    target.inside(hit.rect);
    if (i) assert(hit.rect.y - hits.data()[i-1].rect.bottom() >= 6);
    if (fui::hasState(hit.state, fui::StateFocused)) {
      assert(hit.value == selected);
      ++focused;
    }
  }
  assert(focused == 1);
  const int buttonsTop = hits.data()[0].rect.y;
  for (Rect line : target.inkLines) {
    assert(line.y >= buttonsTop || line.bottom() + 6 <= buttonsTop);
  }
  if (count < static_cast<int>(picker.ownedStrings.size())) {
    const Rect rows{panel.x, hits.data()[0].rect.y, panel.width,
                    static_cast<int16_t>(hits.data()[count-1].rect.bottom() - hits.data()[0].rect.y)};
    fui::drawListScrollIndicator(target, rows, picker.ownedStrings.size(), count, first,
                                 XuesuOptionLayout::SCROLL_WIDTH, 0, XuesuOptionLayout::SCROLL_INSET);
    assert(panel.right() - XuesuOptionLayout::SCROLL_INSET - XuesuOptionLayout::SCROLL_WIDTH -
           hits.data()[0].rect.right() >= 6);
  }
}

int main() {
  for (bool portrait : {false,true}) {
    fui::DeviceContext device;
    device.width = portrait ? 300 : 400;
    device.height = portrait ? 400 : 300;
    device.minTouchSize = 0;
    device.safeArea = {3,5,5,4};
    Target target;
    for (auto [padding, gap, buttonPadding] : {std::array<int,3>{16,6,4}, {20,8,12}, {24,6,10}}) {
      for (bool verbose : {false,true}) {
        Picker picker;
        picker.ownedStrings = {"Classic","Lyra","Lyra Extended","RoundedRaff","Lyra Carousel","INX","Cover Grid"};
        if (verbose) picker.ownedStrings[5] = "界面选项中文很长时会在这里换行显示第二行";
        int applied = -1;
        picker.onSelectCallback = [&](int index) { applied = index; };
        for (int i = 0; i < 7; ++i) {
          drawPicker(target, device, picker, verbose, padding, gap, buttonPadding);
          picker.handleButtons(false, true, false, false, []{});
        }
        assert(picker.selectedIndex == 0);
        picker.handleButtons(true, false, false, false, []{});
        drawPicker(target, device, picker, verbose, padding, gap, buttonPadding);
        picker.handleButtons(false, false, false, true, []{});
        assert(!picker.active && applied == -1);
        picker.active = true;
        picker.handleButtons(false, false, true, false, []{});
        assert(!picker.active && applied == 6);
      }
    }

  }
}
'''
        program = program.replace('@SMALL_LINE@', str(line_height('ubuntu_10_regular'))).replace(
            '@BODY_LINE@', str(line_height('ubuntu_12_regular')))
        run_cpp(program, include_dirs=(ROOT / 'src', SDK / 'include'),
                extra_sources=(SDK / 'src/FreeInkUI.cpp',))

    def test_mixed_height_rows_draw_real_chinese_and_keep_selection_visible(self):
        settings = (ROOT / 'src/activities/settings/XuesuSettingsLayout.h').read_text(encoding='utf-8')
        adapter = (SDK / 'include/FreeInkUIGfxRenderer.h').read_text(encoding='utf-8')
        chinese = (ROOT / 'lib/I18n/translations/chinese.yaml').read_text(encoding='utf-8')
        names = ('STR_SLEEP_COVER_MODE', 'STR_COVER_CUSTOM', 'STR_ABOUT_FIRMWARE_VERSION',
                 'STR_SLEEP_SCREEN', 'STR_AUTO_TURN_PAGES_PER_MIN', 'STR_RESTORE_SYSTEM_SETTINGS_WARNING')
        labels = [re.search(rf'^{key}: "([^"]+)"', chinese, re.M)[1] for key in names]
        labels[-1] = ''.join(character for character in labels[-1] if ord(character) > 127)
        version = 'v2.3.1-xuesu300-20261111-theme-adaptation-verify'
        characters = set(''.join(labels) + version + '0123456789 …')
        tables = []
        for size in (10, 12):
            ubuntu = f'ubuntu_{size}_regular'
            metrics = font_advances(f'notosans_cjk_{size}', 'notosans_cjk_common_intervals', characters)
            metrics.update(font_advances(ubuntu, ubuntu, characters))
            self.assertTrue(characters.issubset({chr(code) for code in metrics}))
            tables.append('{' + ','.join(f'{{{code},{width}}}' for code, width in sorted(metrics.items())) + '}')

        helpers = '\n'.join(method(settings, f'inline {signature}') for signature in
                            ('void fitRows(', 'int readableRowWidth(', 'void reflowValue(', 'void allowReadableRows('))
        program = r'''
#include <FreeInkUI.h>
#include <array>
#include <cassert>
#include <map>
#include <string>
#include <vector>

namespace fui = freeink::ui;
using namespace freeink::ui;

// Font advances are read from the same built-in Ubuntu + Noto CJK assets
// used by the firmware. The GfxRenderer seam records what actually draws.
struct EpdFontFamily {
  enum Style { Regular = 0, SUP = 1, SUB = 2 };
  struct Glyph { int width, height, top, left; };
  const Glyph* getGlyph(uint32_t, Style) const { return nullptr; }
};

struct Target : DrawTarget {
  struct Ink { Rect rect; std::string text; };
  const Target& renderer = *this;
  TextCentering textCentering_ = TextCentering::Advance;
  Rect viewport{};
  mutable std::vector<Ink> ink;
  const std::array<std::map<uint32_t, int>, 2> advances = {
    std::map<uint32_t, int>@SMALL_ADVANCES@, std::map<uint32_t, int>@BODY_ADVANCES@
  };

  static FontId gfxFont(FontId font) { return font; }
  static EpdFontFamily::Style fontStyle(TextStyle) { return EpdFontFamily::Regular; }
  static Color gfxColor(Color color) { return color; }
  static bool tryDrawTextDither(const Target&, int, int, int, const char*, Color, EpdFontFamily::Style, int) {
    return false;
  }
  static bool tryDrawTextRotatedDither(const Target&, int, int, int, const char*, Color, EpdFontFamily::Style, int) {
    return false;
  }
  const std::map<int, EpdFontFamily>& getFontMap() const {
    static const std::map<int, EpdFontFamily> fonts;
    return fonts;
  }
  int getFontAscenderSize(int font) const { return getLineHeight(font); }
  int getLineHeight(int font) const { return font == FONT_SLOT_SMALL ? @SMALL_LINE@ : @BODY_LINE@; }
  int16_t lineHeight(FontId font) const override { return getLineHeight(font); }
  int getTextWidth(int font, const char* text, EpdFontFamily::Style) const {
    const auto& widths = advances[font == FONT_SLOT_SMALL ? 0 : 1];
    int width = 0;
    for (auto p = reinterpret_cast<const unsigned char*>(text); *p;) {
      const auto lead = *p++;
      if (lead == '\n') continue;
      const int trailing = lead < 0x80 ? 0 : lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
      uint32_t cp = lead & (0x7f >> trailing);
      for (int i = 0; i < trailing; ++i) cp = (cp << 6) | (*p++ & 0x3f);
      width += widths.at(cp);
    }
    return width;
  }
  Size measureText(FontId font, const char* text, TextStyle style) const override {
    return {static_cast<int16_t>(getTextWidth(font, text, fontStyle(style))), lineHeight(font)};
  }
  std::string truncatedText(int, const char*, int, EpdFontFamily::Style) const {
    assert(false && "expected complete text");
    return {};
  }
  std::vector<std::string> wrappedText(int, const char*, int, uint8_t, EpdFontFamily::Style) const {
    assert(false && "explicit multiline text must use the same wrapping as list measurement");
    return {};
  }
  void inside(Rect r) const {
    assert(r.x >= viewport.x && r.y >= viewport.y && r.right() <= viewport.right() && r.bottom() <= viewport.bottom());
  }
  void drawText(int font, int x, int y, const char* text, bool, EpdFontFamily::Style style) const {
    const Rect r{static_cast<int16_t>(x), static_cast<int16_t>(y),
                 static_cast<int16_t>(getTextWidth(font, text, style)), lineHeight(font)};
    inside(r);
    ink.push_back({r, text});
  }
  void drawTextRotated90CW(int, int, int, const char*, bool, EpdFontFamily::Style) const {}
  void fill(Rect r, Paint, uint8_t, uint8_t) override { inside(r); }
  void stroke(Rect r, Paint, uint8_t, uint8_t, uint8_t) override { inside(r); }
  void line(Point, Point, uint8_t, Paint) override {}
  void triangle(Point, Point, Point, Paint) override {}
  void bitmap(Rect r, BitmapRef, BitmapMode, Paint, Rotation) override { inside(r); }
''' + method(adapter, '  void text(const Rect rect,') + r'''
};

constexpr int16_t ROW_GAP = 6;
''' + helpers + r'''

int main() {
  const char* screenMode = "@COVER_MODE@";
  const char* coverCustom = "@COVER_CUSTOM@";
  const char* aboutVersion = "@ABOUT_VERSION@";
  const char* version = "@VERSION@";
  const char* sleepScreen = "@SLEEP_SCREEN@";
  const char* autoTurn = "@AUTO_TURN@";
  const char* warning = "@WARNING@";
  Target target;
  for (bool portrait : {false, true}) {
    DeviceContext device;
    device.width = portrait ? 300 : 400;
    device.height = portrait ? 400 : 300;
    device.minTouchSize = 0;
    const Rect viewport{4, 44, static_cast<int16_t>(device.width - 8),
                        static_cast<int16_t>(device.height - 52)};
    target.viewport = viewport;
    TextStyle multiline;
    multiline.font = FONT_SLOT_BODY;
    multiline.maxLines = 3;
    const Rect wrapped{viewport.x, viewport.y, viewport.width, static_cast<int16_t>(3 * target.lineHeight(multiline.font))};
    target.ink.clear();
    target.text(wrapped, warning, multiline);
    std::string complete;
    for (const auto& line : target.ink) complete += line.text;
    assert(target.ink.size() >= 2 && complete == warning);
    target.ink.clear();
    const std::string hardBreak = std::string(sleepScreen) + '\n' + coverCustom;
    target.text(wrapped, hardBreak.c_str(), multiline);
    assert(target.ink.size() == 2 && target.ink[0].text == sleepScreen && target.ink[1].text == coverCustom);

    for (int inset : {0, 12, 20}) {
      const auto theme = themeTokensForLineHeight(target.lineHeight(FONT_SLOT_BODY));
      ListProps props;
      fitRows(target, theme, props);
      props.rowInset = inset;
      props.sidePadding = 20;
      props.valueInset = 8;
      props.textGap = 6;
      props.scrollIndicator = true;
      props.scrollIndicatorWidth = 3;
      props.scrollIndicatorInset = 6;
      props.partialTrailingRow = true;
      props.action = 1;
      props.inputMask = InputTouch;
      allowReadableRows(target, viewport, props);
      const int rowWidth = readableRowWidth(viewport, props);
      assert(props.partialTrailingMinHeight > viewport.height && props.rowGap >= 6);
      std::array<ListItem, 9> rows = {{{.label=sleepScreen, .value="5"},
          {.label=screenMode, .value=coverCustom}, {.label=aboutVersion, .value=version},
          {.label=sleepScreen, .toggle=true}, {.label=autoTurn, .value="10"},
          {.label=warning}, {.label=screenMode, .value=coverCustom},
          {.label=aboutVersion, .value=version}, {.label=sleepScreen, .value="5"}}};
      for (size_t i = 0; i < rows.size(); ++i) {
        rows[i].actionValue = i;
        reflowValue(target, props, rowWidth, rows[i]);
        const auto layout = measureListRow(target, nullptr, rowWidth, props, rows[i]);
        assert(layout.height <= viewport.height && layout.labelWidth > 0);
      }
      assert(rows[0].value && !rows[0].subtitle && rows[3].toggle && !rows[3].subtitle);
      assert(!rows[2].value && rows[2].subtitle == version);
      const auto versionLayout = measureListRow(target, nullptr, rowWidth, props, rows[2]);
      assert(versionLayout.subtitleHeight >= 2 * target.lineHeight(props.subtitleText.font));
      props.items = rows.data();
      props.count = rows.size();
      ListNav nav;
      const InputSnapshot input{};
      for (int selected = 0; selected < props.count; ++selected) {
        nav.selected.store(selected);
        nav.follow(props.count);
        InteractionBuffer<16> hits;
        for (int pass = 0; pass < props.count; ++pass) {
          target.ink.clear();
          hits.clear();
          nav.syncToProps(viewport, props.rowHeight, props.rowGap, props.count, props);
          Frame<16> frame(target, device, input, hits);
          list(frame, viewport, props);
          if (!nav.consumeRebuildNeeded()) break;
        }
        bool focused = false;
        for (size_t i = 0; i < hits.count(); ++i) {
          const auto& hit = hits.data()[i];
          assert(hit.rect.y >= viewport.y && hit.rect.bottom() <= viewport.bottom());
          if (i) assert(hit.rect.y - hits.data()[i-1].rect.bottom() >= ROW_GAP);
          if (hasState(hit.state, StateSelected)) {
            assert(hit.value == selected);
            focused = true;
            if (selected == 2 || selected == 7) {
              std::string painted;
              for (const auto& line : target.ink) {
                if (line.rect.y >= hit.rect.y && line.rect.bottom() <= hit.rect.bottom()) painted += line.text;
              }
              assert(painted == std::string(aboutVersion) + version);
            }
          }
        }
        assert(focused);
      }
    }
  }
}
'''
        replacements = dict(zip(('COVER_MODE', 'COVER_CUSTOM', 'ABOUT_VERSION', 'SLEEP_SCREEN',
                                 'AUTO_TURN', 'WARNING'), labels))
        replacements.update(VERSION=version, SMALL_ADVANCES=tables[0], BODY_ADVANCES=tables[1],
                            SMALL_LINE=str(line_height('ubuntu_10_regular')),
                            BODY_LINE=str(line_height('ubuntu_12_regular')))
        for key, value in replacements.items():
            program = program.replace(f'@{key}@', value)
        run_cpp(program, include_dirs=(ROOT / 'src', SDK / 'include'),
                defines=('FREEINK_DEVICE_XUESU_300=1',), extra_sources=(SDK / 'src/FreeInkUI.cpp',))
