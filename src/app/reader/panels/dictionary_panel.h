#pragma once
#include "ui/controls/buttons.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/controls/paged_text.h"
#include "ui/style/metrics.h"
#include "platform/api/dictionary_service.h"
#include "platform/api/keyboard_controller.h"
#include "plugins/plugin_types.h"
#include <QFont>
#include <QLabel>
#include "ui/dialogs/modals.h"
#include <QPointer>

namespace kobalt {
// A sheet across the full width, fixed to the screen edge away from the word.
class DictionaryPanel : public ui::Panel
{
public:
    explicit DictionaryPanel(QWidget *parent, KeyboardFactory makeKeyboard = {});
    std::function<void()> dismissed;
    std::function<void(QString)> chosen;
    std::function<void(QString)> requested, searchRequested;
    std::function<void(const QFont &)> prepareEditorFont;
    // The My Words cell: a check when the word is saved, a plus otherwise.
    // Hidden while `available` is false, such as without a definition.
    void setSaved(bool saved, bool available = true);
    QWidget *saveActionButton() const;
    std::function<void(bool)> saveToggled;
    void setChoices(QVector<DictionaryChoice> values);
    void setLanguage(const QString &language);
    // The resolved book face supplies the header and default definition text.
    void setBookFont(const QString &family);
    static QFont definitionFont(const QString &family, int percent);
    static int definitionLineHeight(int percent, bool customFont = false);
    void setDefinitionFont(const QString &family, int percent);
    void open(const QString &word, const DictionaryResult &result, const QRect &selection);
    // Adds a plugin's answer to the chooser and marks the chooser button; a card from the
    // same plugin replaces its earlier one.
    void addPluginCard(const plugins::Card &card);
    void clearPluginCards();
    void place(const QRect &selection);
    void dismiss();
    static QString formatDefinition(const QString &html);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;
    void hideEvent(QHideEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    ui::PagerBar *pages, *choicesPaging;
    std::unique_ptr<KeyboardController> keyboard;
    // On a narrow sheet, `actions` opens a menu with edit, search and save.
    ui::CellButton *dictionary, *close, *edit, *submit, *search, *save, *actions;
    // Marks the chooser button while a plugin card is available.
    QWidget *pluginMarker;
    QPointer<ui::ModalOverlay> actionMenu;
    void openActions();
    bool saved = false, saveAvailable = false;
    QLineEdit *query;
    QRect selection;
    bool editing = false;
    bool showingCard = false;
    QLabel *headword, *editHint;
    QString word, bookFamily, language, definitionFamily;
    QVector<plugins::Card> cards;

    // The definition shown before a card was chosen, restored when its dictionary is chosen.
    struct Definition
    {
        QString word, html, source, dictionaryId;
    } beforeCard;

    // The source of the definition the sheet opened with, so that the chooser can always
    // lead back to it, even when no installed dictionary lists it.
    DictionaryChoice definitionChoice;
    void showCard(const plugins::Card &card);
    void restoreDefinition();
    void placeMarker();
    QString definitionHtml, sourceText, renderedHtml;
    int definitionPercent = 100;
    QFont headwordFont;
    QFont bookFont(int pixels) const;
    ui::Panel *dropdown;
    QVector<DictionaryChoice> options;
    QVector<QPushButton *> optionButtons;
    QString currentId;
    int optionsPage = 0;
    bool lower = true;
    // Open at the page that holds the selected dictionary.
    void redrawChoices(bool atCurrent = false);
    ui::PagedText *text;
    int rowTop() const;
    int contentBottom() const;
    void arrange();
    void setHeadword(const QString &text);
    void updateDefinition();
    void refresh();
    void editQuery();
    void finishEditing(bool restore = true);
    void submitQuery();
};
} // namespace kobalt
