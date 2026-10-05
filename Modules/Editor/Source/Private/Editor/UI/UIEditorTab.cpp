#if WITH_EDITOR

#include "Private/Editor/UI/UIEditorTab.h"

#include <QAction>
#include <QComboBox>
#include <QDockWidget>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QUndoStack>

#include "Public/Editor/EditorContext.h"
#include "Private/Editor/UI/UIDesignDocument.h"
#include "Private/Editor/UI/UIDesignerViewport.h"
#include "Private/Editor/UI/UIDetailsPanel.h"
#include "Private/Editor/UI/UIGraphLink.h"
#include "Private/Editor/UI/UIHierarchyPanel.h"
#include "Private/Editor/UI/UIPalettePanel.h"

namespace
{
    /* The design resolutions offered; the last asks. */
    struct Preset { const char* Label; int Width, Height; };
    const Preset PRESETS[] = {
        { "1280 x 720 (720p)",       1280, 720 },
        { "1920 x 1080 (1080p)",     1920, 1080 },
        { "2560 x 1440 (1440p)",     2560, 1440 },
        { "3840 x 2160 (4K)",        3840, 2160 },
        { "3440 x 1440 (ultrawide)", 3440, 1440 },
        { "1080 x 1920 (portrait)",  1080, 1920 },
        { "1280 x 800 (16:10)",      1280, 800 },
        { "Custom...",               0, 0 },
    };
}

UIEditorTab::UIEditorTab(EditorContext& InContext, QWidget* InParent)
    : QMainWindow(InParent)
    , Context(InContext)
{
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks);
    Design = new UIDesignDocument(this);
    Canvas = new UIDesignerViewport(Context, *Design, this);
    setCentralWidget(Canvas);
    BuildDocks();
    BuildToolBar();

    Status = new QLabel(this);
    statusBar()->addWidget(Status, 1);
    connect(Canvas, &UIDesignerViewport::StatusText, Status, &QLabel::setText);
    connect(Details, &UIDetailsPanel::StatusText, Status, &QLabel::setText);
    connect(Design, &UIDesignDocument::DirtyChanged, this, &UIEditorTab::TitleChanged);

    /* The UI graph follows renamed widgets, and says which handlers a delete left behind.
     * Asked on the next turn: never inside the edit that changed the name. */
    connect(Design, &UIDesignDocument::WidgetRenamed, this, [this](const QString& InOld, const QString& InNew)
    {
        QTimer::singleShot(0, this, [this, InOld, InNew]()
        {
            const QStringList Handlers = EditorUIGraph::HandlersForWidget(*Design, InOld);
            if(Handlers.isEmpty()) return;
            QStringList Renamed;
            for(const QString& Each : Handlers)
            {
                if(Each.startsWith(EditorUIGraph::MakeHandlerName(InOld, QString()))) Renamed << Each;
            }
            bool bRename = false;
            if(!Renamed.isEmpty())
            {
                const QMessageBox::StandardButton Answer = QMessageBox::question(this, QStringLiteral("Rename Handlers"),
                    QStringLiteral("%1 has UI graph handlers named for it:\n\n%2\n\nRename them for %3 too (and the actions that call them)?")
                        .arg(InOld, Renamed.join(QStringLiteral(", ")), InNew),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                bRename = Answer == QMessageBox::Yes;
            }
            const QStringList Done = EditorUIGraph::RenameWidgetHandlers(*Design, InOld, InNew, bRename);
            Status->setText(Done.isEmpty() ? QStringLiteral("The UI graph's handlers now note %1").arg(InNew)
                                           : QStringLiteral("Renamed in the UI graph: %1").arg(Done.join(QStringLiteral(", "))));
        });
    });
    connect(Design, &UIDesignDocument::WidgetsRemoved, this, [this](const QStringList& InNames)
    {
        QStringList Orphans;
        for(const QString& Each : InNames) { Orphans << EditorUIGraph::HandlersForWidget(*Design, Each); }
        if(!Orphans.isEmpty())
        {
            Status->setText(QStringLiteral("The UI graph still has handlers for what was deleted: %1 (nothing calls them now)")
                            .arg(Orphans.join(QStringLiteral(", "))));
        }
    });
}

UIEditorTab::~UIEditorTab()
{
    /* The canvas first: it leaves the frame loop while its window exists. */
    delete Canvas;
    Canvas = nullptr;
}

void UIEditorTab::BuildDocks()
{
    auto* PaletteDock = new QDockWidget(QStringLiteral("Palette"), this);
    PaletteDock->setObjectName(QStringLiteral("UIPaletteDock"));
    Palette = new UIPalettePanel(PaletteDock);
    PaletteDock->setWidget(Palette);
    addDockWidget(Qt::LeftDockWidgetArea, PaletteDock);
    connect(Palette, &UIPalettePanel::AddRequested, this, &UIEditorTab::AddFromPalette);

    auto* HierarchyDock = new QDockWidget(QStringLiteral("Hierarchy"), this);
    HierarchyDock->setObjectName(QStringLiteral("UIHierarchyDock"));
    Hierarchy = new UIHierarchyPanel(*Design, HierarchyDock);
    HierarchyDock->setWidget(Hierarchy);
    addDockWidget(Qt::LeftDockWidgetArea, HierarchyDock);

    auto* DetailsDock = new QDockWidget(QStringLiteral("Details"), this);
    DetailsDock->setObjectName(QStringLiteral("UIDetailsDock"));
    Details = new UIDetailsPanel(*Design, DetailsDock);
    DetailsDock->setWidget(Details);
    addDockWidget(Qt::RightDockWidgetArea, DetailsDock);

    resizeDocks({ PaletteDock, HierarchyDock }, { 220, 360 }, Qt::Vertical);
    resizeDocks({ HierarchyDock, DetailsDock }, { 240, 360 }, Qt::Horizontal);
}

void UIEditorTab::BuildToolBar()
{
    QToolBar* Bar = addToolBar(QStringLiteral("UI"));
    Bar->setObjectName(QStringLiteral("UIToolBar"));
    Bar->setMovable(false);

    QAction* SaveAction = Bar->addAction(QStringLiteral("Save"), this, &UIEditorTab::Save);
    SaveAction->setShortcut(QKeySequence::Save);
    SaveAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    SaveAction->setToolTip(QStringLiteral("Save and stage the document (Ctrl+S); Play-in-Editor shows it at once"));
    addAction(SaveAction);

    QAction* UndoAction = Design->GetUndoStack()->createUndoAction(this, QStringLiteral("Undo"));
    UndoAction->setShortcut(QKeySequence::Undo);
    UndoAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    QAction* RedoAction = Design->GetUndoStack()->createRedoAction(this, QStringLiteral("Redo"));
    RedoAction->setShortcuts({ QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y) });
    RedoAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    Bar->addAction(UndoAction);
    Bar->addAction(RedoAction);
    addAction(UndoAction);
    addAction(RedoAction);
    Bar->addSeparator();

    Bar->addWidget(new QLabel(QStringLiteral(" Resolution "), Bar));
    Resolution = new QComboBox(Bar);
    for(const Preset& Each : PRESETS) { Resolution->addItem(QString::fromLatin1(Each.Label)); }
    Resolution->setCurrentIndex(1);
    Resolution->setToolTip(QStringLiteral("The screen the document is laid out for: try them all"));
    connect(Resolution, &QComboBox::activated, this, &UIEditorTab::ApplyResolution);
    Bar->addWidget(Resolution);

    Bar->addAction(QStringLiteral("Fit"), Canvas, &UIDesignerViewport::FitToView)->setToolTip(QStringLiteral("Fit the frame in the view (F)"));
    Bar->addAction(QStringLiteral("100%"), this, [this]() { Canvas->SetZoom(float(devicePixelRatioF())); })
        ->setToolTip(QStringLiteral("One design pixel to one screen pixel"));
    ZoomLabel = new QLabel(Bar);
    Bar->addWidget(ZoomLabel);
    connect(Canvas, &UIDesignerViewport::ZoomChanged, this, [this](const float InScale)
    {
        ZoomLabel->setText(QStringLiteral("  %1%").arg(int(std::lround(InScale / devicePixelRatioF() * 100.0))));
    });
    Bar->addSeparator();

    QAction* SnapAction = Bar->addAction(QStringLiteral("Snap"));
    SnapAction->setCheckable(true);
    SnapAction->setChecked(Canvas->IsSnap());
    SnapAction->setToolTip(QStringLiteral("Moves and resizes snap to a 4-unit grid"));
    connect(SnapAction, &QAction::toggled, this, [this](const bool bOn) { Canvas->SetSnap(bOn); });

    PreviewAction = Bar->addAction(QStringLiteral("Preview"));
    PreviewAction->setCheckable(true);
    PreviewAction->setToolTip(QStringLiteral("Hover, click and navigate as in the game; actions are logged, not run"));
    connect(PreviewAction, &QAction::toggled, this, [this](const bool bOn)
    {
        Canvas->SetPreview(bOn);
        Status->setText(bOn ? QStringLiteral("Preview: the document takes input as in a game (arrows, Return, Escape, Tab)")
                            : QStringLiteral("Design"));
    });
}

void UIEditorTab::ApplyResolution(const int InIndex)
{
    if(InIndex < 0 || InIndex >= int(sizeof(PRESETS) / sizeof(PRESETS[0]))) return;
    const Preset& Chosen = PRESETS[InIndex];
    if(Chosen.Width > 0)
    {
        Canvas->SetDesignSize(QSize(Chosen.Width, Chosen.Height));
        Canvas->FitToView();
        return;
    }
    bool bOk = false;
    const QString Text = QInputDialog::getText(this, QStringLiteral("Resolution"), QStringLiteral("Width x Height:"), QLineEdit::Normal,
                                               QStringLiteral("%1x%2").arg(Canvas->GetDesignSize().width()).arg(Canvas->GetDesignSize().height()), &bOk);
    const QStringList Parts = Text.split(QLatin1Char('x'), Qt::SkipEmptyParts);
    if(bOk && Parts.size() == 2 && Parts[0].trimmed().toInt() > 0 && Parts[1].trimmed().toInt() > 0)
    {
        Canvas->SetDesignSize(QSize(Parts[0].trimmed().toInt(), Parts[1].trimmed().toInt()));
        Canvas->FitToView();
        Resolution->setItemText(InIndex, QStringLiteral("%1 (custom)").arg(Text.trimmed()));
    }
}

void UIEditorTab::AddFromPalette(const QString& InType)
{
    const UI::UIDocumentAsset& Asset = Design->GetAsset();
    int Parent = Design->GetSelected() >= 0 ? Design->GetSelected() : 0;
    while(Parent > 0 && !Asset.CanTakeChild(Parent)) { Parent = Asset.Widgets[size_t(Parent)].Parent; }
    std::vector<UI::UIPropertyText> Properties;
    if(InType == QStringLiteral("Text")) Properties.push_back({ "Text", "Text" });
    if(Design->AddWidget(InType, Parent, -1, Properties) < 0)
    {
        Status->setText(QStringLiteral("No room for a %1 in the selection or its parents").arg(InType));
    }
}

bool UIEditorTab::Load(const QString& InPath, QString* OutError)
{
    return Design->Load(InPath, OutError);
}

const QString& UIEditorTab::GetPath() const
{
    return Design->GetPath();
}

QString UIEditorTab::GetTabTitle() const
{
    const QString Name = QFileInfo(Design->GetPath()).fileName();
    return Design->IsDirty() ? Name + QStringLiteral("*") : Name;
}

bool UIEditorTab::IsDirty() const
{
    return Design->IsDirty();
}

bool UIEditorTab::Save()
{
    QString Error;
    if(!Design->Save(&Error))
    {
        QMessageBox::warning(this, QStringLiteral("Save UI"), QStringLiteral("Could not save: %1").arg(Error));
        return false;
    }
    Status->setText(QStringLiteral("Saved and staged %1").arg(QFileInfo(Design->GetPath()).fileName()));
    emit TitleChanged();
    return true;
}

bool UIEditorTab::ConfirmClose()
{
    if(!IsDirty()) return true;
    const QMessageBox::StandardButton Answer = QMessageBox::question(
        this, QStringLiteral("UI Designer"), QStringLiteral("Save changes to %1?").arg(QFileInfo(Design->GetPath()).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if(Answer == QMessageBox::Cancel) return false;
    if(Answer == QMessageBox::Save) return Save();
    return true;
}

#endif // WITH_EDITOR
