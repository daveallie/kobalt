#include "plugins_section.h"

#include "support/strings.h"
#include "ui/controls/buttons.h"
#include "ui/controls/text.h"
#include "ui/controls/options.h"
#include "ui/dialogs/modals.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"

#include <QLabel>
#include <QVBoxLayout>

namespace kobalt::reader {
namespace metrics = ui::metrics;
using plugins::PluginStatus;

namespace {
QLabel *note(QWidget *parent, const char *name)
{
    auto *label = new ui::InterfaceLabel(parent);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    ui::readerStyle(label,
                    "QLabel{border-bottom:1px solid palette(light);"
                            + ui::styles::font(metrics::chrome::descriptionText) + "}");
    label->setMargin(metrics::chrome::inset);
    return label;
}
} // namespace

PluginsSection::PluginsSection(QWidget *page, QVBoxLayout *rows, QWidget *overlayParent,
                               const QString &folderLabel,
                               std::function<void(ui::CaptionButton *)> styleRow)
    : QObject(page),
      page(page),
      overlayParent(overlayParent),
      styleRow(std::move(styleRow)),
      rows(rows)
{
    useOption = new ui::DescribedOption(strings::get("settings.use_plugins"),
                                        strings::get("settings.use_plugins_description"), page);
    useOption->setObjectName("rrUsePlugins");
    useOption->setChecked(true);
    rows->addWidget(useOption);
    connect(useOption, &QCheckBox::toggled, this, [this](bool checked) {
        pending.enabled = checked;
        refreshStatuses();
    });
    blockedLabel = note(page, "rrPluginsBlocked");
    blockedLabel->setText(strings::get("settings.plugins_blocked"));
    blockedLabel->hide();
    rows->addWidget(blockedLabel);
    hint = note(page, "rrPluginsInstallHint");
    hint->setText(strings::get("settings.plugins_install").arg(folderLabel));
    rows->addWidget(hint);
}

PluginsSection::~PluginsSection()
{
    delete details;
    delete confirmation;
}

void PluginsSection::setApplied(const plugins::Switches &switches)
{
    applied = switches;
    refreshStatuses();
}

void PluginsSection::setDraft(const plugins::Switches &switches)
{
    pending = switches;
    useOption->setChecked(switches.enabled);
    refreshStatuses();
}

plugins::Switches PluginsSection::draft() const
{
    return pending;
}

void PluginsSection::setStatuses(const QVector<PluginStatus> &value)
{
    statuses = value;
    rebuild();
}

void PluginsSection::setBlocked(bool value)
{
    blocked = value;
    rebuild();
}

void PluginsSection::rebuild()
{
    for (auto *row : pluginRows) {
        rows->removeWidget(row);
        row->hide();
        row->deleteLater();
    }
    pluginRows.clear();
    blockedLabel->setVisible(blocked);
    hint->setVisible(!blocked && statuses.isEmpty());
    if (!blocked) {
        int index = rows->indexOf(hint) + 1;
        for (const PluginStatus &status : statuses) {
            auto *row = ui::makeCaptionButton(page, nullptr, "rrPlugin", status.name,
                                              [this, id = status.id] {
                                                  for (const auto &item : statuses)
                                                      if (item.id == id)
                                                          openDetails(item);
                                              });
            row->setObjectName(QStringLiteral("rrPlugin_") + status.id);
            row->setChevron(true);
            if (styleRow)
                styleRow(row);
            rows->insertWidget(index++, row);
            pluginRows.append(row);
        }
    }
    refreshStatuses();
}

bool PluginsSection::drafted(const QString &id) const
{
    return pending.enabled && !pending.disabled.contains(id);
}

QString PluginsSection::statusText(const PluginStatus &status) const
{
    const bool wasOn = applied.enabled && !applied.disabled.contains(status.id);
    const bool loadable = status.state == PluginStatus::Running
            || status.state == PluginStatus::Stopped || status.state == PluginStatus::TurnedOff;
    if (loadable && wasOn != drafted(status.id))
        return strings::get("settings.plugin_status_next_book");
    switch (status.state) {
    case PluginStatus::Running:
        return strings::get("settings.plugin_status_running");
    case PluginStatus::TurnedOff:
        return strings::get("settings.plugin_status_off");
    case PluginStatus::Stopped:
        return strings::get("settings.plugin_status_stopped");
    case PluginStatus::DataOnly:
        return strings::get("settings.plugin_status_data_only");
    case PluginStatus::NotLoaded:
        break;
    }
    return strings::get("settings.plugin_status_not_loaded").arg(status.reason);
}

void PluginsSection::refreshStatuses()
{
    for (int i = 0; i < pluginRows.size() && i < statuses.size(); ++i) {
        const QString text = statusText(statuses[i]);
        pluginRows[i]->setDescription(text);
        pluginRows[i]->setAccessibleDescription(text);
    }
}

void PluginsSection::openDetails(const PluginStatus &status)
{
    delete details;
    details = new ui::ModalOverlay(overlayParent);
    details->setObjectName("rrPluginDetails");
    details->setDimmed(true);
    details->setGeometry(overlayParent->rect());
    details->dismissed = [this] { dismissDialogs(); };
    auto *card = details->card();
    QString about = status.description;
    const QString facts = status.state == PluginStatus::DataOnly
            ? strings::get("settings.plugin_data_only_details")
            : (status.version.isEmpty()
                       ? QString()
                       : strings::get("settings.plugin_version").arg(status.version)
                               + QLatin1Char('\n'))
                    + status.folder;
    about = about.isEmpty() ? facts : about + QStringLiteral("\n\n") + facts;
    card->heading(status.name, about, "rrPluginDetailsTitle", "rrPluginDetailsDescription");
    const int cardWidth = qMin<int>(metrics::notice::cardWidth,
                                    overlayParent->width() - 2 * metrics::notice::sideClearance);
    if (status.state != PluginStatus::DataOnly) {
        auto *enabled =
                new ui::DescribedOption(strings::get("settings.plugin_enabled"),
                                        strings::get("settings.plugin_enabled_description"), card);
        enabled->setObjectName("rrPluginEnabled");
        // Styled as the KOSync page styles an option inside a card.
        auto look = enabled->presentation();
        look.indicator = ui::DescribedOption::Indicator::Square;
        look.pressedFill = true;
        look.dividers = Qt::TopEdge;
        look.horizontalInset = metrics::chrome::inset;
        look.labelPixels = metrics::chrome::labelText;
        look.descriptionPixels = metrics::chrome::descriptionText;
        look.indicatorSize = { metrics::chrome::checkSize, metrics::chrome::checkSize };
        look.minimumRowHeight = metrics::chrome::optionHeight;
        enabled->setPresentation(look);
        enabled->setChecked(!pending.disabled.contains(status.id));
        connect(enabled, &QCheckBox::toggled, this, [this, id = status.id](bool checked) {
            if (checked)
                pending.disabled.remove(id);
            else
                pending.disabled.insert(id);
            refreshStatuses();
        });
        card->rows()->addWidget(enabled);
    }
    auto *remove = ui::makeModalButton(
            card, { strings::get("settings.plugin_delete_data"), "rrPluginDeleteData" }, false);
    auto *done = ui::makeModalButton(card, { strings::get("common.done"), "rrPluginDone" }, true);
    card->adaptiveActions({ remove, done });
    connect(remove, &QPushButton::clicked, this, [this, status] { confirmDelete(status); });
    connect(done, &QPushButton::clicked, this, [this] { dismissDialogs(); });
    card->fitToWidth(cardWidth);
    details->centerCard();
    details->show();
    details->raise();
    done->setFocus();
}

void PluginsSection::confirmDelete(const PluginStatus &status)
{
    delete confirmation;
    confirmation = new ui::ChoiceModal(
            overlayParent,
            { strings::get("settings.plugin_delete_data_warning").arg(status.name),
              strings::get("settings.plugin_delete_data_title"),
              {} },
            { strings::get("common.cancel"), "rrKeepPluginData" },
            { strings::get("settings.plugin_delete"), "rrConfirmDeletePluginData" });
    confirmation->setObjectName("rrPluginDataConfirmation");
    confirmation->setDimmed(true);
    confirmation->setGeometry(overlayParent->rect());
    // The confirmation replaces the details card until it is answered.
    if (details)
        details->hide();
    const auto close = [this] {
        if (confirmation) {
            confirmation->hide();
            confirmation->deleteLater();
        }
        if (details) {
            details->show();
            details->raise();
        }
    };
    confirmation->dismissed = close;
    connect(confirmation->secondaryButton(), &QPushButton::clicked, this, close);
    connect(confirmation->primaryButton(), &QPushButton::clicked, this, [this, close, status] {
        close();
        if (!deleteRequested)
            return;
        QPointer<PluginsSection> guard(this);
        deleteRequested(status.id, [guard](bool ok, const QString &error) {
            if (!guard || !guard->details)
                return;
            if (auto *description =
                        guard->details->findChild<QLabel *>("rrPluginDetailsDescription"))
                description->setText(
                        ok ? strings::get("settings.plugin_data_deleted")
                           : strings::get("settings.plugin_data_not_deleted").arg(error));
        });
    });
    confirmation->fitCard();
    confirmation->show();
    confirmation->raise();
    confirmation->secondaryButton()->setFocus();
}

void PluginsSection::dismissDialogs()
{
    if (confirmation)
        confirmation->deleteLater();
    if (details) {
        details->hide();
        details->deleteLater();
    }
}
} // namespace kobalt::reader
