#include <gtest/gtest.h>

#include "activities/apps/weread/WeReadTouchGeometry.h"

TEST(WeReadTouchGeometry, KeepsDetailSectionsSeparateOnShortScreens) {
  ThemeMetrics metrics{};
  metrics.contentSidePadding = 18;
  metrics.verticalSpacing = 10;
  for (const Rect content : {Rect{0, 76, 300, 324}, Rect{6, 76, 388, 214}, Rect{0, 76, 480, 684}}) {
    const auto layout = weReadDetailLayout(content, metrics, 66, 4, 22);
    EXPECT_GT(layout.cover.height, 0);
    EXPECT_GE(layout.metadata.x, layout.cover.x + layout.cover.width);
    EXPECT_GE(layout.introduction.y, layout.cover.y + layout.cover.height + 6);
    EXPECT_GE(layout.actions.y, layout.introduction.y + layout.introduction.height + 6);
    EXPECT_GE(layout.actionStep - layout.actionGap, 22);
    EXPECT_EQ(layout.actions.height, layout.actionStep * 4);
    EXPECT_EQ(layout.actions.y + layout.actions.height, content.y + content.height);
    EXPECT_EQ(layout.actionGap, content.height < 472 ? 6 : 0);
  }
}

TEST(WeReadTouchGeometry, MapsPortraitGridAndRejectsGaps) {
  const Rect content{0, 0, 480, 700};
  const WeReadShelfGridLayout layout{
      .columns = 3,
      .rows = 3,
      .itemsPerPage = 9,
      .coverWidth = 100,
      .coverHeight = 140,
      .itemHeight = 160,
      .columnGap = 10,
      .rowGap = 20,
      .availableX = 20,
      .availableWidth = 440,
  };

  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 0, 8, 80, 90), 0);
  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 0, 8, 135, 450), 6);
  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 0, 8, 245, 450), 7);
  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 0, 8, 185, 100), -1);
  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 0, 8, 10, 10), -1);
}

TEST(WeReadTouchGeometry, MapsLandscapePageOffsetAndPartialRow) {
  const Rect content{0, 0, 800, 420};
  const WeReadShelfGridLayout layout{
      .columns = 5,
      .rows = 2,
      .itemsPerPage = 10,
      .coverWidth = 100,
      .coverHeight = 140,
      .itemHeight = 160,
      .columnGap = 20,
      .rowGap = 20,
      .availableX = 20,
      .availableWidth = 760,
  };

  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 10, 12, 290, 130), 10);
  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 10, 12, 410, 130), 11);
  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 10, 12, 530, 130), -1);
  EXPECT_EQ(weReadShelfIndexFromPoint(content, layout, 0, 0, 290, 130), -1);
}
