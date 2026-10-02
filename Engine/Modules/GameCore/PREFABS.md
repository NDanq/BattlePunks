# Prefabs

Design for prefabs: the placeable unit of a level, the owner of gameplay
script, and the asset the Prefab Editor authors. Status: **phases 1 (entity
hierarchy), 2 (prefab asset and instances), 3 (instance scripts) and 4
(instances in level files) are implemented**; the rest is the plan. The open decisions were settled as recommended (see the end).

---

## The model

```
Level (.nmworld)
 `-- PrefabInstance  (placed: a prefab path, a transform, overrides)
      |-- script instance + tick settings      <- behaviour lives here
      `-- Entity (root)
           |-- Entity (child)                  <- parented inside the prefab
           |    `-- components
           `-- components
```

- **Entity** -- unchanged in spirit: an `EntityID`, a name, a GUID, and a set
  of components. It **loses its script** and **gains a parent**.
- **Prefab** -- an asset (`.nmprefab`): a tree of entity templates with their
  components, one script, and tick settings. A definition only; nothing ticks.
- **PrefabInstance** -- a prefab spawned into a World. Owns its entities, its
  script instance and its tick state. This is what a level places, what the
  Outliner lists at top level, and what Lua's `self` is.

Raw entities still exist at runtime -- `World` stays an ECS, systems still
`Each<T>` over pools, and C++ can still `CreateEntity`. The rule "prefabs are
what's placeable" is enforced by the **level format and the editor**, not by
`World`. Every entity a level loads belongs to an instance.

---

## 1. Entity hierarchy (GameCore) -- implemented

How it works is in README.md's **Hierarchy** section and DESIGN.md's
`Entity.h` notes. In short:

- Parent and an ordered child list are **entity members** (slot indices, an
  intrusive list), set with `Entity::SetParent(parent, keepWorld = true)`.
  Cycles and dead entities are refused.
- `TransformComponent` is **local**. `Entity::GetWorldPose()` /
  `GetWorldMatrix()` compose position, rotation and scale from the root down
  (`TransformMath::Compose`). `SetWorldPose()` writes the local that lands
  there.
- **Computed on demand, not cached.** This replaces the dirty-flag cache
  proposed earlier: Transform is POD written directly, so World cannot see
  writes, and on-demand is always correct. Unparented entities take the old
  path at the old cost. If deep hierarchies show up in a profile, the fix is
  a memcmp-against-shadow cache.
- The spatial refit, `WorldBridge` render extraction (meshes and the camera),
  the editor's bounds, picking and gizmo all read world transforms.
- Destroying a parent destroys its subtree (deferred inside a tick).
  `RefreshEntityBounds` refits the subtree.
- Saved as `Parent="<guid>"` in `.nmentity` and a parent index per entity in
  the binary `WRLD` chunk (version 2; version 1 still loads).
- Lua: `SetParent`, `GetParent`, `GetChild(ren)`, `GetChildCount`, and the
  Transform's `Get/SetWorldPosition`, `Get/SetWorldRotation`,
  `GetWorldScale`. Graph nodes: Get Parent, Set Parent, Get Child Count, Get
  Child.
- Editor: the Outliner is a tree, drag to reparent; undo covers reparenting
  and restores a deleted subtree whole.

Still to do with prefabs: refusing a reparent across prefab instances
(phase 2).

---

## 2. The prefab asset

### `.nmprefab`, text (editor)

One XML file -- prefabs are small, and one file keeps rename/move trivial.
Reuses `WorldPackage.cpp`'s XML `ComponentArchive` and deterministic writer.

```xml
<Prefab Format="1" Name="Pickup">
  <Script Path="Prefabs/Pickup.script.nmgraph" />
  <Tick Mode="Tick" />                              <!-- Tick | EventsOnly | Off -->
  <Entity Id="1" Name="Root">
    <Component Type="Transform" Version="1">...</Component>
    <Component Type="Collision" Version="1">...</Component>
  </Entity>
  <Entity Id="2" Name="Mesh" Parent="1">
    <Component Type="Render" Version="2">...</Component>
  </Entity>
</Prefab>
```

- `Id` is a **prefab-local** integer, stable across edits (never reused once
  assigned). It is what overrides and saved games key on.
- Exactly one root (`Parent` absent). The root's Transform is the instance's
  placement; children are relative to it.
- Cooked: a binary form using `WorldSerializer`'s chunk layout (`PRFB` entity
  table + `CMPT` chunks), so a type a build lacks is skipped the same way.

### Placement in a level

The `.nmworld` manifest lists instances instead of entities:

```xml
<World Format="2">
  <Instance Guid="..." Prefab="Prefabs/Pickup.nmprefab" />
</World>
```

and `<Name>/<instance-guid>.nmprefabinst` holds the instance's name, root
transform and overrides. One file per instance keeps the "one change, one
diff" property `WorldPackage` was built for.

**Entity GUIDs are derived**, not stored: `Hash(InstanceGuid, LocalId)`. A
reference from one instance's component to another's entity, a save game, and
`FindEntityByGuid` all keep working, and adding an entity to the prefab gives
every existing instance a new entity with a stable GUID automatically.

### Overrides

An instance may change a component field on one of its entities without
forking the prefab. Stored as `(LocalId, ComponentType, FieldName, Value)` --
the `ComponentArchive` already reads and writes by field name, so an override
is "the instance's archive for that component, containing only the changed
fields", applied over the prefab's after spawn. The Details panel shows
overridden fields in bold with a "Revert to Prefab" action.

v1 scope: field overrides on existing entities, including the root Transform.
Not in v1: adding/removing entities or components per instance.

**As built (phase 4)**, with GameCore's README **World files** for the
format and DESIGN.md's WorldPackage notes for the reasoning:

- Files are `<Name>/<guid>.nmprefabinst`, with overrides grouped per
  template entity (`<Entity Id>`). The manifest is format 2 only when it
  lists an instance.
- Field overrides are found by diffing each component's XML against the
  prefab's.
- **Beyond v1:** added components (written whole), removed components
  (`<RemovedComponent>`), removed template entities (`<Removed>`), renames
  and member entity scripts are recorded. They fell out of the diff, and
  without them a save would silently undo those edits. Adding a *member*
  entity is still out: an entity added under an instance is a loose child
  saved as an `.nmentity`.
- Binary worlds (cooked levels, save games) store instances whole, with an
  instance table (`WRLD` v3, `World::RestorePrefabInstance`), not as
  overrides.
- The Details panel's bold overrides and "Revert to Prefab" are phase 8.

**Decided (not in v1):** nested prefabs (an entity in a prefab that is itself an instance of
another prefab). Recommended **not** for v1 -- it multiplies the override and
GUID-derivation rules -- but the format reserves `<Entity Prefab="...">` so it
can be added without a format break.

### Runtime API

```cpp
PrefabInstanceID World::SpawnPrefab(const PrefabAsset& InPrefab, const Transform& InAt,
                                    const char* InName = nullptr);
void             World::DestroyPrefabInstance(PrefabInstanceID);   // deferred in a tick

class PrefabInstance                       // handle, like Entity: {World*, PrefabInstanceID}
{
    Entity      GetRoot() const;
    Entity      FindEntity(const char* InName) const;   // by template name
    Entity      FindEntity(uint32_t InLocalId) const;
    const char* GetPrefabPath() const;
    void        SetTickMode(ETickMode) const;
    bool        IsPhaseEnabled(ETickPhase) const;
    void        SetPhaseEnabled(ETickPhase, bool) const;
};
```

`PrefabInstanceID` is an index + generation, same as `EntityID`. Each entity
gets an `Instance` member pointing back. Prefab assets are loaded once and
cached by path in a `PrefabRegistry` (in `GameCore.dll`, per the
"GameCore.dll allocates" rule).

### As built (phase 2)

GameCore's README **Prefabs** section has the usage; DESIGN.md's `Prefab.h`
section has the reasoning. Differences from the sketch above:

- `SpawnPrefab` returns the `PrefabInstance` handle, takes a
  `TransformPose`, and has a by-path overload through the registry.
- Tick mode is `EPrefabTickMode` (Tick, EventsOnly, Off). It is stored on
  the asset and the instance; `SetPhaseEnabled` / `IsPhaseEnabled` come with
  phase 3, which acts on them.
- Components are stored as serializer blobs. The text form names
  references as `<Field Entity="<local id>"/>`.
- The root's Transform is captured as identity: placement is the
  instance's.
- Membership is on each entity (`GetInstance`, `GetPrefabLocalId`). A
  non-root member cannot be reparented out of its instance (the phase 1
  "Still to do").
- Lua `World.SpawnPrefab(path[, x, y, z])` and a **Spawn Prefab** graph node.

Known gaps, owned by later phases:

- The editor cannot place or show instances yet. Its undo restores a
  deleted instance member as a loose entity (phase 8).
- Nothing cooks `.nmprefab` files automatically; `CookPrefabFile` exists
  for the editor's cook step (phase 7/8).

---

## 3. Script on the prefab

Today: one script per entity (`Entity::SetScript`), a class picks **one**
`TickPhase`, and the driver walks every scripted entity every phase and skips
mismatches (`ScriptSystem.cpp`).

Proposed: one script per **prefab instance**, a function **per phase**, and
per-phase buckets so a phase only visits what ticks in it.

### Lua class shape

```lua
return {
    OnStart       = function(self) end,
    OnPrePhysics  = function(self, dt) end,   -- define only the phases you need
    OnPhysics     = function(self, dt) end,
    OnPostPhysics = function(self, dt) end,
    OnPostUpdate  = function(self, dt) end,
    OnDestroy     = function(self) end,

    -- delegate-style events still arrive regardless of phase functions
    OnOverlapBegin = function(self, other, mine) end,   -- mine: which of MY entities
    OnOverlapEnd   = function(self, other, mine) end,
}
```

- `self.Root` is the root `Entity`; `self:Find("Mesh")` returns a child by
  template name; `self.Instance` the `PrefabInstance`.
- **Back-compat:** `OnTick` + `TickPhase = TickPhase.X` is read as `OnX`. Every
  existing script and compiled graph keeps working unchanged.
- When the class loads, the driver records a **phase mask** from which
  `On<Phase>` functions exist. An instance is inserted into the bucket of each
  phase in its mask and nowhere else -- a prefab with only `OnPostUpdate` costs
  nothing in the other three phases (today it costs a lookup and a compare in
  each).
- Overlap events: today `World::UpdateOverlaps` calls the entity's script.
  It now resolves the entity's instance and calls the instance script with
  `mine` set to the entity that overlapped. Two colliders of the same instance
  overlapping each other are not reported.

### Tick modes

```cpp
enum class ETickMode : uint8_t
{
    Tick,         // default: phase functions run as defined
    EventsOnly,   // script instantiated, OnStart/OnDestroy/overlap/custom events
                  // and delegates run; no phase function is ever called
    Off,          // static geometry: no script instance at all
};
```

- `Off` also marks every entity's `BoundsComponent` static (never refit) and
  excludes the instance from hierarchy propagation after the first pass -- the
  static-geometry case gets cheaper, not just script-free.
- Set in the prefab, overridable per instance, and switchable at runtime
  (`SetTickMode`, `SetPhaseEnabled`), applied in the phase flush like
  `SetScript` is today (same re-entrancy reason: the driver is walking the
  bucket).

### Script graphs

New event nodes **On Pre-Physics / On Physics / On Post-Physics / On Post
Update**, each with `DeltaSeconds`; `On Tick` stays and keeps its
`TickPhase` property. The compiler emits one `On<Phase>` function per event
node present, so the phase mask falls out automatically. A **Self** node group:
`Get Root`, `Find Entity (name)`, `Set Tick Mode`, `Set Phase Enabled`.
`On Overlap Begin/End` gain a `Mine` output.

### As built (phase 3)

GameCore's README **A prefab's script** has the usage; DESIGN.md's
`ScriptSystem.cpp` notes have the reasoning. Differences from the sketch
above:

- `OnStart` runs at the first phase start after spawning, not in the
  instance's "own" phase: an instance may tick in several phases or none.
- `self.Entity` is set as well as `self.Root` (the same entity), so entity
  scripts and existing graphs, whose Self node reads `self.Entity`, run on
  a prefab unchanged.
- A raising instance stays in its phase lists and is skipped: lists only
  change between walks.
- `Off` at runtime calls `OnDestroy`; leaving `Off` starts the script
  afresh. `Off` at spawn makes Bounds static; a later switch does not undo
  that. The design's "excluded from hierarchy propagation" does not apply,
  because world transforms are computed on demand (phase 1).
- Entity scripts get `mine` too (the entity itself), so one overlap handler
  reads the same on either.
- The graph **Prefab** group also has **Destroy Prefab**.

### Entity scripts go away

`Entity::SetScript`, the `Script` element in `.nmentity`, and the script path
in the binary entity table are removed at the end of migration (section 6).
Until then both drivers run: entity scripts as today, instance scripts in the
new buckets, entity scripts first.

---

## 4. Native (C++) prefabs

A game module declares a prefab in code, the way it declares script libraries
today (`ScriptLibrary.h`): registered at static init, unregistered on module
unload.

```cpp
static void BuildMarker(GameCore::PrefabBuilder& B)
{
    auto Root = B.Root("Marker");
    Root.Add(GameCore::TransformComponent{});
    Root.Add(GameCore::CollisionComponent{ .Layer = Layers::Player });
    auto Mesh = Root.Child("Mesh");
    Mesh.Add<RenderCore::RenderComponent>();
    B.Script("Scripts/JumpCounter.script.nmgraph");   // optional
    B.TickMode(GameCore::ETickMode::Tick);
}

static void MarkerPrePhysics(GameCore::PrefabInstance Self, float Dt) { ... }

NIMBLE_DEFINE_NATIVE_PREFAB(Marker, "Sandbox.Marker", BuildMarker,
    .OnStart = ..., .OnPhase = { MarkerPrePhysics, nullptr, nullptr, nullptr });
```

- Behaviour is a table of function pointers (`OnStart`, `OnPhase[TP_MAX]`,
  `OnDestroy`, `OnOverlapBegin/End`) plus `void*` -- not a virtual class, for
  the cross-image reason in DESIGN.md's Systems section. Null entries set the
  phase mask the same way a missing Lua function does.
- A native prefab may also name a Lua/graph script; native callbacks run first
  in each phase, then the script.
- Unload safety: `UnregisterAll` on module unload destroys live instances of
  that module's native prefabs first (the DANGLING CALLBACKS hazard).

**Decided (both):** how the editor treats a native prefab. The two options:
1. **Reference** -- it appears in the Content Browser as
   `native:Sandbox.Marker`, is placeable directly, opens read-only in the Prefab
   Editor, and changes in code reach every placed instance on recompile.
2. **Fork** -- "Create Prefab from Native" runs the builder once and saves a
   normal, editable `.nmprefab`; code changes after that do not propagate.

Recommend supporting **both**: reference is the default for code-owned
prefabs, and "Save As Asset" forks one when a designer wants to take it over.
A forked asset can keep the native behaviour table by name
(`<Native Behavior="Sandbox.Marker" />`) so a designer edits layout while
code keeps the logic.

---

## 5. Prefab Editor (Editor module)

A new window, opened from the Content Browser (double-click a `.nmprefab`,
right-click > New Prefab) and File > New Prefab. One prefab per tab, like the
Graph Editor.

```
+---------------------------------------------------------------------------+
| Toolbar: Save | Simulate | Tick Mode [Tick v] | Script [Graph|Lua|None]   |
+----------------+-------------------------------------+--------------------+
| Hierarchy      |  Viewport (the prefab alone)        | Details            |
|  Root          |                                     |  Entity: Name      |
|   +- Mesh      |                                     |  Components ...    |
|   +- Trigger   |                                     |  [+ Add Component] |
| [+ Entity]     |                                     |                    |
+----------------+-------------------------------------+--------------------+
| Script: graph editor tab  OR  Lua text editor   (bottom dock, tabbed)     |
+---------------------------------------------------------------------------+
```

- **Hierarchy**: tree of the prefab's entities; drag to reparent; add,
  duplicate, rename, delete. Exactly one root.
- **Details**: the existing `DetailsPanel`, per selected entity: name, add /
  remove components, property editing with undo.
- **Viewport**: the existing `ViewportWidget` + gizmo over the prefab's own
  World, framed on the prefab's bounds, with a ground grid.
- **Script**: pick Graph, Lua, or None.
  - Graph embeds a `GraphEditorTab` on `<Prefab>.script.nmgraph`.
  - Lua is a text editor (`QPlainTextEdit` + a syntax highlighter, line
    numbers, error line from the last compile/run) on `<Prefab>.lua`.
  - The script file is created beside the `.nmprefab` and referenced by path,
    so the existing Lua staging / graph-to-Lua cook keeps working unchanged.
  - **Decided (sibling file):** embedding the script inside the `.nmprefab`
    was the alternative. Two files keep the existing pipelines and let a
    script be shared by several prefabs.
- **Simulate**: ticks the prefab's World in place so the script can be seen
  running in isolation; Stop restores the authored state (same snapshot
  mechanism Play-in-Editor uses).
- **Native**: a native prefab opens read-only with a "Save As Asset" button.

### Refactor this needs

`ViewportWidget`, `DetailsPanel` and `OutlinerPanel` each take the one
`EditorContext&`, which owns the level's World, the Renderer, selection and
undo. The Prefab Editor needs a second World with its own selection and undo:

1. Split `EditorContext` into a per-document **`EditorDocument`** (World,
   selection, `EditorHistory`, dirty flag, signals) and the app-wide context
   (Renderer, project paths). Panels bind to a document.
2. Check the Renderer can draw two Worlds into two surfaces. `MeshPreviewView`
   uses `Renderer::RequestMeshPreview`, which suggests one scene plus
   offscreen previews; a second full `RenderScene` per document may be needed.
   **This is the biggest unknown in the editor work** -- spike it first.

### Level editor changes

- Outliner lists **instances** at top level, expandable to their entities.
- Selecting an entity inside an instance edits it as an **override**.
- Content Browser drag of a `.nmprefab` into the viewport places an instance.
- "Create Entity" in a level is replaced by "Create Empty Prefab Instance"
  (an instance of a built-in one-entity prefab) -- or removed, see migration.
- "Convert Selection to Prefab": takes selected raw entities, writes a
  `.nmprefab`, replaces them with one instance.

---

## 6. Migration

Existing levels contain raw entities with per-entity scripts.

1. **Load:** a raw `<Entity>` in a Format-1 manifest still loads, as a
   **loose entity** (no instance). Its script runs through the legacy entity
   driver.
2. **Editor:** loose entities show in the Outliner with a warning badge.
   "Convert to Prefab" (per entity or a whole level) writes one `.nmprefab` per
   distinct (component set + script) and replaces each entity with an
   instance + overrides. Saving writes Format 2.
3. **C++ levels** (`SandboxLevel::OnLoad`) move to native prefabs + `SpawnPrefab`.
4. **Removal:** once Sandbox is converted, delete `Entity::SetScript`, the
   entity driver path and Format-1 load support.

---

## Phases

Each phase lands on its own and keeps Sandbox playable.

| # | Phase | Main files | Depends on |
|---|---|---|---|
| 1 | Entity hierarchy + world transforms -- **done** | `World.h/.cpp`, `Entity.h`, `WorldBridge.cpp`, `WorldPackage.cpp`, `WorldSerializer.cpp`, editor picking/gizmo/Outliner | -- |
| 2 | Prefab asset, registry, `SpawnPrefab`, instance handles, text + cooked formats -- **done** | new `Prefab.h/.cpp`, `PrefabInternal.h`, `XmlComponentArchive.h` | 1 |
| 3 | Instance scripts: per-phase functions, buckets, tick modes, overlap routing; graph nodes -- **done** | `ScriptSystem.cpp`, `Collision.cpp`, `ScriptBindings.cpp`, NodeGraph `ScriptSchema.cpp` | 2 |
| 4 | Level format 2: instances, derived GUIDs, overrides -- **done** | `WorldPackage.cpp`, `WorldSerializer.cpp` | 2 |
| 5 | Native prefabs + `PrefabBuilder` | new `NativePrefab.h/.cpp`, `GameModule.h` | 2, 3 |
| 6 | `EditorDocument` split; second-World viewport spike | `EditorContext`, panels, RenderCore | -- (can start in parallel) |
| 7 | Prefab Editor window (hierarchy, details, viewport, script tab, simulate) | new `Editor/Prefab/*` | 3, 6 |
| 8 | Level editor: instances in Outliner, overrides in Details, place from Content Browser, convert | Outliner, Details, Viewport | 4, 7 |
| 9 | Migrate Sandbox; remove entity scripts and Format 1 | Sandbox, `ScriptSystem.cpp`, `Entity.h` | 8 |

---

## Decisions

Settled 2026-10-02:

1. **Nested prefabs:** not in v1. The format reserves `<Entity Prefab="...">`.
2. **Per-instance structural changes:** not in v1. Field overrides only.
3. **Native prefabs in the editor:** both. Placed by reference by default;
   "Save As Asset" forks one.
4. **Script storage:** a sibling file next to the `.nmprefab`, referenced by
   path.
5. **Loose entities:** allowed at runtime (`CreateEntity` from C++ / Lua),
   never in a saved level. Spawning a prefab is the preferred path.
6. **Script ownership:** the prefab only, one script per instance.
