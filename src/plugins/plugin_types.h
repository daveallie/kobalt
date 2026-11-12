#pragma once
#include <QString>
#include <QtGlobal>

namespace kobalt::plugins {
struct Book
{
    QString identity, title, language;
    // BookmarkStore::bookKey, which keeps a sideloaded book's data when its file moves.
    QString key;
};

struct Position
{
    double percent = 0;
    int page = 0, pages = 0;
    QString chapter;
};

struct Card
{
    QString title, html, footer;

    bool isEmpty() const { return title.isEmpty(); }
};

struct Lookup
{
    quint64 epoch = 0;
    QString word;
    Position position;
};
} // namespace kobalt::plugins
