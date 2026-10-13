# Nav Modifier

Reference for the **Nav Modifier** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Nav Modifier` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `AICore::NavModifierComponent` |
| Header | `#include "Public/AICore/Components/NavModifierComponent.h"` |
| Size | 48 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `NavModifierComponent`.

### Nav Modifier

| Property | Type | Default | Notes |
|---|---|---|---|
| `Half Extents` | Vector3 | (200, 100, 200) | cm, about the entity, turned with its yaw |
| `Area` | uint8_t | 1 | choice: Blocked, High cost, Custom; Blocked: a hole; High cost: walkable at Cost; Custom: Custom Area at Cost |
| `Custom Area` | uint8_t | 2 | 2..62, for a game's own query filters |
| `Cost` | float | 5 | how many times dearer a path through it is |

## C++ API

```cpp
#include "Public/AICore/Components/NavModifierComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<AICore::NavModifierComponent>();        // nullptr if absent
Owner.AddComponent<AICore::NavModifierComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<AICore::NavModifierComponent>();
Owner.RemoveComponent<AICore::NavModifierComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Nav Modifier")      -- nil if absent
local c = entity:AddComponent("Nav Modifier")
entity:HasComponent("Nav Modifier")
entity:RemoveComponent("Nav Modifier")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetHalfExtents()` -> x, y, z<br>`c:SetHalfExtents(x, y, z)` | Property. |
| `c:GetCost()` -> number<br>`c:SetCost(number)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
