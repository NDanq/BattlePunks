#ifndef NIMBLEEDITOR_CONSOLEPANEL_H
#define NIMBLEEDITOR_CONSOLEPANEL_H

#if WITH_EDITOR

#include <QStringList>
#include <QWidget>

#include "Public/Editor/EditorLog.h"

class EditorConsole;
class QCompleter;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QStringListModel;
class QToolButton;

/*
 * The Console dock: EditorLog's transcript above, a command line below.
 *
 *   Log / Warnings / Errors   toggle each verbosity; the count is the history's
 *   Filter                    only lines containing the text
 *   Time                      prefix each line with when it was logged
 *   Clear                     empty the history (the `clear` command does too)
 *
 * The command line runs through EditorConsole. Up/Down walk the lines entered
 * this session; Tab / the popup complete command and config var names. The
 * transcript follows new output only while it is scrolled to the bottom, so
 * scrolling up to read something holds still.
 *
 * Commands and their replies always show, whatever the filters -- they are
 * the conversation, not the log.
 */
class ConsolePanel : public QWidget
{
    Q_OBJECT

public:
    ConsolePanel(EditorConsole& InConsole, QWidget* InParent = nullptr);

    /* Puts the cursor on the command line. */
    void FocusInput();

protected:
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;

private:
    void OnEntriesAppended(int InCount);
    void Rebuild();
    void UpdateCounts();
    void Submit();
    void RecallHistory(int InStep);
    void RefreshCompletions();

    bool Accepts(const EditorLog::Entry& InEntry) const;
    void AppendEntry(const EditorLog::Entry& InEntry);

    EditorConsole& Console;

    QToolButton*    LogToggle     = nullptr;
    QToolButton*    WarningToggle = nullptr;
    QToolButton*    ErrorToggle   = nullptr;
    QToolButton*    TimeToggle    = nullptr;
    QLineEdit*      Filter        = nullptr;
    QPlainTextEdit* Output        = nullptr;
    QLineEdit*      Input         = nullptr;

    QCompleter*       Completer       = nullptr;
    QStringListModel* CompletionModel = nullptr;

    QStringList History;         // oldest first
    int         HistoryCursor = 0;   // == History.size() when not recalling
    QString     Draft;           // what was typed before Up began recalling
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_CONSOLEPANEL_H
