#include <gtest/gtest.h>

#include <string>

#include "TxtChapterIndex.h"

using txt_chapter_index::HeadingKind;
using txt_chapter_index::matchHeading;

TEST(TxtChapterIndex, RecognizesChineseNovelHeadings) {
  EXPECT_EQ(matchHeading("第1章 开始").title, "第1章 开始");
  EXPECT_EQ(matchHeading("第1章开始").kind, HeadingKind::Standalone);
  EXPECT_EQ(matchHeading("  第 十二 回 归来\r").title, "第 十二 回 归来");
  EXPECT_EQ(matchHeading("第二百三十四节").title, "第二百三十四节");
  EXPECT_EQ(matchHeading("第两万零一篇 尾声").title, "第两万零一篇 尾声");
  for (const auto text : {"第一天发生了很多事", "这是第1章的正文说明", "第章 缺少编号", "第二节课开始了。",
                          "第二回合他赢了。", "第一部分是引言。", "第一个集合"}) {
    EXPECT_EQ(matchHeading(text).kind, HeadingKind::Body) << text;
  }
}

TEST(TxtChapterIndex, RecognizesNumberedEnglishHeadings) {
  EXPECT_EQ(matchHeading("Chapter 1: Arrival").title, "Chapter 1: Arrival");
  EXPECT_EQ(matchHeading("chapter xii The Return").title, "chapter xii The Return");
  EXPECT_EQ(matchHeading("BOOK IV—Winter").title, "BOOK IV—Winter");
  EXPECT_EQ(matchHeading("Part 3. Home").title, "Part 3. Home");
  EXPECT_EQ(matchHeading("Volume　１２．Winter").kind, HeadingKind::Standalone);
  EXPECT_EQ(matchHeading("Chapter One").kind, HeadingKind::Body);
  EXPECT_EQ(matchHeading("Chapterhouse 2").kind, HeadingKind::Body);
  EXPECT_EQ(matchHeading("A note about Chapter 2").kind, HeadingKind::Body);
}

TEST(TxtChapterIndex, RejectsEmptyAndOversizedLines) {
  EXPECT_EQ(matchHeading(" \t　\r").kind, HeadingKind::Empty);
  EXPECT_EQ(matchHeading("\xEF\xBB\xBF"
                         "　Chapter 1")
                .title,
            "Chapter 1");

  std::string oversized = "Chapter 1 ";
  oversized.append(txt_chapter_index::TITLE_CAPACITY, 'x');
  EXPECT_EQ(matchHeading(oversized).kind, HeadingKind::Body);
}

TEST(TxtChapterIndex, RecognizesFullwidthBracketsAndSpecialHeadings) {
  EXPECT_EQ(matchHeading("　【第　１２　章：出发】　").title, "【第　１２　章：出发】");
  EXPECT_EQ(matchHeading("〖第一卷：暗潮〗").kind, HeadingKind::Standalone);
  EXPECT_EQ(matchHeading("[第一章] 新世界").kind, HeadingKind::Standalone);
  EXPECT_EQ(matchHeading("（楔子）").kind, HeadingKind::Standalone);
  for (const auto text : {"序章", "楔子", "引子", "前言", "终章", "尾声", "后记", "附录", "番外", "番外篇：旧事",
                          "番外２：重逢", "番外一：重逢", "番外（上）"}) {
    EXPECT_EQ(matchHeading(text).title, text);
  }
  for (const auto text : {"尾声就要到了。", "番外故事还在继续。", "【第一章 这行没闭合", "【任意括号文字】"}) {
    EXPECT_EQ(matchHeading(text).kind, HeadingKind::Body) << text;
  }
}

TEST(TxtChapterIndex, ClassifiesBareNumbersWithoutAcceptingDatesOrTimes) {
  for (const auto text : {"001", "００１", "001 [重逢]", "001[重逢]", "1. Opening", "１、重逢"}) {
    const auto heading = matchHeading(text);
    EXPECT_EQ(heading.kind, HeadingKind::Numbered) << text;
    EXPECT_EQ(heading.number, 1U) << text;
  }
  for (const auto text : {"123456", "2026-11-17", "10:20", "45.761871", "１．２", "2nd place"}) {
    EXPECT_EQ(matchHeading(text).kind, HeadingKind::Body) << text;
  }
}
