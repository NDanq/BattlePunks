# Collision

Reference for the **Collision** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Collision` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `GameCore::CollisionComponent` |
| Header | `#include "Public/GameCore/Components/CollisionComponent.h"` |
| Size | 160 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `CollisionComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Enabled` | uint8_t (flag) | true |  |

### Shape

| Property | Type | Default | Notes |
|---|---|---|---|
| `Source` | uint8_t | 0 | choice: Bounds component, Render component mesh |

### Filtering

| Property | Type | Default | Notes |
|---|---|---|---|
| `Layer` | int32_t | 1 |  |
| `Mask` | int32_t | -1 |  |

## C++ API

```cpp
#include "Public/GameCore/Components/CollisionComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<GameCore::CollisionComponent>();        // nullptr if absent
Owner.AddComponent<GameCore::CollisionComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<GameCore::CollisionComponent>();
Owner.RemoveComponent<GameCore::CollisionComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Collision")      -- nil if absent
local c = entity:AddComponent("Collision")
entity:HasComponent("Collision")
entity:RemoveComponent("Collision")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |
| `c:GetLayer()` -> integer<br>`c:SetLayer(integer)` | Property. |
| `c:GetMask()` -> integer<br>`c:SetMask(integer)` | Property. |
| `c:GetOverlapCount()` -> integer | Read-only property. |
| `c:GetSource()` -> Source:string *(no effects)* | Method. |
| `c:SetSource(Source:string)` | Method. |
| `c:IsOverlapping(Other:Entity)` -> Overlapping:bool *(no effects)* | Method. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
