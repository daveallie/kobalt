#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonValue>
#include <QString>

namespace kobalt::plugins {
// One plugin's saved data: a global store and one store for the open book.
// Values are compact JSON. Not thread-safe: use it only on the plugin thread.
class PluginStore
{
public:
    static constexpr int format = 1;
    static constexpr qint64 globalCap = 256 * 1024, bookCap = 64 * 1024;
    enum class Scope { Global, Book };

    // An empty root means that nothing can be stored.
    PluginStore(QString root, QString pluginId, QString bookKey);

    // Reads both files; an unusable file moves to backups/ and its store starts empty.
    void load();
    // Compact JSON, or empty when the key is missing.
    QByteArray value(Scope scope, const QString &key) const;
    // Empty `json` removes the key. False, with `error`, leaves the store unchanged.
    bool set(Scope scope, const QString &key, const QByteArray &json, QString *error);
    // Writes the scopes that changed since the last load or save.
    bool save(QString *error);
    // Empties both stores and removes the plugin's data folder.
    bool clear(QString *error);
    // UTF-8 key bytes plus encoded value bytes.
    qint64 size(Scope scope) const;
    static QString bookFile(const QString &bookKey);
    static QByteArray encode(const QJsonValue &value);
    static QJsonValue decode(const QByteArray &json);

private:
    struct Values
    {
        QHash<QString, QByteArray> entries;
        qint64 size = 0;
        bool changed = false;
        // A bad file stayed in place because it could not be moved to backups/.
        bool blocked = false;
    };

    Values &values(Scope scope);
    const Values &values(Scope scope) const;
    QString folder() const;
    QString path(Scope scope) const;
    void loadFile(Scope scope);
    // Moves an unusable file to backups/; false when it stays in place.
    bool backUp(const QString &file, const QString &reason);

    QString root, pluginId, bookKey;
    Values global, book;
};
} // namespace kobalt::plugins
