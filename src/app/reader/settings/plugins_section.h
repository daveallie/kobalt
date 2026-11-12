#pragma once
#include "plugins/plugin_types.h"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

class QLabel;
class QVBoxLayout;
class QWidget;

namespace kobalt::ui {
class CaptionButton;
class ChoiceModal;
class DescribedOption;
class ModalOverlay;
} // namespace kobalt::ui

namespace kobalt::reader {
// Settings > System > Plugins: the plugins switch, one row per plugin folder or
// saved data, and a details dialog. Choices are a draft that applies when
// Settings closes and takes effect when the next book opens.
class PluginsSection final : public QObject
{
public:
    // Adds its rows to `rows` on `page`; dialogs open over `overlayParent`.
    // `styleRow` gives a plugin row the look of the other Settings list actions.
    PluginsSection(QWidget *page, QVBoxLayout *rows, QWidget *overlayParent,
                   const QString &folderLabel, std::function<void(ui::CaptionButton *)> styleRow);
    ~PluginsSection() override;

    // The switches that the open book loaded its plugins with.
    void setApplied(const plugins::Switches &switches);
    // The draft, read from and written to preferences around Settings.
    void setDraft(const plugins::Switches &switches);
    plugins::Switches draft() const;
    void setStatuses(const QVector<plugins::PluginStatus> &statuses);
    // Plugins are off because a plugin thread from an earlier book still runs.
    void setBlocked(bool blocked);
    // Removes a plugin's saved data and reports the result on the main thread.
    std::function<void(const QString &id, std::function<void(bool ok, QString error)> done)>
            deleteRequested;
    void dismissDialogs();

    ui::DescribedOption *usePluginsOption() const { return useOption; }

private:
    void rebuild();
    void refreshStatuses();
    QString statusText(const plugins::PluginStatus &status) const;
    bool drafted(const QString &id) const;
    void openDetails(const plugins::PluginStatus &status);
    void confirmDelete(const plugins::PluginStatus &status);

    QWidget *page, *overlayParent;
    std::function<void(ui::CaptionButton *)> styleRow;
    QVBoxLayout *rows;
    ui::DescribedOption *useOption;
    QLabel *hint, *blockedLabel;
    QVector<ui::CaptionButton *> pluginRows;
    QVector<plugins::PluginStatus> statuses;
    plugins::Switches applied, pending;
    bool blocked = false;
    QPointer<ui::ModalOverlay> details;
    QPointer<ui::ChoiceModal> confirmation;
};
} // namespace kobalt::reader
