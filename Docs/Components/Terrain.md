# Terrain

Reference for the **Terrain** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Terrain` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::TerrainComponent` |
| Header | `#include "Public/RenderCore/Components/TerrainComponent.h"` |
| Size | 424 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `TerrainComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Terrain` | string | (empty) | asset: terrain (.nmterrain) |
| `Material` | string | (empty) | asset: material graph (.material.nmgraph) |
| `Visible` | uint8_t (flag) | true |  |
| `UV Tile Size` | float | 400 |  |

### Foliage

| Property | Type | Default | Notes |
|---|---|---|---|
| `Foliage` | string | (empty) | asset: foliage (.nmfoliage) |
| `Foliage Visible` | uint8_t (flag) | true |  |

### Collision

| Property | Type | Default | Notes |
|---|---|---|---|
| `Walkable` | uint8_t (flag) | true |  |

### Shadows

| Property | Type | Default | Notes |
|---|---|---|---|
| `Cast Shadows` | uint8_t (flag) | true |  |

### Culling

| Property | Type | Default | Notes |
|---|---|---|---|
| `Occluder` | uint8_t (flag) | true |  |
| `LOD Distance` | float | 0 |  |

## C++ API

```cpp
#include "Public/RenderCore/Components/TerrainComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::TerrainComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::TerrainComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::TerrainComponent>();
Owner.RemoveComponent<RenderCore::TerrainComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Terrain")      -- nil if absent
local c = entity:AddComponent("Terrain")
entity:HasComponent("Terrain")
entity:RemoveComponent("Terrain")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetTerrain()` -> string<br>`c:SetTerrain(string)` | Property. |
| `c:GetMaterial()` -> string<br>`c:SetMaterial(string)` | Property. |
| `c:GetVisible()` -> boolean<br>`c:SetVisible(boolean)` | Property. |
| `c:GetCastShadows()` -> boolean<br>`c:SetCastShadows(boolean)` | Property. |
| `c:GetOccluder()` -> boolean<br>`c:SetOccluder(boolean)` | Property. |
| `c:GetWalkable()` -> boolean<br>`c:SetWalkable(boolean)` | Property. |
| `c:GetUVTileSize()` -> number<br>`c:SetUVTileSize(number)` | Property. |
| `c:GetLODDistance()` -> number<br>`c:SetLODDistance(number)` | Property. |
| `c:GetFoliage()` -> string<br>`c:SetFoliage(string)` | Property. |
| `c:GetFoliageVisible()` -> boolean<br>`c:SetFoliageVisible(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
