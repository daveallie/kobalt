#pragma once
#include "plugin_types.h"

#include "reading/word_estimates.h"

namespace kobalt::plugins {
// Everything on the visible page counts as reached.
inline Position position(const ReadingActivity &activity)
{
    Position result;
    result.page = activity.page + 1;
    result.pages = activity.pages;
    const auto &selection = activity.chapter;
    const bool chapterSelected =
            selection.current >= 0 && selection.current < activity.chapters.size();
    if (chapterSelected)
        result.chapter = activity.chapters[selection.current].title;

    const auto [chapterPage, chapterPages] = wordEstimates::chapterPage(activity);
    double percent = -1;
    if (chapterSelected && activity.totalWords > 0 && chapterPage > 0
        && activity.chapters[selection.current].words >= 0) {
        qint64 chapterWords = 0, before = 0;
        bool valid = true;
        for (int i = 0; i < activity.chapters.size(); ++i) {
            const auto words = activity.chapters[i].words;
            valid = valid && words >= 0;
            chapterWords += words;
            if (i < selection.current)
                before += words;
        }
        if (valid && chapterWords <= activity.totalWords) {
            const qint64 frontMatter = activity.totalWords - chapterWords;
            const double read = double(frontMatter + before)
                    + double(activity.chapters[selection.current].words) * chapterPage
                            / chapterPages;
            percent = read / double(activity.totalWords) * 100;
        }
    }
    if (percent < 0)
        percent = activity.pages > 0 ? double(activity.page + 1) / activity.pages * 100 : 0;
    result.percent = qBound(0.0, percent, 100.0);
    return result;
}

// Nothing past `page`, from 1, counts as reached, by page progress.
inline Position capAtPage(Position position, int page)
{
    if (position.pages <= 0)
        return position;
    page = qBound(1, page, position.pages);
    position.page = qMin(position.page, page);
    position.percent = qMin(position.percent, double(page) / position.pages * 100);
    return position;
}
} // namespace kobalt::plugins
