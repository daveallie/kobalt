#include "plugin_store.h"

#include "support/diagnostics.h"
#include "support/main_thread.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>

namespace kobalt::plugins {
namespace {
constexpr int keyLimit = 256;

qint64 cap(PluginStore::Scope scope)
{
    return scope == PluginStore::Scope::Global ? PluginStore::globalCap : PluginStore::bookCap;
}

qint64 entrySize(const QString &key, const QByteArray &json)
{
    return key.toUtf8().size() + json.size();
}
} // namespace

// QJsonDocument holds only arrays and objects, so a value travels inside an array.
QByteArray PluginStore::encode(const QJsonValue &value)
{
    QJsonArray holder;
    holder.append(value);
    const QByteArray array = QJsonDocument(holder).toJson(QJsonDocument::Compact);
    return array.mid(1, array.size() - 2);
}

QJsonValue PluginStore::decode(const QByteArray &json)
{
    return QJsonDocument::fromJson('[' + json + ']').array().at(0);
}

PluginStore::PluginStore(QString root, QString pluginId, QString bookKey)
    : root(std::move(root)), pluginId(std::move(pluginId)), bookKey(std::move(bookKey))
{
}

QString PluginStore::bookFile(const QString &bookKey)
{
    return QStringLiteral("books/")
            + QString::fromLatin1(
                    QCryptographicHash::hash(bookKey.toUtf8(), QCryptographicHash::Sha256).toHex())
            + QStringLiteral(".json");
}

PluginStore::Values &PluginStore::values(Scope scope)
{
    return scope == Scope::Global ? global : book;
}

const PluginStore::Values &PluginStore::values(Scope scope) const
{
    return scope == Scope::Global ? global : book;
}

QString PluginStore::folder() const
{
    return root + QLatin1Char('/') + pluginId;
}

QString PluginStore::path(Scope scope) const
{
    return folder() + QLatin1Char('/')
            + (scope == Scope::Global ? QStringLiteral("global.json") : bookFile(bookKey));
}

void PluginStore::load()
{
    global = {};
    book = {};
    if (root.isEmpty())
        return;
    loadFile(Scope::Global);
    if (!bookKey.isEmpty())
        loadFile(Scope::Book);
}

void PluginStore::loadFile(Scope scope)
{
    const QString file = path(scope);
    QFile input(file);
    if (!input.exists())
        return;
    // Twice the cap leaves room for JSON escapes and whitespace a person may add.
    if (input.size() > 2 * cap(scope) + 1024) {
        backUp(file, QStringLiteral("larger than its limit"));
        return;
    }
    if (!input.open(QIODevice::ReadOnly)) {
        backUp(file, QStringLiteral("unreadable"));
        return;
    }
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(input.readAll(), &parse);
    input.close();
    const QJsonObject object = document.object();
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        backUp(file, QStringLiteral("not a JSON object"));
        return;
    }
    if (object.value(QStringLiteral("format")).toDouble() != format) {
        backUp(file, QStringLiteral("unsupported format"));
        return;
    }
    const QJsonValue entries = object.value(QStringLiteral("values"));
    if (!entries.isObject()) {
        backUp(file, QStringLiteral("no values object"));
        return;
    }
    Values loaded;
    const QJsonObject map = entries.toObject();
    for (auto it = map.begin(); it != map.end(); ++it) {
        const QByteArray json = encode(it.value());
        loaded.entries.insert(it.key(), json);
        loaded.size += entrySize(it.key(), json);
    }
    if (loaded.size > cap(scope)) {
        backUp(file, QStringLiteral("larger than its limit"));
        return;
    }
    values(scope) = loaded;
}

void PluginStore::backUp(const QString &file, const QString &reason)
{
    mainthread::BlockingOperation operation("plugin-data-backup");
    const QString backups = folder() + QStringLiteral("/backups");
    QDir().mkpath(backups);
    const QString stem = QFileInfo(file).completeBaseName() + QLatin1Char('-')
            + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmssZ"));
    QString target = backups + QLatin1Char('/') + stem + QStringLiteral(".json");
    for (int suffix = 2; QFile::exists(target); ++suffix)
        target = backups + QLatin1Char('/') + stem + QLatin1Char('-') + QString::number(suffix)
                + QStringLiteral(".json");
    const bool moved = QFile::rename(file, target);
    diagnostic("plugin-data-backup",
               { { "plugin", pluginId },
                 { "reason", reason },
                 { "file", file },
                 { "backup", moved ? target : QString() } },
               DiagnosticSeverity::Warning);
}

QByteArray PluginStore::value(Scope scope, const QString &key) const
{
    return values(scope).entries.value(key);
}

qint64 PluginStore::size(Scope scope) const
{
    return values(scope).size;
}

bool PluginStore::set(Scope scope, const QString &key, const QByteArray &json, QString *error)
{
    if (root.isEmpty()) {
        *error = QStringLiteral("no plugin data folder");
        return false;
    }
    if (scope == Scope::Book && bookKey.isEmpty()) {
        *error = QStringLiteral("no book to store data for");
        return false;
    }
    const int keyBytes = key.toUtf8().size();
    if (keyBytes == 0 || keyBytes > keyLimit) {
        *error = QStringLiteral("keys must be 1 to 256 bytes");
        return false;
    }
    Values &target = values(scope);
    const auto existing = target.entries.constFind(key);
    const bool present = existing != target.entries.constEnd();
    if (present ? *existing == json : json.isEmpty())
        return true;
    const qint64 next = target.size - (present ? entrySize(key, *existing) : 0)
            + (json.isEmpty() ? 0 : entrySize(key, json));
    if (next > cap(scope)) {
        *error = QStringLiteral("the value would take the store past its limit of ")
                + QString::number(cap(scope) / 1024) + QStringLiteral(" KiB");
        return false;
    }
    if (json.isEmpty())
        target.entries.remove(key);
    else
        target.entries.insert(key, json);
    target.size = next;
    target.changed = true;
    return true;
}

bool PluginStore::save(QString *error)
{
    for (const Scope scope : { Scope::Global, Scope::Book }) {
        Values &source = values(scope);
        if (!source.changed)
            continue;
        QJsonObject map;
        for (auto it = source.entries.constBegin(); it != source.entries.constEnd(); ++it)
            map.insert(it.key(), decode(it.value()));
        QJsonObject document;
        document.insert(QStringLiteral("format"), format);
        document.insert(QStringLiteral("values"), map);
        const QByteArray bytes = QJsonDocument(document).toJson(QJsonDocument::Compact);
        const QString file = path(scope);
        mainthread::BlockingOperation operation("plugin-data-save");
        QDir().mkpath(QFileInfo(file).absolutePath());
        QSaveFile output(file);
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size()
            || !output.commit()) {
            *error = QStringLiteral("cannot write ") + file;
            return false;
        }
        source.changed = false;
    }
    return true;
}

bool PluginStore::clear(QString *error)
{
    global = {};
    book = {};
    if (root.isEmpty())
        return true;
    mainthread::BlockingOperation operation("plugin-data-delete");
    QDir data(folder());
    if (data.exists() && !data.removeRecursively()) {
        *error = QStringLiteral("cannot remove ") + folder();
        return false;
    }
    return true;
}
} // namespace kobalt::plugins
