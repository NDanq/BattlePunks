#if WITH_EDITOR

#include "Public/Editor/ConsolePanel.h"
#include "Private/Editor/EditorIcons.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QStringListModel>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/Editor/EditorConsole.h"

namespace
{
    /* The transcript keeps at most this many lines of its own; the history
     * behind it is bounded separately (EditorLog::MaxEntries). */
    constexpr int MaxOutputBlocks = 50000;

    /* Mid-tones, legible on the light and the dark Windows palette alike. */
    QColor KindColor(const EditorLog::EKind InKind, const QPalette& InPalette)
    {
        switch(InKind)
        {
            case EditorLog::EKind::Warning:     return QColor(200, 140, 0);
            case EditorLog::EKind::Error:
            case EditorLog::EKind::Fatal:
            case EditorLog::EKind::OutputError: return QColor(222, 72, 64);
            case EditorLog::EKind::Input:       return QColor(70, 140, 230);
            default:                            return InPalette.color(QPalette::Text);
        }
    }

    QToolButton* MakeToggle(const QString& InToolTip, QWidget* InParent)
    {
        auto* Button = new QToolButton(InParent);
        Button->setCheckable(true);
        Button->setChecked(true);
        Button->setAutoRaise(true);
        Button->setToolTip(InToolTip);
        return Button;
    }
}

ConsolePanel::ConsolePanel(EditorConsole& InConsole, QWidget* InParent)
    : QWidget(InParent)
    , Console(InConsole)
{
    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(2, 2, 2, 2);
    Layout->setSpacing(2);

    // --- Filter bar ----------------------------------------------------------
    auto* Bar = new QHBoxLayout();
    Bar->setSpacing(2);

    LogToggle     = MakeToggle(QStringLiteral("Show log messages"), this);
    WarningToggle = MakeToggle(QStringLiteral("Show warnings"), this);
    ErrorToggle   = MakeToggle(QStringLiteral("Show errors"), this);
    TimeToggle    = MakeToggle(QStringLiteral("Prefix each line with the time it was logged"), this);
    TimeToggle->setText(QStringLiteral("Time"));
    TimeToggle->setChecked(false);

    Filter = new QLineEdit(this);
    Filter->setPlaceholderText(QStringLiteral("Filter"));
    Filter->setClearButtonEnabled(true);

    auto* ClearButton = new QToolButton(this);
    ClearButton->setText(QStringLiteral("Clear"));
    ClearButton->setIcon(EditorIcons::Get(EditorIcons::EIcon::Clear));
    ClearButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    ClearButton->setAutoRaise(true);
    ClearButton->setToolTip(QStringLiteral("Empty the console"));

    Bar->addWidget(LogToggle);
    Bar->addWidget(WarningToggle);
    Bar->addWidget(ErrorToggle);
    Bar->addSpacing(6);
    Bar->addWidget(Filter, 1);
    Bar->addWidget(TimeToggle);
    Bar->addWidget(ClearButton);
    Layout->addLayout(Bar);

    // --- Transcript ----------------------------------------------------------
    const QFont Mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    Output = new QPlainTextEdit(this);
    Output->setReadOnly(true);
    Output->setUndoRedoEnabled(false);
    Output->setMaximumBlockCount(MaxOutputBlocks);
    Output->setLineWrapMode(QPlainTextEdit::NoWrap);
    Output->setFont(Mono);
    Output->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    Layout->addWidget(Output, 1);

    // --- Command line --------------------------------------------------------
    auto* InputRow = new QHBoxLayout();
    auto* Prompt = new QLabel(QStringLiteral(">"), this);
    Prompt->setFont(Mono);
    Input = new QLineEdit(this);
    Input->setFont(Mono);
    Input->setPlaceholderText(QStringLiteral("Command or Section.Name [value] -- 'help' lists commands"));
    Input->installEventFilter(this);
    InputRow->addWidget(Prompt);
    InputRow->addWidget(Input, 1);
    Layout->addLayout(InputRow);

    CompletionModel = new QStringListModel(this);
    Completer = new QCompleter(CompletionModel, this);
    Completer->setCaseSensitivity(Qt::CaseInsensitive);
    Completer->setFilterMode(Qt::MatchStartsWith);
    Completer->setCompletionMode(QCompleter::PopupCompletion);
    Completer->setMaxVisibleItems(12);
    Input->setCompleter(Completer);

    // --- Wiring --------------------------------------------------------------
    for(QToolButton* Toggle : { LogToggle, WarningToggle, ErrorToggle, TimeToggle })
    {
        connect(Toggle, &QToolButton::toggled, this, &ConsolePanel::Rebuild);
    }
    connect(Filter, &QLineEdit::textChanged, this, &ConsolePanel::Rebuild);
    connect(ClearButton, &QToolButton::clicked, this, []()
    {
        if(EditorLog* Log = EditorLog::Get())
        {
            Log->Clear();
        }
    });
    connect(Input, &QLineEdit::returnPressed, this, &ConsolePanel::Submit);

    if(EditorLog* Log = EditorLog::Get())
    {
        connect(Log, &EditorLog::EntriesAppended, this, &ConsolePanel::OnEntriesAppended);
        connect(Log, &EditorLog::Cleared, this, &ConsolePanel::Rebuild);
    }

    RefreshCompletions();
    Rebuild();
}

void ConsolePanel::FocusInput()
{
    Input->setFocus(Qt::ShortcutFocusReason);
    Input->selectAll();
}

bool ConsolePanel::Accepts(const EditorLog::Entry& InEntry) const
{
    switch(InEntry.Kind)
    {
        case EditorLog::EKind::Log:     if(!LogToggle->isChecked())     return false; break;
        case EditorLog::EKind::Warning: if(!WarningToggle->isChecked()) return false; break;
        case EditorLog::EKind::Error:
        case EditorLog::EKind::Fatal:   if(!ErrorToggle->isChecked())   return false; break;
        default:                        return true;   // the console's own lines
    }

    const QString Needle = Filter->text();
    return Needle.isEmpty() || InEntry.Text.contains(Needle, Qt::CaseInsensitive);
}

void ConsolePanel::AppendEntry(const EditorLog::Entry& InEntry)
{
    QTextCharFormat Format;
    Format.setForeground(KindColor(InEntry.Kind, palette()));
    if(InEntry.Kind == EditorLog::EKind::Input)
    {
        Format.setFontWeight(QFont::Bold);
    }

    QString Text = InEntry.Kind == EditorLog::EKind::Input ? QStringLiteral("> ") + InEntry.Text : InEntry.Text;
    if(TimeToggle->isChecked())
    {
        /* Continuation lines indented under the first, past the stamp. */
        const QString Stamp = InEntry.Time.toString(QStringLiteral("HH:mm:ss.zzz "));
        Text.replace(QLatin1Char('\n'), QLatin1Char('\n') + QString(Stamp.size(), QLatin1Char(' ')));
        Text.prepend(Stamp);
    }

    QTextCursor Cursor(Output->document());
    Cursor.movePosition(QTextCursor::End);
    if(!Output->document()->isEmpty())
    {
        Cursor.insertBlock();
    }
    Cursor.insertText(Text, Format);
}

void ConsolePanel::OnEntriesAppended(const int InCount)
{
    const EditorLog* Log = EditorLog::Get();
    if(!Log)
    {
        return;
    }

    QScrollBar* Scroll = Output->verticalScrollBar();
    const bool bFollow = Scroll->value() >= Scroll->maximum() - 2;

    const std::deque<EditorLog::Entry>& Entries = Log->GetEntries();
    for(size_t Index = Entries.size() - size_t(InCount); Index < Entries.size(); ++Index)
    {
        if(Accepts(Entries[Index]))
        {
            AppendEntry(Entries[Index]);
        }
    }

    if(bFollow)
    {
        Scroll->setValue(Scroll->maximum());
    }
    UpdateCounts();
}

void ConsolePanel::Rebuild()
{
    Output->clear();
    if(const EditorLog* Log = EditorLog::Get())
    {
        for(const EditorLog::Entry& Each : Log->GetEntries())
        {
            if(Accepts(Each))
            {
                AppendEntry(Each);
            }
        }
    }
    Output->verticalScrollBar()->setValue(Output->verticalScrollBar()->maximum());
    UpdateCounts();
}

void ConsolePanel::UpdateCounts()
{
    const EditorLog* Log = EditorLog::Get();
    const int Logs     = Log ? Log->GetCount(EditorLog::EKind::Log) : 0;
    const int Warnings = Log ? Log->GetCount(EditorLog::EKind::Warning) : 0;
    const int Errors   = Log ? Log->GetCount(EditorLog::EKind::Error) + Log->GetCount(EditorLog::EKind::Fatal) : 0;

    LogToggle->setText(QStringLiteral("Log %1").arg(Logs));
    WarningToggle->setText(QStringLiteral("Warnings %1").arg(Warnings));
    ErrorToggle->setText(QStringLiteral("Errors %1").arg(Errors));
}

void ConsolePanel::Submit()
{
    const QString Line = Input->text().trimmed();
    Input->clear();
    Draft.clear();
    if(Line.isEmpty())
    {
        return;
    }

    if(History.isEmpty() || History.back() != Line)
    {
        History << Line;
    }
    HistoryCursor = int(History.size());

    /* Whatever the user was scrolled to, the reply is what they want to see. */
    Output->verticalScrollBar()->setValue(Output->verticalScrollBar()->maximum());

    Console.Execute(Line);
    RefreshCompletions();
}

void ConsolePanel::RecallHistory(const int InStep)
{
    if(History.isEmpty())
    {
        return;
    }
    if(HistoryCursor == int(History.size()))
    {
        Draft = Input->text();
    }

    HistoryCursor = std::clamp(HistoryCursor + InStep, 0, int(History.size()));
    Input->setText(HistoryCursor == int(History.size()) ? Draft : History[HistoryCursor]);
    Input->end(false);
}

void ConsolePanel::RefreshCompletions()
{
    CompletionModel->setStringList(Console.GetCompletions());
}

bool ConsolePanel::eventFilter(QObject* InWatched, QEvent* InEvent)
{
    if(InWatched == Input && InEvent->type() == QEvent::KeyPress && !Completer->popup()->isVisible())
    {
        const auto* Key = static_cast<QKeyEvent*>(InEvent);
        switch(Key->key())
        {
            case Qt::Key_Up:
                RecallHistory(-1);
                return true;
            case Qt::Key_Down:
                RecallHistory(+1);
                return true;
            case Qt::Key_Tab:
                /* Offer every completion for the word so far; Tab is
                 * otherwise focus traversal and would leave the line. */
                Completer->setCompletionPrefix(Input->text());
                if(Completer->completionCount() == 1)
                {
                    Input->setText(Completer->currentCompletion() + QLatin1Char(' '));
                }
                else if(Completer->completionCount() > 1)
                {
                    Completer->complete();
                }
                return true;
            case Qt::Key_Escape:
                Input->clear();
                HistoryCursor = int(History.size());
                return true;
            default:
                break;
        }
    }
    return QWidget::eventFilter(InWatched, InEvent);
}

#endif // WITH_EDITOR
