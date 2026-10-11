# Render

Reference for the **Render** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Render` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::RenderComponent` |
| Header | `#include "Public/RenderCore/Components/RenderComponent.h"` |
| Size | 1600 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `RenderComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Visible` | uint8_t (flag) | true |  |

### Appearance

| Property | Type | Default | Notes |
|---|---|---|---|
| `Mesh` | string | (empty) | asset: mesh (.nmmesh) |
| `Materials` | string | (8 elements) | asset: material graph (.material.nmgraph); array of 8 |

### Shadows

| Property | Type | Default | Notes |
|---|---|---|---|
| `Cast Shadows` | uint8_t (flag) | true |  |
| `Cast Inset Shadow` | uint8_t (flag) | false |  |

### Culling

| Property | Type | Default | Notes |
|---|---|---|---|
| `Occluder` | uint8_t (flag) | false |  |

### Material Parameters

| Property | Type | Default | Notes |
|---|---|---|---|
| `Parameters` | struct[8] | (8 elements) | array of 8 |
| `Parameters[].Slot` | uint8_t | |  |
| `Parameters[].Name` | char[N] | |  |
| `Parameters[].X` | float | |  |
| `Parameters[].Y` | float | |  |
| `Parameters[].Z` | float | |  |
| `Parameters[].W` | float | |  |

## C++ API

```cpp
#include "Public/RenderCore/Components/RenderComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::RenderComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::RenderComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::RenderComponent>();
Owner.RemoveComponent<RenderCore::RenderComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Render")      -- nil if absent
local c = entity:AddComponent("Render")
entity:HasComponent("Render")
entity:RemoveComponent("Render")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetVisible()` -> boolean<br>`c:SetVisible(boolean)` | Property. |
| `c:GetMesh()` -> string<br>`c:SetMesh(string)` | Property. |
| `c:GetMaterial(Index:integer)` -> Material:string *(no effects)* | Method. |
| `c:SetMaterial(Index:integer, Material:string)` -> Set:bool | Method. |
| `c:GetNumRegisteredMaterials()` -> Count:integer *(no effects)* | Method. |
| `c:SetMaterialParameter(Slot:integer, Name:string, X:number, Y:number, Z:number, W:number)` -> Set:bool | Method. |
| `c:SetMaterialColor(Slot:integer, Name:string, Color:Color)` -> Set:bool | Method. |
| `c:GetMaterialParameter(Slot:integer, Name:string)` -> X:number, Y:number, Z:number, W:number, Set:bool *(no effects)* | Method. |
| `c:ClearMaterialParameter(Slot:integer, Name:string)` -> Cleared:bool | Method. |
| `c:GetCastShadows()` -> boolean<br>`c:SetCastShadows(boolean)` | Property. |
| `c:GetCastInsetShadow()` -> boolean<br>`c:SetCastInsetShadow(boolean)` | Property. |
| `c:GetOccluder()` -> boolean<br>`c:SetOccluder(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
