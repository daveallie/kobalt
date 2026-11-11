#include "plugins/plugin_position.h"

#include "check.h"

#include <cmath>

namespace {
kobalt::ReadingActivity activity(int current, int chapterPage, int chapterPages)
{
    kobalt::ReadingActivity result;
    result.totalWords = 500;
    result.page = 10;
    result.pages = 50;
    result.chapters = { { "One", "p1", 0, 0, 100 },
                        { "Two", "p2", 10, 0, 300 },
                        { "Three", "p3", 40, 0, 100 } };
    result.chapter = { current, -1, chapterPage, chapterPages };
    return result;
}

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-9;
}

void wordPercentAtEndOfPage()
{
    const auto position = kobalt::plugins::position(activity(1, 2, 3));
    require(near(position.percent, (100 + 300 * 2.0 / 3) / 500 * 100),
            "Word percent did not count through the end of the page");
}

void lastPageIsHundred()
{
    auto last = activity(2, 1, 1);
    last.page = 49;
    require(near(kobalt::plugins::position(last).percent, 100), "Last page was not 100 percent");
}

void pageFallback()
{
    auto fallback = activity(1, 2, 3);
    fallback.totalWords = -1;
    fallback.page = 4;
    fallback.pages = 10;
    require(near(kobalt::plugins::position(fallback).percent, 50),
            "Page fallback did not count through the end of the page");

    auto unknownWords = activity(1, 2, 3);
    unknownWords.chapters[1].words = -1;
    unknownWords.page = 4;
    unknownWords.pages = 10;
    require(near(kobalt::plugins::position(unknownWords).percent, 50),
            "Negative chapter words did not use the page fallback");
}

void chapterTitleAndPage()
{
    const auto position = kobalt::plugins::position(activity(1, 2, 3));
    require(position.chapter == "Two" && position.page == 11 && position.pages == 50,
            "Chapter title or page was wrong");
}
} // namespace

int main()
{
    wordPercentAtEndOfPage();
    lastPageIsHundred();
    pageFallback();
    chapterTitleAndPage();
    return 0;
}
