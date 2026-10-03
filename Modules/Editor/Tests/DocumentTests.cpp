/*
 * NimbleEditorDocumentTests -- EditorDocument without a Renderer or a window:
 * entity edits and their undo/redo, World replacement, and a borrowed World
 * (what Play-in-Editor lends the level).
 */

#include <cstdio>

#include <QCoreApplication>
#include <QUndoStack>

#include "Public/Core/CoreMemory.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

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

/* EntitiesChanged and a destroyed selection's signal wait for the next turn. */
static void Pump()
{
    QCoreApplication::processEvents();
}

static void TestCreateUndoRedo()
{
    std::printf("Create, undo, redo\n");
    EditorDocument Doc;

    int EntitiesChanged = 0;
    QObject::connect(&Doc, &EditorDocument::EntitiesChanged, [&EntitiesChanged]() { ++EntitiesChanged; });

    Expect(!Doc.CanUndo(), "a new document has nothing to undo");
    Expect(Doc.GetHistory().isClean(), "a new document is clean");

    const EntityID Made = Doc.CreateEntity(QStringLiteral("Box"), EngineCore::Vector3(1.0, 2.0, 3.0));
    Expect(Doc.GetWorld().IsAlive(Made), "CreateEntity makes a live entity");
    Expect(Doc.GetDisplayName(Made) == QStringLiteral("Box"), "the name is its display name");
    const GameCore::TransformComponent* Transform = Doc.GetWorld().GetEntity(Made).GetComponent<GameCore::TransformComponent>();
    Expect(Transform && Transform->Position.GetY() == 2.0, "with a Transform at the position");
    Expect(Doc.CanUndo(), "creating is recorded");
    Expect(!Doc.GetHistory().isClean(), "and dirties the document");

    Pump();
    Expect(EntitiesChanged == 1, "EntitiesChanged once, on the next turn");

    const GameCore::EntityGuid Guid = Doc.GetWorld().GetEntity(Made).GetGuid();
    Doc.Undo();
    Expect(Doc.GetWorld().GetEntityCount() == 0, "undo removes it");
    Expect(Doc.CanRedo(), "and can be redone");

    Doc.Redo();
    const EntityID Again = Doc.GetWorld().FindEntityByGuid(Guid);
    Expect(Doc.GetWorld().IsAlive(Again), "redo brings it back with its GUID");
    Expect(Doc.GetDisplayName(Again) == QStringLiteral("Box"), "and its name");

    Doc.RenameEntity(Again, QStringLiteral("Crate"));
    Expect(Doc.GetDisplayName(Again) == QStringLiteral("Crate"), "rename applies");
    Doc.Undo();
    Expect(Doc.GetDisplayName(Doc.GetWorld().FindEntityByGuid(Guid)) == QStringLiteral("Box"), "undo restores the name");
}

static void TestSelection()
{
    std::printf("Selection\n");
    EditorDocument Doc;

    GameCore::EntityID LastSignalled;
    int Signals = 0;
    QObject::connect(&Doc, &EditorDocument::SelectionChanged, [&](const EntityID InEntity) { LastSignalled = InEntity; ++Signals; });

    const EntityID A = Doc.CreateEntity(QStringLiteral("A"), EngineCore::Vector3());
    Doc.SetSelection(A);
    Expect(Doc.GetSelection() == A, "SetSelection selects");
    Expect(Signals == 1 && LastSignalled == A, "and signals once");
    Doc.SetSelection(A);
    Expect(Signals == 1, "selecting it again does not signal");

    Doc.DestroyEntity(A);
    Pump();
    Expect(!Doc.GetSelection().IsSet(), "deleting the selection clears it");
    Expect(!LastSignalled.IsSet(), "and says so");
}

static void TestNewScene()
{
    std::printf("New scene\n");
    EditorDocument Doc;

    int AboutToGo = 0;
    int Created   = 0;
    int Resets    = 0;
    GameCore::World* Before = &Doc.GetEditedWorld();
    bool bOldWorldStillThere = false;
    QObject::connect(&Doc, &EditorDocument::EditedWorldAboutToBeDestroyed, [&]()
    {
        ++AboutToGo;
        bOldWorldStillThere = &Doc.GetEditedWorld() == Before;   // a Renderer unbinds it here
    });
    QObject::connect(&Doc, &EditorDocument::EditedWorldCreated, [&]() { ++Created; });
    QObject::connect(&Doc, &EditorDocument::WorldReset, [&]() { ++Resets; });

    Doc.SetSelection(Doc.CreateEntity(QStringLiteral("Gone"), EngineCore::Vector3()));
    Doc.NewScene();

    Expect(AboutToGo == 1 && bOldWorldStillThere, "EditedWorldAboutToBeDestroyed while the old World is alive");
    Expect(Created == 1, "EditedWorldCreated for the new one");
    Expect(Resets == 1, "WorldReset once");
    Expect(Doc.GetWorld().GetEntityCount() == 0, "the new World is empty");
    Expect(!Doc.GetSelection().IsSet(), "nothing is selected");
    Expect(!Doc.CanUndo() && Doc.GetHistory().isClean(), "the history starts again");
}

static void TestBorrowedWorld()
{
    std::printf("Borrowed World\n");
    EditorDocument Doc;

    const EntityID Kept = Doc.CreateEntity(QStringLiteral("Kept"), EngineCore::Vector3());
    Doc.SetSelection(Kept);
    GameCore::World* Edited = &Doc.GetEditedWorld();

    int HistorySignals = 0;
    QObject::connect(&Doc, &EditorDocument::HistoryChanged, [&]() { ++HistorySignals; });

    /* Owned here, as PlaySession owns the game's. */
    GameCore::World* Game = EngineCore::Memory::Alloc<GameCore::World>();
    Doc.BorrowWorld(Game);

    Expect(Doc.IsWorldBorrowed(), "BorrowWorld borrows");
    Expect(&Doc.GetWorld() == Game, "GetWorld is the borrowed World");
    Expect(&Doc.GetEditedWorld() == Edited, "GetEditedWorld is still the edited one");
    Expect(!Doc.GetSelection().IsSet(), "with nothing selected");
    Expect(!Doc.CanUndo(), "undo is refused");
    Expect(HistorySignals >= 1, "and HistoryChanged says so");

    const EntityID Spawned = Doc.CreateEntity(QStringLiteral("Spawned"), EngineCore::Vector3());
    Expect(Game->IsAlive(Spawned), "entity operations reach the borrowed World");
    Expect(Doc.GetHistory().count() == 1, "but are not recorded");

    Doc.NewScene();
    Expect(&Doc.GetEditedWorld() == Edited && Edited->IsAlive(Kept), "scene operations are refused");

    /* A level swap: unhook, the owner frees it, a new one is lent. */
    Doc.UnhookBorrowedWorld();
    Doc.DropBorrowedWorld();
    EngineCore::Memory::Dealloc(Game);
    Game = EngineCore::Memory::Alloc<GameCore::World>();
    Doc.BorrowWorld(Game);
    Expect(&Doc.GetWorld() == Game, "a replacement World can be lent");
    Pump();

    /* Stop: unhook, drop, the owner frees it, then return. */
    Doc.UnhookBorrowedWorld();
    Doc.DropBorrowedWorld();
    EngineCore::Memory::Dealloc(Game);
    Doc.ReturnToEditedWorld();
    Pump();

    Expect(!Doc.IsWorldBorrowed(), "ReturnToEditedWorld returns");
    Expect(&Doc.GetWorld() == Edited, "to the edited World");
    Expect(Doc.GetSelection() == Kept, "with its selection back");
    Expect(Doc.CanUndo(), "and undo allowed again");

    /* The edited World's hooks are live again: a delete clears the selection. */
    Doc.DestroyEntity(Kept);
    Pump();
    Expect(!Doc.GetSelection().IsSet(), "the edited World is hooked again");
}

static void TestDestroyWhileBorrowed()
{
    std::printf("Destroyed while borrowing\n");
    GameCore::World* Game = EngineCore::Memory::Alloc<GameCore::World>();
    {
        EditorDocument Doc;
        Doc.BorrowWorld(Game);
        Doc.CreateEntity(QStringLiteral("Spawned"), EngineCore::Vector3());
    }
    /* The document let go without freeing it, and without leaving its hooks. */
    Expect(Game->GetEntityCount() == 1, "a borrowed World outlives the document");
    Game->CreateEntity("After");
    Pump();
    EngineCore::Memory::Dealloc(Game);
    Expect(true, "and fires no stale delegate");
}

int main(int argc, char* argv[])
{
    QCoreApplication App(argc, argv);
    std::printf("Editor document tests\n");

    TestCreateUndoRedo();
    TestSelection();
    TestNewScene();
    TestBorrowedWorld();
    TestDestroyWhileBorrowed();

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
