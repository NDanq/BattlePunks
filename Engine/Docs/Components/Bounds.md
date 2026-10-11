# Bounds

Reference for the **Bounds** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Bounds` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `GameCore::BoundsComponent` |
| Header | `#include "Public/GameCore/Components/BoundsComponent.h"` |
| Size | 96 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `BoundsComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Local` | AABB | (3.40282e+38, 3.40282e+38, 3.40282e+38) to (-3.40282e+38, -3.40282e+38, -3.40282e+38) |  |
| `World` | AABB | (3.40282e+38, 3.40282e+38, 3.40282e+38) to (-3.40282e+38, -3.40282e+38, -3.40282e+38) | read-only |
| `Static` | uint8_t (flag) | false |  |

## C++ API

```cpp
#include "Public/GameCore/Components/BoundsComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<GameCore::BoundsComponent>();        // nullptr if absent
Owner.AddComponent<GameCore::BoundsComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<GameCore::BoundsComponent>();
Owner.RemoveComponent<GameCore::BoundsComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Bounds")      -- nil if absent
local c = entity:AddComponent("Bounds")
entity:HasComponent("Bounds")
entity:RemoveComponent("Bounds")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetLocal()` -> minX, minY, minZ, maxX, maxY, maxZ<br>`c:SetLocal(minX, minY, minZ, maxX, maxY, maxZ)` | Property. |
| `c:GetWorld()` -> minX, minY, minZ, maxX, maxY, maxZ | Read-only property. |
| `c:GetStatic()` -> boolean<br>`c:SetStatic(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
