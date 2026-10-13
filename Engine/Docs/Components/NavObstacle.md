# Nav Obstacle

Reference for the **Nav Obstacle** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Nav Obstacle` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `AICore::NavObstacleComponent` |
| Header | `#include "Public/AICore/Components/NavObstacleComponent.h"` |
| Size | 80 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `NavObstacleComponent`.

### Nav Obstacle

| Property | Type | Default | Notes |
|---|---|---|---|
| `Half Extents` | Vector3 | (50, 100, 50) | cm; a Cylinder's X is its radius, Y half its height |
| `Shape` | uint8_t | 0 | choice: Box, Cylinder; a Box turns with the entity's yaw; a Cylinder stands upright |
| `Enabled` | uint8_t (flag) | true | cut from the navmesh while on; switching it re-cuts the tiles next tick |

## C++ API

```cpp
#include "Public/AICore/Components/NavObstacleComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<AICore::NavObstacleComponent>();        // nullptr if absent
Owner.AddComponent<AICore::NavObstacleComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<AICore::NavObstacleComponent>();
Owner.RemoveComponent<AICore::NavObstacleComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Nav Obstacle")      -- nil if absent
local c = entity:AddComponent("Nav Obstacle")
entity:HasComponent("Nav Obstacle")
entity:RemoveComponent("Nav Obstacle")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetHalfExtents()` -> x, y, z<br>`c:SetHalfExtents(x, y, z)` | Property. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
