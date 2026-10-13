# Nav Mesh Volume

Reference for the **Nav Mesh Volume** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Nav Mesh Volume` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `AICore::NavMeshVolumeComponent` |
| Header | `#include "Public/AICore/Components/NavMeshVolumeComponent.h"` |
| Size | 224 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `NavMeshVolumeComponent`.

### Volume

| Property | Type | Default | Notes |
|---|---|---|---|
| `Half Extents` | Vector3 | (5000, 1000, 5000) | cm, about the entity: what the build covers |
| `Nav Mesh` | string | (empty) | asset: navmesh (.nmnav); the baked file the level loads; World > Navigation > Build writes it |
| `Build At Runtime` | uint8_t (flag) | true | builds when there is no file, or it is older than the level's geometry |
| `Rebuild On Change` | uint8_t (flag) | true | the editor rebuilds tiles under geometry as it moves; a level, under terrain that changes |

### Agent

| Property | Type | Default | Notes |
|---|---|---|---|
| `Agent Radius` | float | 35 | cm; walls are eroded by this much |
| `Agent Height` | float | 180 | cm of headroom |
| `Agent Max Climb` | float | 45 | cm: the tallest step walked up |
| `Agent Max Slope` | float | 45 | degrees: the steepest walkable ramp |

### Voxels and Tiles

| Property | Type | Default | Notes |
|---|---|---|---|
| `Cell Size` | float | 15 | cm: voxel width (X/Z); smaller is finer and slower |
| `Cell Height` | float | 10 | cm: voxel height (Y) |
| `Tile Size` | int32_t | 48 | voxels a tile side; a moved obstacle rebuilds only its tiles |
| `Region Min Size` | float | 8 | voxels a side: smaller islands are dropped |
| `Region Merge Size` | float | 20 | voxels: smaller regions merge into neighbours |
| `Edge Max Length` | float | 1200 | cm; 0 for no limit |
| `Edge Max Error` | float | 1.3 | voxels a contour may stray |
| `Layer Mask` | int32_t | -1 | physics layers that count as ground |

## C++ API

```cpp
#include "Public/AICore/Components/NavMeshVolumeComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<AICore::NavMeshVolumeComponent>();        // nullptr if absent
Owner.AddComponent<AICore::NavMeshVolumeComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<AICore::NavMeshVolumeComponent>();
Owner.RemoveComponent<AICore::NavMeshVolumeComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Nav Mesh Volume")      -- nil if absent
local c = entity:AddComponent("Nav Mesh Volume")
entity:HasComponent("Nav Mesh Volume")
entity:RemoveComponent("Nav Mesh Volume")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetHalfExtents()` -> x, y, z<br>`c:SetHalfExtents(x, y, z)` | Property. |
| `c:GetAgentRadius()` -> number<br>`c:SetAgentRadius(number)` | Property. |
| `c:GetAgentHeight()` -> number<br>`c:SetAgentHeight(number)` | Property. |
| `c:GetNavMesh()` -> string<br>`c:SetNavMesh(string)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
