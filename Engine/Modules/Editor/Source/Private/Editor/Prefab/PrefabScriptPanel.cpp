#if WITH_EDITOR

#include "Private/Editor/Prefab/PrefabScriptPanel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStackedWidget>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVBoxLayout>

#include "Public/NodeGraph/Schema.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Prefab/PrefabAssets.h"

namespace
{
    /* Keywords, strings, numbers and comments -- enough to read a script by. */
    class LuaHighlighter : public QSyntaxHighlighter
    {
    public:
        explicit LuaHighlighter(QTextDocument* InDocument) : QSyntaxHighlighter(InDocument)
        {
            Keyword.setForeground(QColor(86, 156, 214));
            Keyword.setFontWeight(QFont::Bold);
            String.setForeground(QColor(206, 145, 120));
            Number.setForeground(QColor(181, 206, 168));
            Comment.setForeground(QColor(106, 153, 85));
            Callback.setForeground(QColor(220, 220, 170));
        }

    protected:
        void highlightBlock(const QString& InText) override
        {
            static const QRegularExpression Keywords(QStringLiteral(
                "\\b(and|break|do|else|elseif|end|false|for|function|goto|if|in|local|nil|not|or|repeat|return|then|true|until|while|self)\\b"));
            static const QRegularExpression Callbacks(QStringLiteral(
                "\\b(On(Start|Destroy|Tick|PrePhysics|Physics|PostPhysics|PostUpdate|OverlapBegin|OverlapEnd))\\b"));
            static const QRegularExpression Numbers(QStringLiteral("\\b\\d+(\\.\\d+)?\\b"));
            static const QRegularExpression Strings(QStringLiteral("\"([^\"\\\\]|\\\\.)*\"|'([^'\\\\]|\\\\.)*'"));

            const auto Apply = [this, &InText](const QRegularExpression& InPattern, const QTextCharFormat& InFormat)
            {
                for(auto It = InPattern.globalMatch(InText); It.hasNext();)
                {
                    const QRegularExpressionMatch Match = It.next();
                    setFormat(int(Match.capturedStart()), int(Match.capturedLength()), InFormat);
                }
            };
            Apply(Keywords, Keyword);
            Apply(Callbacks, Callback);
            Apply(Numbers, Number);
            Apply(Strings, String);

            /* Last, over anything a comment contains. Block comments are not
             * tracked across lines. */
            const int Dashes = int(InText.indexOf(QStringLiteral("--")));
            if(Dashes >= 0) { setFormat(Dashes, int(InText.size()) - Dashes, Comment); }
        }

    private:
        QTextCharFormat Keyword, String, Number, Comment, Callback;
    };

    bool WriteText(const QString& InPath, const QString& InText)
    {
        QDir().mkpath(QFileInfo(InPath).absolutePath());
        QSaveFile File(InPath);
        if(!File.open(QIODevice::WriteOnly | QIODevice::Text)) { return false; }
        File.write(InText.toUtf8());
        return File.commit();
    }
}

PrefabScriptPanel::PrefabScriptPanel(QWidget* InParent)
    : QWidget(InParent)
{
    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(4, 4, 4, 4);

    PathLabel = new QLabel(this);
    PathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Layout->addWidget(PathLabel);

    Pages = new QStackedWidget(this);
    Layout->addWidget(Pages, 1);

    NoneNote = new QLabel(QStringLiteral("No script. Pick Lua or Graph under Script in the toolbar to make one beside the "
                                         "prefab, or Choose an existing one."), this);
    NoneNote->setWordWrap(true);
    NoneNote->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    NoneNote->setStyleSheet(QStringLiteral("color: gray;"));
    Pages->addWidget(NoneNote);

    LuaEditor = new QPlainTextEdit(this);
    LuaEditor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    LuaEditor->setLineWrapMode(QPlainTextEdit::NoWrap);
    LuaEditor->setTabStopDistance(4 * LuaEditor->fontMetrics().horizontalAdvance(QLatin1Char(' ')));
    new LuaHighlighter(LuaEditor->document());
    connect(LuaEditor->document(), &QTextDocument::modificationChanged, this, &PrefabScriptPanel::ModifiedChanged);
    Pages->addWidget(LuaEditor);

    auto* GraphPage = new QWidget(this);
    auto* GraphLayout = new QVBoxLayout(GraphPage);
    GraphLayout->setContentsMargins(0, 0, 0, 0);
    OpenGraphButton = new QPushButton(QStringLiteral("Open in Graph Editor"), GraphPage);
    connect(OpenGraphButton, &QPushButton::clicked, this, [this]()
    {
        if(!FilePath.isEmpty()) { EditorGraphs::OpenGraph(FilePath, window()); }
    });
    GraphLayout->addWidget(OpenGraphButton, 0, Qt::AlignLeft);
    auto* GraphNote = new QLabel(QStringLiteral("A script graph is edited, saved and staged in the Graph Editor."), GraphPage);
    GraphNote->setStyleSheet(QStringLiteral("color: gray;"));
    GraphLayout->addWidget(GraphNote);
    GraphLayout->addStretch(1);
    Pages->addWidget(GraphPage);

    SetScript(QString());
}

void PrefabScriptPanel::SetScript(const QString& InRuntimePath)
{
    RuntimePath = InRuntimePath;
    FilePath    = InRuntimePath.isEmpty() ? QString() : EditorPrefabs::ToProjectPath(InRuntimePath);

    if(InRuntimePath.isEmpty())
    {
        PathLabel->setText(QStringLiteral("Script: none"));
        Pages->setCurrentWidget(NoneNote);
    }
    else if(InRuntimePath.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive))
    {
        PathLabel->setText(QStringLiteral("Script: %1 (Lua)").arg(InRuntimePath));
        QFile File(FilePath);
        const bool bRead = File.open(QIODevice::ReadOnly | QIODevice::Text);
        LuaEditor->setPlainText(bRead ? QString::fromUtf8(File.readAll())
                                      : QStringLiteral("-- %1 could not be read").arg(QDir::toNativeSeparators(FilePath)));
        LuaEditor->document()->setModified(false);
        Pages->setCurrentWidget(LuaEditor);
    }
    else
    {
        PathLabel->setText(QStringLiteral("Script: %1 (Graph)").arg(InRuntimePath));
        Pages->setCurrentIndex(2);
    }
    emit ModifiedChanged();
}

bool PrefabScriptPanel::IsModified() const
{
    return Pages->currentWidget() == LuaEditor && LuaEditor->document()->isModified();
}

bool PrefabScriptPanel::Save(QString& OutError)
{
    if(!IsModified())
    {
        return true;
    }
    if(!WriteText(FilePath, LuaEditor->toPlainText()))
    {
        OutError = QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(FilePath));
        return false;
    }
    EditorPrefabs::Stage(FilePath);
    LuaEditor->document()->setModified(false);
    return true;
}

void PrefabScriptPanel::StageForRun()
{
    if(Pages->currentWidget() != LuaEditor)
    {
        return;   // a graph stages itself when saved; none has nothing
    }
    if(!IsModified())
    {
        EditorPrefabs::Stage(FilePath);   // a script written outside the editor may never have been
        return;
    }
    const QString Staged = EditorGraphs::GetStagedPath(FilePath);
    if(!Staged.isEmpty())
    {
        WriteText(Staged, LuaEditor->toPlainText());
    }
}

QString PrefabScriptPanel::StarterLua(const QString& InPrefabName)
{
    return QStringLiteral(
        "-- %1: each placed %1 runs its own copy of this script.\n"
        "-- self.Root is the prefab's root entity; self.Instance:Find(\"Name\") finds an entity\n"
        "-- in it by its name in the prefab.\n"
        "return {\n"
        "    OnStart = function(self)\n"
        "    end,\n"
        "\n"
        "    -- Define only the phases you need: OnPrePhysics, OnPhysics, OnPostPhysics, OnPostUpdate.\n"
        "    OnPrePhysics = function(self, dt)\n"
        "    end,\n"
        "}\n").arg(InPrefabName);
}

#endif // WITH_EDITOR
