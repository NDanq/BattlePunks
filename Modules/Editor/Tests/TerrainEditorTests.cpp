/*
 * NimbleEditorTerrainTests -- Terrain mode, offscreen: making a terrain for a
 * level, strokes as undo steps (and cancelled ones), saving edited terrain
 * with the level and dropping unsaved edits, heightmap export and import,
 * the "terrain" console command, and the dock driving the viewport's tool.
 *
 * The test level lives under the project's Saved/ folder (never committed),
 * since terrain files go beside their level inside the project; it is
 * removed at the end.
 */

#include <cmath>
#include <cstdio>
#include <string>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QUndoStack>

#include "Public/Editor/EditorConsole.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/Components/RigidBodyComponent.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Terrain.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/TerrainComponent.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Terrain/TerrainEditing.h"
#include "Private/Editor/Terrain/TerrainToolsWindow.h"

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const std::string& What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What.c_str());
    }
}

namespace
{
    bool Near(const double InA, const double InB, const double InTolerance = 1e-3) { return std::fabs(InA - InB) <= InTolerance; }

    std::string Num(const double InValue)
    {
        char Buffer[64];
        std::snprintf(Buffer, sizeof(Buffer), "%.3f", InValue);
        return Buffer;
    }

    void Pump()
    {
        for(int Each = 0; Each < 3; ++Each) { QApplication::processEvents(); }
    }

    float CentreHeight(const EditorTerrain::Target& InTarget)
    {
        const uint32_t Mid = InTarget.Data->GetResolution() / 2;
        return InTarget.Data->GetHeight(int32_t(Mid), int32_t(Mid));
    }

    /* A second of Raise at full strength in the middle of InTarget, as one stroke. */
    void Raise(const EditorTerrain::Target& InTarget, const QString& InLabel, const bool bInCancel = false)
    {
        EditorTerrain::Stroke Stroke(InTarget, InLabel);
        GameCore::TerrainBrush Brush;
        Brush.Strength = 1.0f;
        GameCore::TerrainBrushOp Op;
        Op.Tool         = GameCore::TT_Raise;
        Op.Rate         = 300.0f;
        Op.DeltaSeconds = 0.1f;
        for(int Dab = 0; Dab < 10; ++Dab) { Stroke.Dab(EngineCore::Vector3(0.0, 0.0, 0.0), 500.0f, Brush, Op); }
        Stroke.End(bInCancel);
    }
}

int main(int argc, char** argv)
{
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication App(argc, argv);
    std::printf("NimbleEditorTerrainTests\n");

    const QString Folder = QDir(EditorGraphs::GetProjectRoot()).filePath(QStringLiteral("Saved/TerrainEditorTests"));
    QDir(Folder).removeRecursively();
    QDir().mkpath(Folder);
    const QString WorldFile = QDir(Folder).filePath(QStringLiteral("Hills.nmworld"));

    EditorContext   Context;
    EditorDocument& Level = Context.GetLevel();
    QString         CurrentPath;

    EditorTerrain::Host Host;
    Host.Context      = &Context;
    Host.Document     = &Level;
    Host.GetWorldFile = [&]() { return CurrentPath; };
    EditorTerrain::SetHost(Host);

    // --- Making one -----------------------------------------------------------
    std::printf("Create\n");
    QString Error;
    EditorTerrain::CreateSettings Settings;
    Settings.Name       = QStringLiteral("Ground");
    Settings.Resolution = 129;
    Settings.Spacing    = 50.0f;
    Expect(!EditorTerrain::CreateTerrain(Settings, EngineCore::Vector3(0.0, 0.0, 0.0), Error).IsSet() && !Error.isEmpty(),
           "an unsaved level cannot have a terrain yet: " + Error.toStdString());

    CurrentPath = WorldFile;
    const int StepsBefore = Level.GetHistory().count();
    const GameCore::EntityID Ground = EditorTerrain::CreateTerrain(Settings, EngineCore::Vector3(0.0, 0.0, 0.0), Error);
    Expect(Ground.IsSet(), "CreateTerrain makes an entity: " + Error.toStdString());
    GameCore::World& World = Level.GetEditedWorld();
    const GameCore::Entity E = World.GetEntity(Ground);
    const auto* Terrain = E.GetComponent<RenderCore::TerrainComponent>();
    const auto* Body    = E.GetComponent<GameCore::RigidBodyComponent>();
    Expect(Terrain && std::string(Terrain->Terrain) == "Saved/TerrainEditorTests/Hills/Terrain/Ground.nmterrain",
           std::string("its Terrain names the level's Terrain folder: ") + (Terrain ? Terrain->Terrain : "(none)"));
    Expect(Body && Body->Motion == GameCore::RBM_Static && Body->Shape == GameCore::RBS_HeightField, "and a Static heightfield Rigid Body");
    Expect(E.GetMobility() == GameCore::EMobility::Static, "the entity is Static");
    Expect(Near(E.GetWorldPose().Position.GetX(), -3200.0) && Near(E.GetWorldPose().Position.GetZ(), -3200.0),
           "centred on the origin: its corner at -3200");
    Expect(QFileInfo::exists(QDir(Folder).filePath(QStringLiteral("Hills/Terrain/Ground.nmterrain"))), "its file is written at once");
    Expect(Level.GetHistory().count() == StepsBefore + 1 && Level.GetSelection() == Ground, "one undo step, and selected");
    Expect(!EditorTerrain::CreateTerrain(Settings, EngineCore::Vector3(0.0, 0.0, 0.0), Error).IsSet(), "the name is taken now");

    EditorTerrain::Target Target = EditorTerrain::FindEditTarget();
    Expect(Target.IsValid() && Target.Entity == Ground && Target.Data->GetResolution() == 129, "FindEditTarget: the selection's terrain");
    Level.SetSelection(GameCore::EntityID());
    Expect(EditorTerrain::FindEditTarget().Entity == Ground, "or the level's only terrain");
    Level.SetSelection(Ground);

    // --- Strokes --------------------------------------------------------------
    std::printf("Strokes\n");
    const int StrokeBase = Level.GetHistory().count();
    Raise(Target, QStringLiteral("Sculpt Raise"));
    const float Raised = CentreHeight(Target);
    Expect(Near(Raised, 300.0, 0.5), "a second of Raise at full strength lifts the centre 300: " + Num(Raised));
    Expect(Level.GetHistory().count() == StrokeBase + 1 && Level.GetHistory().undoText() == QStringLiteral("Sculpt Raise Ground"),
           "the stroke is one step: " + Level.GetHistory().undoText().toStdString());
    Expect(EditorTerrain::IsDirty(Target.Path), "and the terrain is unsaved");

    Level.Undo();
    Expect(CentreHeight(Target) == 0.0f, "undo puts every sample back exactly");
    Level.Redo();
    Expect(CentreHeight(Target) == Raised, "redo brings the stroke back");

    Raise(Target, QStringLiteral("Sculpt Raise"), true);
    Expect(CentreHeight(Target) == Raised && Level.GetHistory().count() == StrokeBase + 1, "a cancelled stroke leaves nothing, and no step");

    // --- Saving and discarding -----------------------------------------------
    std::printf("Save\n");
    QObject::connect(&Level, &EditorDocument::AboutToSaveWorld, [&]() { EditorTerrain::SaveDirty(); });
    Expect(Level.SaveWorld(WorldFile), "the level saves");
    Expect(!EditorTerrain::IsDirty(Target.Path), "and its terrain with it");
    GameCore::TerrainData FromDisk;
    Expect(GameCore::LoadTerrainFile(EditorTerrain::ProjectFileFor(Target.Path).toStdString(), FromDisk)
           && Near(FromDisk.GetHeight(64, 64), Raised, 0.05), "the file holds the stroke: " + Num(FromDisk.GetHeight(64, 64)));

    Raise(Target, QStringLiteral("Sculpt Raise"));
    Expect(CentreHeight(Target) > Raised + 100.0f && EditorTerrain::IsDirty(Target.Path), "another stroke, unsaved");
    EditorTerrain::DiscardUnsaved();
    const std::shared_ptr<GameCore::TerrainData> Reloaded = GameCore::TerrainAssetCache::Find(Target.Path);
    Expect(Reloaded && Reloaded != Target.Data && Near(Reloaded->GetHeight(64, 64), Raised, 0.05),
           "discarding drops it: the file's version loads again");
    Target = EditorTerrain::FindEditTarget();

    // --- Heightmaps ----------------------------------------------------------
    std::printf("Heightmaps\n");
    const QString Png = QDir(Folder).filePath(QStringLiteral("Ground.png"));
    Expect(EditorTerrain::ExportHeightmap(Target, Png, Error) && QFileInfo::exists(Png), "export writes a PNG");
    const float Low = Target.Data->GetMinHeight(), High = Target.Data->GetMaxHeight();
    const int ImportBase = Level.GetHistory().count();
    Expect(EditorTerrain::ImportHeightmap(Target, Png, Low, High, Error), "and it imports back: " + Error.toStdString());
    Expect(Near(CentreHeight(Target), Raised, 0.05) && Near(Target.Data->GetHeight(0, 0), 0.0, 0.05),
           "the round trip keeps the heights (" + Num(CentreHeight(Target)) + ")");
    Expect(Level.GetHistory().count() == ImportBase + 1, "an import is one step");
    Expect(EditorTerrain::ImportHeightmap(Target, Png, 0.0f, 1000.0f, Error) && Near(CentreHeight(Target), 1000.0, 1.0),
           "Black/White scale it: white is 1000");
    Level.Undo();
    Expect(Near(CentreHeight(Target), Raised, 0.05), "and undo puts it back");

    // --- The console ---------------------------------------------------------
    std::printf("Console\n");
    EditorConsole Console;
    Console.Register(QStringLiteral("terrain"), EditorTerrain::GetConsoleUsage(), QString(),
                     [&](const QStringList& InArgs) { EditorTerrain::RunConsoleCommand(Console, InArgs); });
    const int ConsoleBase = Level.GetHistory().index();   // after an undo: the new step replaces the redo
    Expect(Console.Execute(QStringLiteral("terrain sculpt Lower 1000 1000 400 1 0.5")), "terrain sculpt runs");
    Expect(Level.GetHistory().index() == ConsoleBase + 1 && Level.GetHistory().undoText() == QStringLiteral("Sculpt Lower Ground"),
           "as one step: " + Level.GetHistory().undoText().toStdString());
    const EngineCore::Vector3 At = Target.ToLocal(EngineCore::Vector3(1000.0, 0.0, 1000.0));
    Expect(Target.Data->SampleHeight(float(At.GetX()), float(At.GetZ())) < -100.0f, "the ground there went down");
    Expect(Console.Execute(QStringLiteral("terrain paint 2 -1000 -1000 400 1 1")), "terrain paint runs");
    const EngineCore::Vector3 Painted = Target.ToLocal(EngineCore::Vector3(-1000.0, 0.0, -1000.0));
    float Weights[GameCore::TERRAIN_LAYERS];
    Target.Data->SampleWeights(float(Painted.GetX()), float(Painted.GetZ()), Weights);
    Expect(Weights[2] > 0.9f, "the paint landed on layer 2: " + Num(Weights[2]));
    Expect(Level.GetHistory().undoText() == QStringLiteral("Paint Rock Ground"), "named for its layer: " + Level.GetHistory().undoText().toStdString());

    // --- The dock -----------------------------------------------------------
    std::printf("Dock\n");
    {
        QMainWindow Window;
        TerrainToolsWindow& Dock = TerrainToolsWindow::Install(Window, nullptr);
        Window.show();
        TerrainToolsWindow::Show();
        Pump();
        Expect(Dock.isVisible(), "Show shows the dock");
        const QLabel* Label = Dock.findChild<QLabel*>();
        Expect(Label && Label->text().contains(QStringLiteral("Ground")), "it names the terrain it edits");
        Expect(TerrainToolsWindow::SelectTool(QStringLiteral("Lower")) && Dock.GetTool().GetSettings().bLower
               && Dock.GetTool().GetSettings().Tool == GameCore::TT_Raise, "SelectTool Lower: Raise, inverted");
        Expect(TerrainToolsWindow::SelectTool(QStringLiteral("Paint")) && Dock.GetTool().GetSettings().Tool == GameCore::TT_Paint,
               "SelectTool Paint");
        Expect(!TerrainToolsWindow::SelectTool(QStringLiteral("Bulldoze")), "an unknown tool is refused");
    }

    EditorTerrain::DiscardUnsaved();
    GameCore::TerrainAssetCache::Invalidate("");
    QDir(Folder).removeRecursively();

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
