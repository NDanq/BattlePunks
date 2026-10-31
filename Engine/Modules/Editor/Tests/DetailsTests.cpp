/*
 * NimbleEditorDetailsTests -- the Details panel, offscreen: what it builds for
 * a component's properties, struct arrays above all (a RenderComponent's
 * material parameter overrides) -- and the Texture Viewer.
 * NIMBLE_DETAILS_SNAPSHOT=<file.png> also saves the panel as drawn, for a
 * look; NIMBLE_TEXTURE_SNAPSHOT=<file.png> the Texture Viewer;
 * NIMBLE_DOCS_SNAPSHOT=<file.png> Help > Documentation (at NIMBLE_DOCS_PAGE,
 * an .md, if given); NIMBLE_PICKER_SNAPSHOT=<file.png> the Add Component
 * picker, searching NIMBLE_PICKER_FILTER if given.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QKeyEvent>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreConfig.h"
#include "Private/Editor/SettingsDialog.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/ViewportPanel.h"
#include "Public/Editor/ViewportWidget.h"
#include "Public/RenderCore/TextureAsset.h"
#include "Private/Editor/Content/TextureViewerWindow.h"
#include "Private/Editor/Content/AnimStateEditorWindow.h"
#include "Private/Editor/Content/AnimStateGraph.h"
#include "Private/Editor/Content/ClipTimeline.h"
#include "Private/Editor/Build/ProjectBuild.h"
#include "Private/Editor/ComponentDocs.h"
#include "Private/Editor/ComponentPicker.h"
#include "Private/Editor/DocumentationWindow.h"
#include "Private/Editor/Graph/GraphDocument.h"
#include "Private/Editor/Graph/GraphEditorTab.h"
#include "Public/NodeGraph/GraphFile.h"
#include "Public/NodeGraph/Schema.h"
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QTextBlock>
#include <QTextBrowser>
#include "Private/Editor/Graph/GraphView.h"
#include <QGraphicsItem>
#include <QMouseEvent>
#include "Public/RenderCore/AnimStateAsset.h"

#include "Public/Editor/DetailsPanel.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/RenderComponent.h"

using GameCore::EntityID;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const char* What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What);
    }
}

namespace
{
    /* Events for InMs: the panel reads values back on a short timer. */
    void PumpFor(const int InMs)
    {
        QElapsedTimer Clock;
        Clock.start();
        while(Clock.elapsed() < InMs)
        {
            QApplication::processEvents(QEventLoop::AllEvents, 10);
        }
    }

    bool HasLabel(const QWidget& InRoot, const QString& InText)
    {
        for(const QLabel* Each : InRoot.findChildren<QLabel*>())
        {
            if(Each->text() == InText) { return true; }
        }
        return false;
    }

    int CountLabels(const QWidget& InRoot, const QString& InText)
    {
        int Count = 0;
        for(const QLabel* Each : InRoot.findChildren<QLabel*>())
        {
            if(Each->text() == InText) { ++Count; }
        }
        return Count;
    }

    bool HasLineEdit(const QWidget& InRoot, const QString& InText)
    {
        for(const QLineEdit* Each : InRoot.findChildren<QLineEdit*>())
        {
            if(Each->text() == InText) { return true; }
        }
        return false;
    }
}

static void TestMaterialParameters()
{
    std::printf("Material parameter overrides\n");

    EditorDocument Doc;
    const EntityID Brick = Doc.CreateEntity(QStringLiteral("Brick"), EngineCore::Vector3(0.0, 0.0, 0.0));

    RenderCore::RenderComponent Render;
    std::snprintf(Render.Materials[0], sizeof(Render.Materials[0]), "%s", "Materials/Brick.material.nmgraph");
    Render.SetMaterialParameter(0, "Color", 1.0f, 0.3f, 0.3f, 1.0f);
    Render.SetMaterialParameter(1, "Glow", 2.5f, 0.0f, 0.0f, 0.0f);
    Doc.GetWorld().GetEntity(Brick).AddComponent(Render);

    DetailsPanel Panel(Doc);
    Panel.resize(460, 1600);
    Panel.show();   // it reads values only while visible (offscreen: no window appears)
    Doc.SetSelection(Brick);
    PumpFor(300);

    Expect(HasLabel(Panel, QStringLiteral("Parameters")) && HasLabel(Panel, QStringLiteral("2 entries")),
           "the overrides are one list, with an entry each");
    Expect(HasLabel(Panel, QStringLiteral("    0")) && HasLabel(Panel, QStringLiteral("    1"))
           && !HasLabel(Panel, QStringLiteral("    2")), "each override in use is an element; the unused ones are not shown");
    Expect(CountLabels(Panel, QStringLiteral("        Name")) == 2 && CountLabels(Panel, QStringLiteral("        Slot")) == 2
           && CountLabels(Panel, QStringLiteral("        W")) == 2, "each element shows its members");
    Expect(HasLineEdit(Panel, QStringLiteral("Color")) && HasLineEdit(Panel, QStringLiteral("Glow")),
           "with their values: the parameter names");

    /* Written by the panel's editor, the component sees it. */
    for(QLineEdit* Each : Panel.findChildren<QLineEdit*>())
    {
        if(Each->text() == QStringLiteral("Glow"))
        {
            Each->setText(QStringLiteral("Pulse"));
            emit Each->editingFinished();
        }
    }
    PumpFor(300);
    float Value[4] = {};
    const RenderCore::RenderComponent* Live = Doc.GetWorld().GetEntity(Brick).GetComponent<RenderCore::RenderComponent>();
    Expect(Live && Live->GetMaterialParameter(1, "Pulse", Value) && Value[0] == 2.5f && !Live->GetMaterialParameter(1, "Glow", Value),
           "editing a member writes that element of the component");

    if(const char* Snapshot = std::getenv("NIMBLE_DETAILS_SNAPSHOT"))
    {
        Panel.grab().save(QString::fromUtf8(Snapshot));
        std::printf("  saved %s\n", Snapshot);
    }
}

namespace
{
    /* The combo whose items include InItem. */
    QComboBox* ComboWith(const QWidget& InRoot, const QString& InItem)
    {
        for(QComboBox* Each : InRoot.findChildren<QComboBox*>())
        {
            if(Each->findText(InItem, Qt::MatchStartsWith) >= 0) { return Each; }
        }
        return nullptr;
    }

    QPushButton* Button(const QWidget& InRoot, const QString& InText)
    {
        for(QPushButton* Each : InRoot.findChildren<QPushButton*>())
        {
            if(Each->text() == InText) { return Each; }
        }
        return nullptr;
    }

    bool AnyLabelContains(const QWidget& InRoot, const QString& InText)
    {
        for(const QLabel* Each : InRoot.findChildren<QLabel*>())
        {
            if(Each->text().contains(InText)) { return true; }
        }
        return false;
    }

    bool ReadTextureFile(const QString& InPath, RenderCore::TextureData& OutTexture)
    {
        QFile File(InPath);
        if(!File.open(QIODevice::ReadOnly)) { return false; }
        const QByteArray Bytes = File.readAll();
        EngineCore::Archive Ar = EngineCore::Archive::ForReading(std::vector<uint8_t>(Bytes.begin(), Bytes.end()));
        return RenderCore::SerializeTexture(Ar, OutTexture);
    }
}

/* The Texture Viewer, offscreen: settings edited, mips rebuilt and shown, saved. */
/* The entity section's Detail Mode row (GameCore's DetailMode.h): there,
 * listing the four tiers, setting the bucket as one undo step. */
static void TestDetailModeRow()
{
    std::printf("Detail Mode row\n");

    EditorDocument Doc;
    const EntityID Vase = Doc.CreateEntity(QStringLiteral("Vase"), EngineCore::Vector3(0.0, 0.0, 0.0));

    DetailsPanel Panel(Doc);
    Panel.resize(460, 1200);
    Panel.show();
    Doc.SetSelection(Vase);
    PumpFor(200);

    Expect(HasLabel(Panel, QStringLiteral("Detail Mode")), "the entity section has a Detail Mode row");
    QComboBox* Combo = nullptr;
    for(QComboBox* Each : Panel.findChildren<QComboBox*>())
    {
        if(Each->count() == 4 && Each->itemText(0) == QStringLiteral("Low") && Each->itemText(3) == QStringLiteral("Ultra")) { Combo = Each; }
    }
    Expect(Combo != nullptr && Combo->currentText() == QStringLiteral("Low"), "its combo lists Low to Ultra, at Low");
    if(!Combo) { return; }

    Combo->setCurrentIndex(2);
    emit Combo->activated(2);
    PumpFor(50);
    Expect(Doc.GetWorld().GetEntity(Vase).GetDetailMode() == GameCore::EDetailMode::High, "choosing High sets the bucket");
    Doc.Undo();
    PumpFor(50);
    Expect(Doc.GetWorld().GetEntity(Vase).GetDetailMode() == GameCore::EDetailMode::Low, "and one undo puts it back");
    Expect(Combo->currentText() == QStringLiteral("Low"), "the row follows the undo");
}

static void TestTextureViewer()
{
    std::printf("Texture Viewer\n");
    QTemporaryDir Temp;
    const QString Path = Temp.filePath(QStringLiteral("Gradient.nmtexture"));
    {
        RenderCore::TextureData Texture;
        Texture.Pixels = EngineCore::Image(64, 32, EngineCore::EImageFormat::RGBA8);
        for(int Y = 0; Y < 32; ++Y)
        {
            for(int X = 0; X < 64; ++X)
            {
                uint8_t* Px = Texture.Pixels.GetScanline(Y) + X * 4;
                Px[0] = uint8_t(X * 4); Px[1] = uint8_t(Y * 8); Px[2] = ((X / 8 + Y / 8) & 1) ? 220 : 40; Px[3] = 255;
            }
        }
        Texture.ImportInfo.SourcePath = "Source/Gradient.png";
        Texture.ImportInfo.Settings   = "SRGB=1\nFilter=1\nWrap=0\nMaxDimension=4096\nFlipVertically=0\n";   // from before mips
        RenderCore::SaveTextureFile(Path.toStdString(), Texture);
    }

    TextureViewerWindow* Viewer = TextureViewerWindow::Open(Path, nullptr);
    Expect(Viewer != nullptr, "the viewer opens a texture");
    if(!Viewer) { return; }
    PumpFor(50);

    QComboBox* Levels      = ComboWith(*Viewer, QStringLiteral("0: 64 x 32"));
    QComboBox* MipGen      = ComboWith(*Viewer, QStringLiteral("Kaiser"));
    QComboBox* Compression = ComboWith(*Viewer, QStringLiteral("BC7"));
    QPushButton* Save      = Button(*Viewer, QStringLiteral("Save"));
    Expect(Levels && Levels->count() == 1 && MipGen && Compression && Save, "it lists one level for a texture without mips");
    Expect(AnyLabelContains(*Viewer, QStringLiteral("64 x 32")) && AnyLabelContains(*Viewer, QStringLiteral("No mips"))
           && AnyLabelContains(*Viewer, QStringLiteral("GPU memory 8")), "and its statistics: size, no mips, 8 KB on the GPU");
    if(!Levels || !MipGen || !Compression || !Save) { delete Viewer; return; }
    Expect(!Save->isEnabled(), "nothing to save yet");

    MipGen->setCurrentIndex(MipGen->findData(int(RenderCore::ETextureMipGen::Box)));
    PumpFor(20);
    Expect(Levels->count() == 7 && Levels->itemText(6) == QStringLiteral("6: 1 x 1"), "Box mips: seven levels, 64 x 32 down to 1 x 1");
    Expect(Save->isEnabled() && Viewer->windowTitle().contains(QLatin1Char('*')), "and the texture is dirty");
    Levels->setCurrentIndex(3);   // shown: 8 x 4

    Compression->setCurrentIndex(Compression->findData(int(RenderCore::ETextureCompression::BC7)));
    PumpFor(20);
    Expect(AnyLabelContains(*Viewer, QStringLiteral("Encoded on save by bc7enc_rdo"))
           && AnyLabelContains(*Viewer, QStringLiteral("once BC7")), "compression names its encoder, and what it will take");

    /* As compressed: encoded now, and kept for Save. */
    QCheckBox* AsCompressed = nullptr;
    for(QCheckBox* Each : Viewer->findChildren<QCheckBox*>())
    {
        if(Each->text() == QStringLiteral("As compressed")) { AsCompressed = Each; }
    }
    Expect(AsCompressed && AsCompressed->isEnabled(), "the compressed preview can be asked for");
    if(AsCompressed) { AsCompressed->setChecked(true); }
    PumpFor(20);
    Expect(AnyLabelContains(*Viewer, QStringLiteral("as BC7 blocks")) && AnyLabelContains(*Viewer, QStringLiteral("Encoded by bc7enc_rdo")),
           "which encodes it: the GPU memory is the blocks'");

    if(const char* Snapshot = std::getenv("NIMBLE_TEXTURE_SNAPSHOT"))
    {
        Viewer->grab().save(QString::fromUtf8(Snapshot));
        std::printf("  saved %s\n", Snapshot);
    }

    Save->click();
    PumpFor(20);
    RenderCore::TextureData Back;
    Expect(ReadTextureFile(Path, Back) && Back.GetMipCount() == 7 && Back.MipGen == RenderCore::ETextureMipGen::Box
           && Back.Compression == RenderCore::ETextureCompression::BC7, "Save writes the chain and the settings");
    Expect(Back.Blocks.size() == 7 && Back.Blocks[0].Bytes.size() == 64 * 32 && Back.Blocks[6].Bytes.size() == 16
           && Back.Pixels.GetWidth() == 64, "and the BC7 blocks of every level, beside the RGBA8 source");
    Expect(QString::fromStdString(Back.ImportInfo.Settings).contains(QStringLiteral("MipGen=1"))
           && QString::fromStdString(Back.ImportInfo.Settings).contains(QStringLiteral("Compression=5")),
           "and records them in the import options, so Reimport keeps them");
    Expect(!Save->isEnabled() && !Viewer->windowTitle().contains(QLatin1Char('*')), "and it is clean again");

    MipGen->setCurrentIndex(MipGen->findData(int(RenderCore::ETextureMipGen::None)));
    PumpFor(20);
    Expect(Levels->count() == 1, "None drops the chain");
    Button(*Viewer, QStringLiteral("Revert"))->click();
    PumpFor(20);
    Expect(Levels->count() == 7 && !Save->isEnabled(), "Revert brings back what was saved");

    Expect(TextureViewerWindow::Open(Path, nullptr) == Viewer, "opening it again brings the same window forward");
    delete Viewer;
}

/*
 * The viewport's view settings outlive a session: saved with the Editor
 * Settings (EditorSettings.ini, here under Qt's test paths), read back at
 * startup, applied to a new viewport, and written again when the viewport
 * changes them -- after a short delay, or as it closes.
 */
static void TestViewSettings()
{
    std::printf("[view settings]\n");
    QStandardPaths::setTestModeEnabled(true);
    QFile::remove(SettingsDialog::GetUserSettingsPath());

    const auto Var = [](const char* InPath) { return EngineCore::ConfigFindVarByPath(InPath); };
    const char* const Paths[] = { "Editor.ViewGizmoMode", "Editor.ViewGizmoSpace", "Editor.ViewGizmoSnap", "Editor.ViewMode",
                                  "Editor.ViewAllBounds", "Editor.ViewLightGizmos", "Editor.ViewPlayerStarts", "Editor.ViewCameraSpeed", "Rendering.BakedLighting" };
    bool bAll = true;
    for(const char* Path : Paths) { bAll &= Var(Path) != nullptr; }
    Expect(bAll, "every view setting is a config var");
    if(!bAll) { return; }
    const auto Reset = [&]() { for(const char* Path : Paths) { Var(Path)->SetFromString(Var(Path)->DefaultToString()); } };
    const auto FileText = []()
    {
        QFile File(SettingsDialog::GetUserSettingsPath());
        return File.open(QIODevice::ReadOnly) ? QString::fromUtf8(File.readAll()) : QString();
    };

    /* Saved and loaded, as the editor does at exit and startup. */
    Var("Editor.ViewGizmoMode")->SetInt(2);
    Var("Editor.ViewGizmoSnap")->SetBool(true);
    Var("Editor.ViewMode")->SetInt(1);
    Var("Editor.ViewAllBounds")->SetBool(true);
    Var("Editor.ViewLightGizmos")->SetBool(false);
    Var("Editor.ViewPlayerStarts")->SetBool(false);
    Var("Editor.ViewCameraSpeed")->SetInt(5);
    Var("Rendering.BakedLighting")->SetBool(false);
    Expect(SettingsDialog::SaveUserSettings(), "the settings save");
    const QString Saved = FileText();
    Expect(Saved.contains(QStringLiteral("ViewGizmoMode=2")) && Saved.contains(QStringLiteral("ViewLightGizmos=false"))
           && Saved.contains(QStringLiteral("[Rendering]")) && Saved.contains(QStringLiteral("BakedLighting=false")),
           "the view is in EditorSettings.ini");
    Reset();
    SettingsDialog::LoadUserSettings();
    Expect(Var("Editor.ViewGizmoMode")->GetInt() == 2 && Var("Editor.ViewGizmoSnap")->GetBool() && Var("Editor.ViewMode")->GetInt() == 1
           && Var("Editor.ViewAllBounds")->GetBool() && !Var("Editor.ViewLightGizmos")->GetBool() && !Var("Editor.ViewPlayerStarts")->GetBool()
           && Var("Editor.ViewCameraSpeed")->GetInt() == 5 && !Var("Rendering.BakedLighting")->GetBool(),
           "and read back");

    /* A viewport opens as it was left, and remembers what it changes. */
    EditorContext Context;
    EditorDocument Document;
    {
        ViewportPanel Panel(Context, Document);
        ViewportWidget* View = Panel.GetViewport();
        Expect(View->GetGizmoMode() == TransformGizmo::EMode::Rotate && View->IsGizmoSnapEnabled() && View->IsShowingAllBounds()
               && !View->IsShowingLightGizmos() && !View->IsShowingPlayerStarts() && Context.GetViewMode() == RenderCore::ERenderViewMode::Unlit,
               "a new viewport takes the saved handles, snap, gizmos and view mode");

        View->SetGizmoMode(TransformGizmo::EMode::Scale);   // as its 4 key does
        Expect(Var("Editor.ViewGizmoMode")->GetInt() == 3, "changing the handles in the viewport sets the setting");
        PumpFor(1200);
        Expect(FileText().contains(QStringLiteral("ViewGizmoMode=3")), "and saves it shortly after");

        View->SetGizmoSpace(TransformGizmo::ESpace::Local);
    }
    Expect(FileText().contains(QStringLiteral("ViewGizmoSpace=1")), "a change not yet saved is saved as the viewport closes");

    Reset();
    Context.SetViewMode(RenderCore::ERenderViewMode::Lit);
    QFile::remove(SettingsDialog::GetUserSettingsPath());
    QStandardPaths::setTestModeEnabled(false);
}

/* The anim state editor's graph, offscreen: what is asked on it -- states
 * and transitions added and removed, a box moved, the entry set -- is what
 * Save writes. */
static void TestAnimStateGraph()
{
    std::printf("Anim state graph\n");

    RenderCore::AnimLayer Unplaced;
    Unplaced.States.resize(3);
    Unplaced.States[0].Name = "A";
    Unplaced.States[1].Name = "B";
    Unplaced.States[2].Name = "C";
    Unplaced.EntryState = 2;
    AnimStateGraph::LayOut(Unplaced);
    Expect(Unplaced.bEditorPlaced && Unplaced.States[2].EditorX == 0.0f && Unplaced.States[2].EditorY == 0.0f
           && Unplaced.States[0].EditorX > 0.0f, "a layer laid out: placed, the entry first");

    QTemporaryDir Temp;
    const QString Path = Temp.filePath(QStringLiteral("Graph.nmanimstate"));
    {
        RenderCore::AnimStateData Data;
        RenderCore::AnimLayer Layer;
        Layer.Name = "Base";
        Layer.States.resize(3);
        Layer.States[0].Name = "A";
        Layer.States[1].Name = "B";
        Layer.States[2].Name = "C";
        RenderCore::AnimTransition AB;
        AB.From = 0;
        AB.To   = 1;
        Layer.Transitions.push_back(AB);
        Data.Layers.push_back(Layer);
        RenderCore::SaveAnimStateFile(Path.toStdString(), Data);
    }

    AnimStateEditorWindow* Window = AnimStateEditorWindow::Open(Path, nullptr);
    AnimStateGraph* Graph = Window ? Window->findChild<AnimStateGraph*>() : nullptr;
    Expect(Window && Graph, "the editor opens with its graph");
    if(!Window || !Graph) { return; }
    PumpFor(50);

    emit Graph->AddTransitionRequested(-1, 2);     // Any -> C
    PumpFor(10);
    emit Graph->RemoveStateRequested(1);           // B, and A -> B with it; C becomes 1
    PumpFor(10);
    emit Graph->AddStateRequested(QPointF(500.0, 40.0));
    PumpFor(10);
    emit Graph->StateMoved(0, QPointF(-100.0, 60.0));
    emit Graph->EntryRequested(1);
    PumpFor(10);
    Expect(Window->Save(), "it saves");

    RenderCore::AnimStateData Back;
    Expect(RenderCore::LoadAnimStateFile(Path.toStdString(), Back) && Back.Layers.size() == 1, "and loads back");
    if(Back.Layers.size() == 1)
    {
        const RenderCore::AnimLayer& Layer = Back.Layers[0];
        Expect(Layer.States.size() == 3 && Layer.States[0].Name == "A" && Layer.States[1].Name == "C", "B removed, the rest in order, one added");
        Expect(Layer.Transitions.size() == 1 && Layer.Transitions[0].From == -1 && Layer.Transitions[0].To == 1,
               "its transition went with it; Any -> C renumbered");
        Expect(Layer.EntryState == 1, "the entry set");
        Expect(Layer.bEditorPlaced && Layer.States[0].EditorX == -100.0f && Layer.States[0].EditorY == 60.0f
               && Layer.States[2].EditorX == 500.0f, "where the boxes were put is kept");
    }

    /* Left-drag on empty canvas pans; a click there clears the selection. */
    if(GraphView* View = Graph->findChild<GraphView*>())
    {
        QWidget* Port = View->viewport();
        auto Send = [Port](const QEvent::Type InType, const QPoint& InAt, const Qt::MouseButtons InButtons)
        {
            QMouseEvent Event(InType, QPointF(InAt), Port->mapToGlobal(QPointF(InAt)),
                              InType == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, InButtons, Qt::NoModifier);
            QApplication::sendEvent(Port, &Event);
        };
        QPoint Empty(-1, -1);
        for(int Y = 4; Y < Port->height() && Empty.x() < 0; Y += 8)
        {
            for(int X = 4; X < Port->width() - 60; X += 8)
            {
                if(!View->itemAt(QPoint(X, Y))) { Empty = QPoint(X, Y); break; }
            }
        }
        Expect(Empty.x() >= 0, "the canvas has empty space");
        if(Empty.x() >= 0)
        {
            QGraphicsItem* Box = nullptr;
            for(QGraphicsItem* Each : View->scene()->items()) { if(Each->flags() & QGraphicsItem::ItemIsMovable) { Box = Each; break; } }
            const QPointF BoxAt  = Box ? Box->pos() : QPointF();
            const QPointF Before = View->mapToScene(Empty);
            Send(QEvent::MouseButtonPress, Empty, Qt::LeftButton);
            Send(QEvent::MouseMove, Empty + QPoint(30, 0), Qt::LeftButton);
            Send(QEvent::MouseMove, Empty + QPoint(50, 20), Qt::LeftButton);
            Send(QEvent::MouseButtonRelease, Empty + QPoint(50, 20), Qt::NoButton);
            const QPointF Moved = View->mapToScene(Empty) - Before;
            Expect(std::fabs(Moved.x() + 50.0 / View->transform().m11()) < 1.5 && std::fabs(Moved.y() + 20.0 / View->transform().m11()) < 1.5,
                   "left-drag on empty canvas pans the view");
            Expect(!Box || Box->pos() == BoxAt, "and moves no state");

            Graph->SetSelection(0, -1);
            const QPoint Still = View->mapFromScene(Before);
            if(!View->itemAt(Still))
            {
                Send(QEvent::MouseButtonPress, Still, Qt::LeftButton);
                Send(QEvent::MouseButtonRelease, Still, Qt::NoButton);
                Expect(View->scene()->selectedItems().isEmpty(), "a click on empty canvas clears the selection");
            }
        }
    }
    Window->close();
    PumpFor(10);
}

/* The clip timeline, offscreen: real mouse events -- scrub, drag a notify,
 * drag the Out handle, double-click to add. */
static void TestDocumentation()
{
    std::printf("Documentation\n");
    QTemporaryDir Dir;
    const auto Write = [&Dir](const QString& InRelative, const QByteArray& InText)
    {
        const QString Path = Dir.filePath(InRelative);
        QDir().mkpath(QFileInfo(Path).absolutePath());
        QFile File(Path);
        File.open(QIODevice::WriteOnly);
        File.write(InText);
    };
    Write(QStringLiteral("README.md"), "# Readme\n\nSee [the guide](Docs/Guide.md#c--you).\n");
    Write(QStringLiteral("Docs/Guide.md"), "# Guide\n\n## Intro\n\ntext\n\n## Intro\n\n## C++ & You\n\n| A | B |\n|---|---|\n| 1 | 2 |\n");
    Write(QStringLiteral("External/Lib/README.md"), "# vendored\n");
    Write(QStringLiteral("ThirdParty/Source/Lib/README.md"), "# vendored\n");
    Write(QStringLiteral("cmake-build-debug/Notes.md"), "# build\n");
    Write(QStringLiteral("Build/CMakeCache.txt"), "");
    Write(QStringLiteral("Build/Generated.md"), "# build\n");
    Write(QStringLiteral("Sub/Build/editor-debug/Notes.md"), "# build\n");
    Write(QStringLiteral(".git/Hidden.md"), "# hidden\n");
    EditorDocs::SetDocsRoot(Dir.path());   // the viewer shows only the docs root

    const QStringList Found = DocumentationWindow::FindMarkdownFiles(Dir.path());
    Expect(Found.size() == 2 && Found.first().endsWith(QStringLiteral("Docs/Guide.md")) && Found.last().endsWith(QStringLiteral("README.md")),
           "the .md files are found, skipping External/, ThirdParty/, build trees and hidden folders");

    Expect(DocumentationWindow::HeadingAnchor(QStringLiteral("Position in the tree")) == QStringLiteral("position-in-the-tree"),
           "a heading's anchor is GitHub's");
    Expect(DocumentationWindow::HeadingAnchor(QStringLiteral("C++ & You")) == QStringLiteral("c--you"),
           "punctuation is dropped from an anchor");

    DocumentationWindow* Window = DocumentationWindow::Open(Dir.filePath(QStringLiteral("README.md")));
    auto* Page = Window->findChild<QTextBrowser*>();
    Expect(Page && Page->source().toLocalFile().endsWith(QStringLiteral("README.md")), "the window opens at the file given");
    if(!Page) { return; }

    emit Page->anchorClicked(QUrl(QStringLiteral("Docs/Guide.md#c--you")));
    Expect(Page->source().toLocalFile().endsWith(QStringLiteral("Docs/Guide.md")), "a relative .md link opens in the window");

    QStringList Anchors;
    for(QTextBlock Block = Page->document()->begin(); Block.isValid(); Block = Block.next())
    {
        for(QTextBlock::iterator It = Block.begin(); !It.atEnd(); ++It)
        {
            Anchors << It.fragment().charFormat().anchorNames();
        }
    }
    Expect(Anchors.contains(QStringLiteral("intro")) && Anchors.contains(QStringLiteral("intro-1")) && Anchors.contains(QStringLiteral("c--you")),
           "headings get anchors, a repeated one numbered");

    Page->backward();
    Expect(Page->source().toLocalFile().endsWith(QStringLiteral("README.md")), "Back returns to the previous page");

    /* Only the docs root is shown: another folder's .md is refused, by path and by link. */
    QTemporaryDir Outside;
    {
        QFile Stray(Outside.filePath(QStringLiteral("Stray.md")));
        Stray.open(QIODevice::WriteOnly);
        Stray.write("# Stray\n");
    }
    Expect(!DocumentationWindow::IsDocumentation(Outside.filePath(QStringLiteral("Stray.md"))), "a file outside the docs root is not documentation");
    DocumentationWindow::Open(Outside.filePath(QStringLiteral("Stray.md")));
    Expect(Page->source().toLocalFile().endsWith(QStringLiteral("README.md")), "opening a path outside the docs root changes nothing");
    emit Page->anchorClicked(QUrl::fromLocalFile(Outside.filePath(QStringLiteral("Stray.md"))));
    Expect(Page->source().toLocalFile().endsWith(QStringLiteral("README.md")), "a link outside the docs root is not followed");

    /* A component's reference page is written on first ask, with both APIs, and kept. */
    const GameCore::ComponentTypeID Transform = GameCore::TransformComponent::StaticType().ID;
    const QString TransformDoc = EditorDocs::EnsureComponentDoc(Transform);
    Expect(TransformDoc == Dir.filePath(QStringLiteral("Components/Transform.md")) && QFileInfo(TransformDoc).isFile(),
           "a component's page is written under Components/");
    QFile Generated(TransformDoc);
    Generated.open(QIODevice::ReadOnly);
    const QString GeneratedText = QString::fromUtf8(Generated.readAll());
    Generated.close();
    Expect(GeneratedText.contains(QStringLiteral("## C++ API")) && GeneratedText.contains(QStringLiteral("## Lua API"))
           && GeneratedText.contains(QStringLiteral("Position")), "the page has the properties and both APIs");
    Generated.open(QIODevice::WriteOnly | QIODevice::Truncate);
    Generated.write("# Edited\n");
    Generated.close();
    EditorDocs::EnsureComponentDoc(Transform);
    Generated.open(QIODevice::ReadOnly);
    Expect(Generated.readAll().startsWith("# Edited"), "an existing page is never overwritten");
    Generated.close();
    Expect(DocumentationWindow::IsDocumentation(TransformDoc), "a component page is documentation");

    /* A screenshot wider than the page is drawn at the page's width, and refits as the window is resized. */
    QDir().mkpath(Dir.filePath(QStringLiteral("Images")));
    QImage Wide(2400, 300, QImage::Format_RGB32);
    Wide.fill(Qt::darkCyan);
    Expect(Wide.save(Dir.filePath(QStringLiteral("Images/Wide.png"))), "a wide screenshot is written");
    Write(QStringLiteral("Wide.md"), "# Wide\n\n![a wide shot](Images/Wide.png)\n");
    DocumentationWindow::Open(Dir.filePath(QStringLiteral("Wide.md")));
    const auto ImageWidth = [Page]()
    {
        for(QTextBlock Block = Page->document()->begin(); Block.isValid(); Block = Block.next())
        {
            for(QTextBlock::iterator It = Block.begin(); !It.atEnd(); ++It)
            {
                const QTextCharFormat Format = It.fragment().charFormat();
                if(Format.isImageFormat()) { return Format.toImageFormat().width(); }
            }
        }
        return -1.0;
    };
    Window->resize(900, 600);
    PumpFor(50);
    const qreal Narrow = ImageWidth();
    Expect(Narrow > 0.0 && Narrow <= Page->viewport()->width(), "the wide image fits the page instead of running off its edge");
    Window->resize(1600, 600);
    PumpFor(50);
    Expect(ImageWidth() > Narrow + 100.0 && ImageWidth() <= Page->viewport()->width(), "and refits when the window is resized");

    EditorDocs::SetDocsRoot(QString());
    DocumentationWindow::CloseIfOpen();
    PumpFor(10);

    /* The real thing -- the editor's project and engine docs -- for a look. */
    const QByteArray Snapshot = qgetenv("NIMBLE_DOCS_SNAPSHOT");
    if(!Snapshot.isEmpty())
    {
        DocumentationWindow* Real = DocumentationWindow::Open(QString::fromLocal8Bit(qgetenv("NIMBLE_DOCS_PAGE")));
        PumpFor(200);
        /* NIMBLE_DOCS_ANCHOR=<heading anchor>: the page scrolled to it. */
        if(!qEnvironmentVariableIsEmpty("NIMBLE_DOCS_ANCHOR"))
        {
            if(auto* Browser = Real->findChild<QTextBrowser*>()) { Browser->scrollToAnchor(QString::fromLocal8Bit(qgetenv("NIMBLE_DOCS_ANCHOR"))); }
            PumpFor(600);
        }
        Real->grab().save(QString::fromLocal8Bit(Snapshot));
        DocumentationWindow::CloseIfOpen();
        PumpFor(10);
    }
}

static void TestClipTimeline()
{
    std::printf("Clip timeline\n");
    ClipTimeline Timeline;
    Timeline.resize(320, 74);   // strip: x 10..310, y 22..52; 31 frames, 1 s: 300 px a second
    Timeline.SetClip(31, 30.0f);
    Timeline.SetMarkers({ { QStringLiteral("Step"), 0.5f } });
    Timeline.show();
    PumpFor(10);

    float Scrubbed = -1.0f, Moved = -1.0f;
    int   Dropped = -1;
    float Added = -1.0f;
    uint32_t RangeLast = 0;
    QObject::connect(&Timeline, &ClipTimeline::Scrubbed, [&](const float InT) { Scrubbed = InT; });
    QObject::connect(&Timeline, &ClipTimeline::MarkerMoved, [&](int, const float InT) { Moved = InT; });
    QObject::connect(&Timeline, &ClipTimeline::MarkerDropped, [&](const int InM) { Dropped = InM; });
    QObject::connect(&Timeline, &ClipTimeline::AddRequested, [&](const float InT) { Added = InT; });
    QObject::connect(&Timeline, &ClipTimeline::RangeChanged, [&](uint32_t, const uint32_t InLast) { RangeLast = InLast; });

    const auto Send = [&Timeline](const QEvent::Type InType, const QPointF& InAt, const Qt::MouseButtons InHeld)
    {
        QMouseEvent Event(InType, InAt, Timeline.mapToGlobal(InAt.toPoint()), InType == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                          InHeld, Qt::NoModifier);
        QApplication::sendEvent(&Timeline, &Event);
    };

    Send(QEvent::MouseButtonPress, QPointF(85.0, 40.0), Qt::LeftButton);    // a quarter in
    Send(QEvent::MouseButtonRelease, QPointF(85.0, 40.0), Qt::NoButton);
    Expect(std::fabs(Scrubbed - 0.25f) < 0.01f, "a click on the strip scrubs there");

    Send(QEvent::MouseButtonPress, QPointF(160.0, 20.0), Qt::LeftButton);   // the notify's diamond, at 0.5 s
    Send(QEvent::MouseMove, QPointF(250.0, 20.0), Qt::LeftButton);
    Send(QEvent::MouseButtonRelease, QPointF(250.0, 20.0), Qt::NoButton);
    Expect(std::fabs(Moved - 0.8f) < 0.01f && Dropped == 0, "a notify dragged moves -- to the frame it lands near -- and is dropped");

    Send(QEvent::MouseButtonPress, QPointF(310.0, 50.0), Qt::LeftButton);   // the Out handle, at the end
    Send(QEvent::MouseMove, QPointF(220.0, 50.0), Qt::LeftButton);
    Send(QEvent::MouseButtonRelease, QPointF(220.0, 50.0), Qt::NoButton);
    Expect(RangeLast == 21 && Timeline.GetRangeLast() == 21 && !Timeline.IsWholeClip(), "the Out handle dragged sets the range's end");

    Send(QEvent::MouseButtonDblClick, QPointF(70.0, 40.0), Qt::LeftButton);
    Expect(std::fabs(Added - 0.2f) < 0.01f, "a double-click asks for a notify there");

    Timeline.SetReadOnly(true);
    Added = -1.0f;
    Send(QEvent::MouseButtonDblClick, QPointF(70.0, 40.0), Qt::LeftButton);
    Expect(Added < 0.0f, "read-only, it adds nothing");
}

/* Build > Build Project's configure: tests and uncooked only in Debug and RelWithDebInfo, never Shipping. */
static void TestProjectBuildOptions()
{
    std::printf("Build Project options\n");
    using namespace EditorProjectBuild;
    Expect(IsDevelopment(BC_Debug) && IsDevelopment(BC_RelWithDebInfo) && !IsDevelopment(BC_Shipping),
           "Debug and RelWithDebInfo are development configurations; Shipping is not");

    BuildOptions Options;
    Options.bBuildTests  = true;
    Options.bRunUncooked = true;
    for(const EConfiguration Each : { BC_Debug, BC_RelWithDebInfo })
    {
        Options.Configuration = Each;
        const QStringList Args = ConfigureArgs(QStringLiteral("client-debug"), Options);
        Expect(Args.contains(QStringLiteral("-DNIMBLE_BUILD_TESTS=ON")) && Args.contains(QStringLiteral("-DNIMBLE_RUN_UNCOOKED=ON")),
               "a development build passes its tests and uncooked choices");
    }
    Options.Configuration = BC_Shipping;
    const QStringList Shipping = ConfigureArgs(QStringLiteral("client-shipping"), Options);
    Expect(Shipping.contains(QStringLiteral("-DNIMBLE_BUILD_TESTS=OFF")) && Shipping.contains(QStringLiteral("-DNIMBLE_RUN_UNCOOKED=OFF")),
           "Shipping never builds tests or runs uncooked, whatever the options say");

    Options.Configuration = BC_Debug;
    Options.bBuildTests   = false;
    Expect(ConfigureArgs(QStringLiteral("client-debug"), Options).contains(QStringLiteral("-DNIMBLE_BUILD_TESTS=OFF")),
           "tests off is passed too, so a tree that had them loses them");
}

/* NIMBLE_GRAPH_SNAPSHOT_FILE=<x.nmgraph> NIMBLE_GRAPH_SNAPSHOT=<png> [NIMBLE_GRAPH_SNAPSHOT_SCALE=2]:
 * a graph as the editor's canvas draws it (GraphEditorTab::SaveCanvasImage), for the docs. */
static void TestGraphSnapshot()
{
    const QByteArray File  = qgetenv("NIMBLE_GRAPH_SNAPSHOT_FILE");
    const QByteArray Image = qgetenv("NIMBLE_GRAPH_SNAPSHOT");
    if(File.isEmpty() || Image.isEmpty()) { return; }

    std::printf("Graph snapshot\n");
    NodeGraph::RefreshSchemas();
    NodeGraph::Graph Loaded;
    std::string Error;
    Expect(NodeGraph::LoadGraphFile(File.toStdString(), Loaded, Error), "the graph loads");

    GraphDocument Document(QString::fromLocal8Bit(File), Loaded);
    GraphEditorTab Tab(&Document);
    Tab.resize(1400, 800);
    Tab.show();
    PumpFor(300);

    bool bScaleOk = false;
    double Scale = qEnvironmentVariable("NIMBLE_GRAPH_SNAPSHOT_SCALE").toDouble(&bScaleOk);
    if(!bScaleOk || Scale <= 0.0) { Scale = 2.0; }
    QRectF Area;
    Expect(Tab.SaveCanvasImage(QString::fromLocal8Bit(Image), Scale, Area), "the canvas image is written");
    std::printf("  scene area %g,%g %gx%g at scale %g\n", Area.x(), Area.y(), Area.width(), Area.height(), Scale);
}

namespace
{
    /* The choosable (enabled) types under InItem, depth first. */
    void CollectLeaves(QTreeWidgetItem* InItem, QStringList& OutEnabled, QStringList& OutDisabled)
    {
        if(InItem->childCount() == 0)
        {
            ((InItem->flags() & Qt::ItemIsEnabled) ? OutEnabled : OutDisabled).push_back(InItem->text(0));
            return;
        }
        for(int Index = 0; Index < InItem->childCount(); ++Index) { CollectLeaves(InItem->child(Index), OutEnabled, OutDisabled); }
    }

    void CollectTree(const QTreeWidget& InTree, QStringList& OutEnabled, QStringList& OutDisabled)
    {
        for(int Index = 0; Index < InTree.topLevelItemCount(); ++Index) { CollectLeaves(InTree.topLevelItem(Index), OutEnabled, OutDisabled); }
    }

    ComponentPicker* OpenPicker(DetailsPanel& InPanel)
    {
        for(QToolButton* Each : InPanel.findChildren<QToolButton*>())
        {
            if(Each->text() == QStringLiteral("+ Add Component")) { Each->click(); break; }
        }
        PumpFor(50);
        ComponentPicker* Found = nullptr;
        for(ComponentPicker* Each : InPanel.findChildren<ComponentPicker*>())
        {
            if(Each->isVisible()) { Found = Each; }
        }
        return Found;
    }
}

static void TestAddComponentPicker()
{
    std::printf("Add Component picker\n");

    EditorDocument Doc;
    const EntityID Thing = Doc.CreateEntity(QStringLiteral("Thing"), EngineCore::Vector3(0.0, 0.0, 0.0));

    DetailsPanel Panel(Doc);
    Panel.resize(360, 700);
    Panel.show();
    Doc.SetSelection(Thing);
    PumpFor(300);

    ComponentPicker* Picker = OpenPicker(Panel);
    Expect(Picker != nullptr, "+ Add Component opens the picker");
    if(!Picker) { return; }
    auto* Tree   = Picker->findChild<QTreeWidget*>();
    auto* Search = Picker->findChild<QLineEdit*>();
    Expect(Tree && Search, "the picker has a search box over a tree");
    if(!Tree || !Search) { return; }

    /* Browsing: folders by category, Rendering/Lights nested, the entity's own types greyed. */
    QStringList Folders;
    for(int Index = 0; Index < Tree->topLevelItemCount(); ++Index) { Folders.push_back(Tree->topLevelItem(Index)->text(0)); }
    Expect(Folders.contains(QStringLiteral("Rendering")) && Folders.contains(QStringLiteral("Physics"))
           && Folders.contains(QStringLiteral("Core")), "types are filed in category folders");
    Expect(!Folders.contains(QStringLiteral("Other")) || Folders.last() == QStringLiteral("Other"), "Other is the last folder");
    QStringList SortedFolders = Folders;
    SortedFolders.removeAll(QStringLiteral("Other"));
    QStringList Expected = SortedFolders;
    std::sort(Expected.begin(), Expected.end(), [](const QString& A, const QString& B) { return A.compare(B, Qt::CaseInsensitive) < 0; });
    Expect(SortedFolders == Expected, "folders are alphabetical");

    QStringList Enabled, Disabled;
    CollectTree(*Tree, Enabled, Disabled);
    Expect(Disabled.contains(QStringLiteral("Transform")), "a type the entity has is listed greyed out");
    Expect(Enabled.contains(QStringLiteral("Point Light")) || Enabled.contains(QStringLiteral("PointLight")), "an addable type is choosable");
    std::printf("  %d folders, %d choosable, %d present\n", int(Folders.size()), int(Enabled.size()), int(Disabled.size()));

    /* Searching: every word must match; the first match is current, Enter adds it. */
    Search->setText(QStringLiteral("light"));
    PumpFor(20);
    Enabled.clear(); Disabled.clear();
    CollectTree(*Tree, Enabled, Disabled);
    bool bAllLights = !Enabled.isEmpty();
    for(const QString& Each : Enabled) { bAllLights = bAllLights && Each.contains(QStringLiteral("Light"), Qt::CaseInsensitive); }
    Expect(bAllLights && Enabled.size() >= 4, "\"light\" lists the lights");
    Expect(Tree->currentItem() && Tree->currentItem()->text(0) == Enabled.first(), "the first match is current");

    Search->setText(QStringLiteral("nav ag"));
    PumpFor(20);
    Enabled.clear(); Disabled.clear();
    CollectTree(*Tree, Enabled, Disabled);
    Expect(Enabled.size() == 1, "\"nav ag\" narrows to one type");

    Search->setText(QStringLiteral("zzzz"));
    PumpFor(20);
    Expect(Tree->topLevelItemCount() == 1 && !(Tree->topLevelItem(0)->flags() & Qt::ItemIsEnabled), "no match says so");

    Search->setText(QStringLiteral("point light"));
    PumpFor(20);
    const QString PickedName = Tree->currentItem() ? Tree->currentItem()->text(0) : QString();
    QKeyEvent Enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(Search, &Enter);
    PumpFor(100);
    bool bAdded = false;
    const size_t TypeCount = GameCore::GetComponentTypeCount();
    for(size_t Index = 0; Index < TypeCount; ++Index)
    {
        const GameCore::ComponentTypeInfo* Info = GameCore::GetComponentType(GameCore::ComponentTypeID(Index));
        if(Info && PickedName == QString::fromLatin1(Info->Name))
        {
            bAdded = Doc.GetWorld().GetEntity(Thing).HasComponentByType(GameCore::ComponentTypeID(Index));
        }
    }
    Expect(!PickedName.isEmpty() && bAdded, "Enter adds the current type to the entity");

    /* Reopened, the added type is greyed out. */
    PumpFor(200);
    Picker = OpenPicker(Panel);
    Expect(Picker != nullptr, "the picker opens again after an add");
    if(!Picker) { return; }
    Tree = Picker->findChild<QTreeWidget*>();
    Search = Picker->findChild<QLineEdit*>();
    Enabled.clear(); Disabled.clear();
    CollectTree(*Tree, Enabled, Disabled);
    Expect(Disabled.contains(PickedName), "the added type is greyed out next time");

    const QByteArray Snapshot = qgetenv("NIMBLE_PICKER_SNAPSHOT");
    if(!Snapshot.isEmpty())
    {
        Search->setText(qEnvironmentVariable("NIMBLE_PICKER_FILTER"));
        PumpFor(50);
        Expect(Picker->grab().save(QString::fromLocal8Bit(Snapshot)), "the picker snapshot is written");
    }
    Picker->close();
    PumpFor(20);
}

int main(int argc, char** argv)
{
    /* No display needed. */
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication App(argc, argv);

    std::printf("NimbleEditorDetailsTests\n");
    TestMaterialParameters();
    TestDetailModeRow();
    TestTextureViewer();
    TestAnimStateGraph();
    TestClipTimeline();
    TestViewSettings();
    TestDocumentation();
    TestProjectBuildOptions();
    TestGraphSnapshot();
    TestAddComponentPicker();

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
