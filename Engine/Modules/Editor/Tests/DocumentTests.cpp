/*
 * NimbleEditorDocumentTests -- EditorDocument without a Renderer or a window:
 * entity edits and their undo/redo, World replacement, and a borrowed World
 * (what Play-in-Editor lends the level).
 */

#include <cstdio>
#include <string>

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QUndoStack>

#include "Public/Core/CoreMemory.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldPackage.h"
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

/* Several entities selected: the primary, toggling and adding, the roots. */
static void TestMultiSelection()
{
    std::printf("Multiple selection\n");
    EditorDocument Doc;
    GameCore::World& W = Doc.GetWorld();
    const EntityID A = Doc.CreateEntity(QStringLiteral("A"), EngineCore::Vector3());
    const EntityID B = Doc.CreateEntity(QStringLiteral("B"), EngineCore::Vector3());
    const EntityID C = Doc.CreateEntity(QStringLiteral("C"), EngineCore::Vector3());
    W.GetEntity(C).SetParent(A);

    int Signals = 0;
    QObject::connect(&Doc, &EditorDocument::SelectionChanged, [&Signals](EntityID) { ++Signals; });

    Doc.SetSelection(A);
    Doc.AddToSelection(B);
    Expect(Doc.GetSelectedEntities().size() == 2 && Doc.GetSelection() == B, "Shift adds, the added entity the primary");
    Expect(Doc.IsSelected(A) && Doc.IsSelected(B) && !Doc.IsSelected(C), "IsSelected says which");
    Doc.ToggleSelected(B);
    Expect(Doc.GetSelectedEntities().size() == 1 && Doc.GetSelection() == A, "Ctrl on a selected entity drops it; the last left is primary");
    Doc.ToggleSelected(C);
    Expect(Doc.GetSelection() == C && Doc.GetSelectedEntities().size() == 2, "Ctrl on another adds it");
    const std::vector<EntityID> Roots = Doc.GetSelectedRoots();
    Expect(Roots.size() == 1 && Roots.front() == A, "a child goes with its selected parent: not a root of the selection");

    const int Before = Signals;
    Doc.SetSelection({ A, C }, C);
    Expect(Signals == Before, "the same set and primary signals nothing");
    Doc.SetSelection({ A, B }, EntityID());
    Expect(Doc.GetSelection() == B, "no primary given: the last");

    Doc.DestroyEntity(B);
    Expect(Doc.GetSelectedEntities().size() == 1 && Doc.GetSelection() == A, "deleting a selected entity leaves the rest selected");
    Doc.SetSelection(EntityID());
    Expect(Doc.GetSelectedEntities().empty() && !Doc.GetSelection().IsSet(), "an unset id clears it all");
}

/* Cut, copy, paste and duplicate: new entities, one undo step each. */
static void TestClipboard()
{
    std::printf("Cut, copy, paste, duplicate\n");
    EditorDocument Doc;
    GameCore::World& W = Doc.GetWorld();
    const EntityID Crate = Doc.CreateEntity(QStringLiteral("Crate"), EngineCore::Vector3(100.0, 0.0, 0.0));
    const EntityID Lid   = Doc.CreateEntity(QStringLiteral("Lid"),   EngineCore::Vector3(100.0, 50.0, 0.0));
    const EntityID Post  = Doc.CreateEntity(QStringLiteral("Post"),  EngineCore::Vector3(-200.0, 0.0, 0.0));
    W.GetEntity(Lid).SetParent(Crate);
    const GameCore::EntityGuid CrateGuid = W.GetEntity(Crate).GetGuid();

    Doc.SetSelection({ Crate, Post }, Post);
    Expect(Doc.CopySelection() == 2 && Doc.CanPaste(), "Copy takes the selection's two subtrees");
    const size_t Count = W.GetEntityCount();
    const int UndoBefore = Doc.GetHistory().count();

    const std::vector<EntityID> Pasted = Doc.Paste();
    Expect(Pasted.size() == 2 && W.GetEntityCount() == Count + 3, "Paste makes the two subtrees again, the child too");
    Expect(Doc.GetHistory().count() == UndoBefore + 1, "as one undo step");
    const GameCore::Entity Copy = W.GetEntity(Pasted.front());
    Expect(Copy.GetGuid() != CrateGuid && std::string(Copy.GetName()) == "Crate", "a new entity, with the name");
    Expect(Copy.GetWorldPose().Position.GetX() == 100.0, "where the original stood");
    Expect(Copy.GetFirstChild().IsSet() && std::string(W.GetEntity(Copy.GetFirstChild()).GetName()) == "Lid"
           && Copy.GetFirstChild() != Lid, "with a copy of its child, not the original");
    Expect(Doc.GetSelectedEntities().size() == 2 && Doc.IsSelected(Pasted.front()), "the copies are selected");

    Doc.Undo();
    Expect(W.GetEntityCount() == Count, "undo removes every copy");
    Doc.Redo();
    Expect(W.GetEntityCount() == Count + 3, "redo brings them back");

    const std::vector<EntityID> Again = Doc.Paste();
    Expect(Again.size() == 2 && W.GetEntity(Again.front()).GetGuid() != W.GetEntity(Pasted.front()).GetGuid(),
           "pasting again makes more, with GUIDs of their own");

    /* Cut: copied, then gone; it pastes back. */
    const size_t BeforeCut = W.GetEntityCount();
    Doc.SetSelection(Post);
    Expect(Doc.CutSelection() == 1 && !W.IsAlive(Post) && W.GetEntityCount() == BeforeCut - 1, "Cut removes the selection");
    Expect(Doc.GetSelectedEntities().empty(), "and leaves nothing selected");
    const std::vector<EntityID> Back = Doc.Paste();
    Expect(Back.size() == 1 && std::string(W.GetEntity(Back.front()).GetName()) == "Post", "and pastes back");
    Doc.Undo();
    Doc.Undo();
    Expect(W.GetEntityCount() == BeforeCut && W.FindEntityByName("Post").IsSet(), "undoing the paste and the cut puts it back");

    /* Duplicate: copies the selection, leaves the clipboard. */
    Doc.SetSelection(Crate);
    const size_t BeforeDuplicate = W.GetEntityCount();
    const std::vector<EntityID> Twins = Doc.DuplicateSelection();
    Expect(Twins.size() == 1 && W.GetEntityCount() == BeforeDuplicate + 2 && Doc.GetSelection() == Twins.front(),
           "Duplicate copies the selection's subtree and selects it");
    const std::vector<EntityID> Still = Doc.Paste();
    Expect(Still.size() == 1 && std::string(W.GetEntity(Still.front()).GetName()) == "Post", "the clipboard still holds the cut");

    /* Delete: the selection's subtrees, one step. */
    Doc.SetSelection({ Twins.front(), Still.front() }, Still.front());
    const size_t BeforeDelete = W.GetEntityCount();
    Expect(Doc.DeleteSelection() == 2 && W.GetEntityCount() == BeforeDelete - 3, "Delete removes every selected subtree");
    Doc.Undo();
    Expect(W.GetEntityCount() == BeforeDelete, "and one undo brings them all back");
}

/* A copied prefab instance comes back as a new instance. */
static void TestCopyInstance()
{
    std::printf("Copy a prefab instance\n");
    QTemporaryDir Temp;
    const QString Path = Temp.filePath(QStringLiteral("Lamp.nmprefab"));
    {
        GameCore::World Scratch;
        const GameCore::Entity Root = Scratch.CreateEntity("Root");
        Root.AddComponent<GameCore::TransformComponent>();
        const GameCore::Entity Bulb = Scratch.CreateEntity("Bulb");
        Bulb.AddComponent<GameCore::TransformComponent>();
        Bulb.SetParent(Root, false);
        GameCore::PrefabAsset Asset;
        GameCore::CapturePrefab(Scratch, Root.GetID(), Asset);
        GameCore::SavePrefabFile(Path.toUtf8().constData(), Asset);
    }
    EditorDocument Doc;
    GameCore::World& W = Doc.GetEditedWorld();
    const EntityID Root = Doc.PlacePrefab(Path, EngineCore::Vector3(10.0, 0.0, 0.0));
    Doc.SetSelection(Root);
    const std::vector<EntityID> Twins = Doc.DuplicateSelection();
    Expect(Twins.size() == 1 && W.GetInstanceCount() == 2, "duplicating an instance root makes a second instance");
    const GameCore::PrefabInstance Twin = W.GetInstance(W.GetEntity(Twins.front()).GetInstance());
    Expect(Twin.IsValid() && Twin.GetRoot().GetID() == Twins.front(), "rooted at the copy");
    const GameCore::Entity Bulb = Twin.FindEntity("Bulb");
    Expect(Bulb.IsAlive() && Bulb.GetGuid() == GameCore::DerivePrefabEntityGuid(Twin.GetGuid(), Bulb.GetPrefabLocalId()),
           "its members' GUIDs derived from the new instance's, as a load makes them");
    Doc.Undo();
    Expect(W.GetInstanceCount() == 1, "undo removes the copy's instance");

    /* A member copied alone is loose. */
    Doc.SetSelection(W.GetInstance(W.GetEntity(Root).GetInstance()).FindEntity("Bulb").GetID());
    const std::vector<EntityID> Loose = Doc.DuplicateSelection();
    Expect(Loose.size() == 1 && !W.GetEntity(Loose.front()).IsInInstance() && !W.GetEntity(Loose.front()).GetParent().IsSet(),
           "a member duplicated alone comes out loose, at the top");
}

/* Dragging several entities with the gizmo: one step for all of them. */
static void TestGroupEdit()
{
    std::printf("Group edit\n");
    EditorDocument Doc;
    GameCore::World& W = Doc.GetWorld();
    const EntityID A = Doc.CreateEntity(QStringLiteral("A"), EngineCore::Vector3(0.0, 0.0, 0.0));
    const EntityID B = Doc.CreateEntity(QStringLiteral("B"), EngineCore::Vector3(50.0, 0.0, 0.0));
    Doc.SetSelection({ A, B }, B);
    const int Steps = Doc.GetHistory().count();
    const GameCore::ComponentTypeID Transform = GameCore::TransformComponent::StaticType().ID;
    Doc.BeginGroupEdit({ A, B }, Transform, QStringLiteral("Move"));
    for(int Frame = 1; Frame <= 3; ++Frame)
    {
        for(const EntityID Each : { A, B })
        {
            GameCore::TransformComponent* Moved = W.GetEntity(Each).GetComponent<GameCore::TransformComponent>();
            Moved->Position = Moved->Position + EngineCore::Vector3(0.0, 10.0, 0.0);
            Doc.NotifyPropertyEdited(Each, Transform);
        }
    }
    Doc.EndGroupEdit(false);
    Expect(Doc.GetHistory().count() == Steps + 1, "a group drag is one undo step");
    Doc.Undo();
    Expect(W.GetEntity(A).GetComponent<GameCore::TransformComponent>()->Position.GetY() == 0.0
           && W.GetEntity(B).GetComponent<GameCore::TransformComponent>()->Position.GetY() == 0.0, "undo moves every one back");

    Doc.BeginGroupEdit({ A, B }, Transform, QStringLiteral("Move"));
    W.GetEntity(A).GetComponent<GameCore::TransformComponent>()->Position = EngineCore::Vector3(999.0, 0.0, 0.0);
    Doc.EndGroupEdit(true);
    Expect(W.GetEntity(A).GetComponent<GameCore::TransformComponent>()->Position.GetX() == 0.0, "a cancelled group edit puts it back");
}

/* GetEntityBounds: a parent -- a prefab instance's root -- encloses its children. */
static void TestHierarchyBounds()
{
    std::printf("Hierarchy bounds\n");
    EditorDocument Doc;
    GameCore::World& World = Doc.GetWorld();

    /* A root with only a Transform, and two children drawing the unit quad
     * (no mesh): 100 across at x = 500 and, one level deeper, at x = -300. */
    const EntityID Root  = Doc.CreateEntity(QStringLiteral("Root"), EngineCore::Vector3(0.0, 0.0, 0.0));
    const EntityID Near  = Doc.CreateEntity(QStringLiteral("Near"), EngineCore::Vector3(500.0, 0.0, 0.0));
    const EntityID Far   = Doc.CreateEntity(QStringLiteral("Far"),  EngineCore::Vector3(-300.0, 200.0, 0.0));
    const EntityID Alone = Doc.CreateEntity(QStringLiteral("Alone"), EngineCore::Vector3(0.0, 1000.0, 0.0));
    for(const EntityID Each : { Near, Far })
    {
        World.GetEntity(Each).GetComponent<GameCore::TransformComponent>()->Scale = EngineCore::Vector3(100.0, 100.0, 1.0);
        World.GetEntity(Each).AddComponent(RenderCore::RenderComponent());
    }
    World.GetEntity(Near).SetParent(Root);
    World.GetEntity(Far).SetParent(Near);

    const GameCore::AABB Box = Doc.GetEntityBounds(Root);
    Expect(!Box.IsEmpty() && Box.Min.GetX() <= -350.0 + 1e-6 && Box.Max.GetX() >= 550.0 - 1e-6
           && Box.Max.GetY() >= 250.0 - 1e-6, "a root's bounds enclose every descendant's");
    Expect(Box.Max.GetX() <= 550.0 + 1e-6 && Box.Min.GetY() >= -50.0 - 1e-6,
           "and no more: no marker cube for a root whose children have shapes");
    const GameCore::AABB Middle = Doc.GetEntityBounds(Near);
    Expect(Middle.Min.GetX() <= -350.0 + 1e-6 && Middle.Max.GetX() >= 550.0 - 1e-6, "a child's include its own children");
    const GameCore::AABB Marker = Doc.GetEntityBounds(Alone);
    Expect(!Marker.IsEmpty() && Marker.Max.GetX() - Marker.Min.GetX() == 50.0, "an empty without children is still a small cube");
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


static void TestPrefab()
{
    std::printf("Prefab\n");
    QTemporaryDir Temp;
    const QString Path = Temp.filePath(QStringLiteral("Door.nmprefab"));

    /* Root with one child, written as the editor's text form. */
    {
        GameCore::World Scratch;
        const GameCore::Entity Root = Scratch.CreateEntity("Root");
        Root.AddComponent<GameCore::TransformComponent>();
        const GameCore::Entity Panel = Scratch.CreateEntity("Panel");
        Panel.AddComponent<GameCore::TransformComponent>();
        Panel.SetParent(Root, false);
        GameCore::PrefabAsset Asset;
        GameCore::CapturePrefab(Scratch, Root.GetID(), Asset);
        Asset.SetScriptPath("Prefabs/Door.lua");
        Expect(GameCore::SavePrefabFile(Path.toUtf8().constData(), Asset), "a prefab file to open");
    }

    EditorDocument Doc;
    Doc.SetSingleRoot(true);
    GameCore::PrefabAsset Asset;
    Expect(!Doc.LoadPrefab(Temp.filePath(QStringLiteral("Missing.nmprefab")), Asset), "a missing prefab does not load");
    Expect(Doc.LoadPrefab(Path, Asset), "LoadPrefab reads it");
    Expect(Doc.GetWorld().GetEntityCount() == 2, "and spawns its entities");
    Expect(Doc.GetRootEntities().size() == 1, "under one root");
    Expect(std::string(Asset.GetScriptPath()) == "Prefabs/Door.lua", "with its settings in the asset");
    Expect(!Doc.CanUndo() && Doc.GetHistory().isClean(), "as the clean starting point");

    /* Single root: a new entity goes under the root. */
    const EntityID Root   = Doc.GetRootEntities().front();
    const EntityID Handle = Doc.CreateEntity(QStringLiteral("Handle"), EngineCore::Vector3(5.0, 0.0, 0.0));
    Expect(Doc.GetWorld().GetEntity(Handle).GetParent() == Root, "a created entity is parented under the root");
    Expect(Doc.GetRootEntities().size() == 1, "so there is still one root");
    Doc.Undo();
    Expect(Doc.GetWorld().GetEntityCount() == 2, "and undoing it removes it");
    Doc.Redo();

    QString Error;
    Expect(Doc.SavePrefab(Path, Asset, Error), "SavePrefab writes it");
    Expect(Doc.GetHistory().isClean(), "and marks the history clean");
    const uint32_t HandleId = Asset.FindLocalId("Handle");
    Expect(HandleId != GameCore::PREFAB_NO_LOCAL_ID, "with the new entity in it");

    /* Saved again, the new entity keeps its id: placed instances key on it. */
    Doc.RenameEntity(Doc.GetWorld().FindEntityByGuid(Doc.GetWorld().GetEntity(Root).GetGuid()), QStringLiteral("Door"));
    Expect(Doc.SavePrefab(Path, Asset, Error), "saving again");
    Expect(Asset.FindLocalId("Handle") == HandleId, "keeps every entity's id");

    GameCore::PrefabAsset Reread;
    Expect(GameCore::LoadPrefabFile(Path.toUtf8().constData(), Reread) && Reread.GetEntityCount() == 3
           && Reread.FindLocalId("Handle") == HandleId && std::string(Reread.GetEntityName(0)) == "Door",
           "the file has what was saved");

    /* Refusals: two roots, and a borrowed World. */
    Doc.SetSingleRoot(false);
    const EntityID Stray = Doc.CreateEntity(QStringLiteral("Stray"), EngineCore::Vector3());
    Expect(!Doc.SavePrefab(Path, Asset, Error) && Error.contains(QStringLiteral("exactly one root")), "two roots are refused");
    Doc.DestroyEntity(Stray);

    GameCore::World* Sim = EngineCore::Memory::Alloc<GameCore::World>();
    Doc.BorrowWorld(Sim);
    Expect(!Doc.SavePrefab(Path, Asset, Error), "saving while simulating is refused");
    Doc.UnhookBorrowedWorld();
    Doc.DropBorrowedWorld();
    EngineCore::Memory::Dealloc(Sim);
    Doc.ReturnToEditedWorld();
    Expect(Doc.SavePrefab(Path, Asset, Error), "and works again after");
}


static void TestInstances()
{
    std::printf("Prefab instances\n");
    QTemporaryDir Temp;
    const QString Path = Temp.filePath(QStringLiteral("Lamp.nmprefab"));
    {
        GameCore::World Scratch;
        const GameCore::Entity Root = Scratch.CreateEntity("Root");
        Root.AddComponent<GameCore::TransformComponent>();
        const GameCore::Entity Bulb = Scratch.CreateEntity("Bulb");
        Bulb.AddComponent<GameCore::TransformComponent>();
        Bulb.SetParent(Root, false);
        GameCore::PrefabAsset Asset;
        GameCore::CapturePrefab(Scratch, Root.GetID(), Asset);
        GameCore::SavePrefabFile(Path.toUtf8().constData(), Asset);
    }

    EditorDocument Doc;
    GameCore::World& W = Doc.GetEditedWorld();
    const EntityID Root = Doc.PlacePrefab(Path, EngineCore::Vector3(10.0, 0.0, 0.0));
    Expect(Root.IsSet() && W.GetInstanceCount() == 1, "PlacePrefab spawns an instance");
    Expect(W.GetEntity(Root).GetWorldPose().Position.GetX() == 10.0, "where asked");
    Expect(Doc.CanUndo(), "as one undo step");

    const GameCore::PrefabInstance Instance = W.GetInstance(W.GetEntity(Root).GetInstance());
    const GameCore::EntityGuid InstanceGuid = Instance.GetGuid();
    const GameCore::EntityGuid BulbGuid = Instance.FindEntity("Bulb").GetGuid();

    Doc.Undo();
    Expect(W.GetInstanceCount() == 0 && W.GetEntityCount() == 0, "undo removes the instance");
    Doc.Redo();
    const GameCore::PrefabInstanceID Back = W.FindInstanceByGuid(InstanceGuid);
    Expect(Back.IsSet(), "redo brings it back as an instance, with its GUID");
    Expect(W.GetInstance(Back).FindEntity("Bulb").GetGuid() == BulbGuid, "and its members' GUIDs");

    /* Clicking a member in a viewport picks the whole instance, by its root;
     * an entity in no instance picks itself, even parented under one. */
    const EntityID BackRoot = W.GetInstance(Back).GetRoot().GetID();
    Expect(Doc.GetPickTarget(W.GetInstance(Back).FindEntity("Bulb").GetID()) == BackRoot, "a member picks its instance's root");
    Expect(Doc.GetPickTarget(BackRoot) == BackRoot, "and the root itself");
    const EntityID Loose = Doc.CreateEntity(QStringLiteral("Loose"), EngineCore::Vector3());
    Expect(Doc.GetPickTarget(Loose) == Loose, "an entity in no instance picks itself");
    W.GetEntity(BackRoot).SetParent(Loose);
    Expect(Doc.GetPickTarget(W.GetInstance(Back).FindEntity("Bulb").GetID()) == BackRoot,
           "an instance under a plain parent still picks its own root");
    W.GetEntity(BackRoot).SetParent(GameCore::EntityID());
    Doc.DestroyEntity(Loose);

    /* A member deleted and undone rejoins its instance. */
    const EntityID Bulb = W.GetInstance(Back).FindEntity("Bulb").GetID();
    const uint32_t BulbId = W.GetEntity(Bulb).GetPrefabLocalId();
    Doc.DestroyEntity(Bulb);
    Expect(!W.GetInstance(Back).FindEntity("Bulb").IsAlive(), "a member can be deleted");
    Doc.Undo();
    const GameCore::Entity Rejoined = W.GetInstance(Back).FindEntity(BulbId);
    Expect(Rejoined.IsAlive() && Rejoined.GetGuid() == BulbGuid, "undo puts it back in its instance, with its id");

    /* The whole instance deleted and undone. */
    Doc.DestroyEntity(W.GetInstance(Back).GetRoot().GetID());
    Expect(W.GetInstanceCount() == 0, "deleting the root deletes the instance");
    Doc.Undo();
    const GameCore::PrefabInstanceID Again = W.FindInstanceByGuid(InstanceGuid);
    Expect(Again.IsSet() && W.GetInstance(Again).FindEntity(BulbId).IsAlive(), "undo restores it whole");
    Expect(std::string(W.GetInstance(Again).GetPrefabPath()) == std::string(Path.toUtf8().constData()), "with its prefab path");

    /* Prefabs do not nest. */
    EditorDocument PrefabDoc;
    PrefabDoc.SetSingleRoot(true);
    Expect(!PrefabDoc.PlacePrefab(Path, EngineCore::Vector3()).IsSet(), "a prefab document refuses an instance");
}


static void TestReplaceWithPrefab()
{
    std::printf("Replace with prefab\n");
    QTemporaryDir Temp;
    const QString Path = Temp.filePath(QStringLiteral("Crate.nmprefab"));

    EditorDocument Doc;
    GameCore::World& W = Doc.GetEditedWorld();
    const EntityID Crate = Doc.CreateEntity(QStringLiteral("Crate"), EngineCore::Vector3(30.0, 5.0, 0.0));
    const EntityID Lid   = Doc.CreateEntity(QStringLiteral("Lid"), EngineCore::Vector3(30.0, 6.0, 0.0),
                                            [Crate](const GameCore::Entity& InEntity) { InEntity.SetParent(Crate, true); });
    const GameCore::EntityGuid CrateGuid = W.GetEntity(Crate).GetGuid();
    const int StepsBefore = Doc.GetHistory().count();

    GameCore::PrefabAsset Asset;
    GameCore::CapturePrefab(W, Crate, Asset);
    GameCore::SavePrefabFile(Path.toUtf8().constData(), Asset);

    Expect(!Doc.ReplaceWithPrefab(Crate, Temp.filePath(QStringLiteral("Missing.nmprefab"))).IsSet() && W.IsAlive(Crate),
           "a prefab that cannot load leaves the entity alone");

    const EntityID Root = Doc.ReplaceWithPrefab(Crate, Path);
    Expect(Root.IsSet() && W.GetEntity(Root).IsInInstance(), "the subtree becomes an instance");
    Expect(!W.IsAlive(Crate) && !W.IsAlive(Lid) && W.GetEntityCount() == 2, "in place of the entities");
    Expect(W.GetEntity(Root).GetWorldPose().Position.GetX() == 30.0, "where they stood");
    Expect(Doc.GetHistory().count() == StepsBefore + 1, "as one undo step");

    Doc.Undo();
    Expect(W.GetInstanceCount() == 0 && W.IsAlive(W.FindEntityByGuid(CrateGuid)) && W.GetEntityCount() == 2,
           "undo brings the entities back");
    Doc.Redo();
    Expect(W.GetInstanceCount() == 1 && !W.IsAlive(W.FindEntityByGuid(CrateGuid)), "redo replaces them again");
}


static void TestRefreshInstances()
{
    std::printf("Refresh instances\n");
    QTemporaryDir Temp;
    const QString Path = Temp.filePath(QStringLiteral("Sign.nmprefab"));
    const QByteArray Utf8 = Path.toUtf8();
    {
        GameCore::World Scratch;
        const GameCore::Entity Root = Scratch.CreateEntity("Root");
        Root.AddComponent<GameCore::TransformComponent>();
        GameCore::PrefabAsset Asset;
        GameCore::CapturePrefab(Scratch, Root.GetID(), Asset);
        GameCore::SavePrefabFile(Utf8.constData(), Asset);
    }

    EditorDocument Doc;
    GameCore::World& W = Doc.GetEditedWorld();
    const EntityID Root = Doc.PlacePrefab(Path, EngineCore::Vector3(7.0, 0.0, 0.0));
    Doc.SetSelection(Root);
    const GameCore::EntityGuid RootGuid = W.GetEntity(Root).GetGuid();
    const int Steps = Doc.GetHistory().count();

    /* The prefab gains a child. */
    {
        GameCore::World Scratch;
        GameCore::PrefabAsset Asset = *W.GetInstance(W.GetEntity(Root).GetInstance()).GetPrefab();
        const GameCore::PrefabInstance Copy = Scratch.SpawnPrefab(Asset);
        const GameCore::Entity Post = Scratch.CreateEntity("Post");
        Post.AddComponent<GameCore::TransformComponent>();
        Post.SetParent(Copy.GetRoot(), false);
        GameCore::CapturePrefab(Scratch, Copy.GetRoot().GetID(), Asset);
        GameCore::SavePrefabFile(Utf8.constData(), Asset);
    }
    GameCore::ReloadPrefab(Utf8.constData());

    Expect(Doc.RefreshPrefabInstances(QStringLiteral("Other.nmprefab")) == 0, "other prefabs' instances are left alone");
    Expect(Doc.RefreshPrefabInstances(Path) == 1, "RefreshPrefabInstances refreshes the instance");
    const EntityID NewRoot = W.FindEntityByGuid(RootGuid);
    Expect(W.GetInstance(W.GetEntity(NewRoot).GetInstance()).FindEntity("Post").IsAlive(), "which gains the new entity");
    Expect(W.GetEntity(NewRoot).GetWorldPose().Position.GetX() == 7.0, "where it was placed");
    Expect(Doc.GetSelection() == NewRoot, "the selection follows its GUID");
    Expect(Doc.GetHistory().count() == Steps, "and nothing is recorded");
    GameCore::ClearPrefabCache();
}


static void TestConvertTrees()
{
    std::printf("Convert trees to prefabs\n");
    QTemporaryDir Temp;
    const QString Path = Temp.filePath(QStringLiteral("Barrel.nmprefab"));

    EditorDocument Doc;
    GameCore::World& W = Doc.GetEditedWorld();
    const auto MakeBarrel = [&](const char* InName, const double InX, const double InLidY)
    {
        const EntityID Root = Doc.CreateEntity(QString::fromLatin1(InName), EngineCore::Vector3(InX, 0.0, 0.0));
        Doc.CreateEntity(QStringLiteral("Lid"), EngineCore::Vector3(InX, InLidY, 0.0),
                         [Root](const GameCore::Entity& InEntity) { InEntity.SetParent(Root, true); });
        return Root;
    };
    const EntityID A = MakeBarrel("BarrelA", 0.0, 2.0);
    const EntityID B = MakeBarrel("BarrelB", 50.0, 5.0);
    Expect(Doc.GetLooseRoots().size() == 2, "both trees are loose");

    GameCore::PrefabAsset Asset;
    GameCore::CapturePrefab(W, A, Asset);
    GameCore::SavePrefabFile(Path.toUtf8().constData(), Asset);
    const int Steps = Doc.GetHistory().count();

    Expect(Doc.ConvertTreesToPrefabs({ { A, Path }, { B, Path } }) == 2, "both trees convert");
    Expect(W.GetInstanceCount() == 2 && Doc.GetLooseRoots().empty(), "into instances; nothing loose is left");
    Expect(Doc.GetHistory().count() == Steps + 1, "as one undo step");

    /* B's own values, not the prefab's (A's). */
    struct Find { GameCore::World* W; EntityID Found; } Ctx{ &W, EntityID() };
    W.ForEachInstance([](const GameCore::PrefabInstanceID InInstance, void* InUserData)
    {
        auto& C = *static_cast<Find*>(InUserData);
        const GameCore::Entity Root = C.W->GetInstance(InInstance).GetRoot();
        if(std::string(Root.GetName()) == "BarrelB") { C.Found = Root.GetID(); }
    }, &Ctx);
    const EntityID BRoot = Ctx.Found;
    Expect(BRoot.IsSet(), "the second keeps its own name");
    const GameCore::Entity BLid = W.GetEntity(W.GetEntity(BRoot).GetFirstChild());
    Expect(W.GetEntity(BRoot).GetWorldPose().Position.GetX() == 50.0, "and its placement");
    Expect(BLid.GetWorldPose().Position.GetY() == 5.0, "and its child's values, as overrides");

    Doc.Undo();
    Expect(W.GetInstanceCount() == 0 && Doc.GetLooseRoots().size() == 2 && W.GetEntityCount() == 4,
           "undo brings the loose trees back");
    Doc.Redo();
    Expect(W.GetInstanceCount() == 2 && W.GetEntityCount() == 4, "redo converts them again");
}

int main(int argc, char* argv[])
{
    QCoreApplication App(argc, argv);
    std::printf("Editor document tests\n");

    TestCreateUndoRedo();
    TestSelection();
    TestHierarchyBounds();
    TestMultiSelection();
    TestClipboard();
    TestCopyInstance();
    TestGroupEdit();
    TestNewScene();
    TestBorrowedWorld();
    TestDestroyWhileBorrowed();
    TestPrefab();
    TestInstances();
    TestReplaceWithPrefab();
    TestRefreshInstances();
    TestConvertTrees();

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
