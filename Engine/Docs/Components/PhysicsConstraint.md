# Physics Constraint

Reference for the **Physics Constraint** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Physics Constraint` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `GameCore::PhysicsConstraintComponent` |
| Header | `#include "Public/GameCore/Components/PhysicsConstraintComponent.h"` |
| Size | 160 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `PhysicsConstraintComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Enabled` | uint8_t (flag) | true |  |
| `Type` | uint8_t | 1 | choice: Fixed, Point, Hinge, Slider, Distance |
| `Target Name` | string | (empty) |  |

### Frame

| Property | Type | Default | Notes |
|---|---|---|---|
| `Pivot` | Vector3 | (0, 0, 0) |  |
| `Axis` | Vector3 | (0, 1, 0) |  |

### Limits

| Property | Type | Default | Notes |
|---|---|---|---|
| `Limit Min` | float | 0 |  |
| `Limit Max` | float | 0 |  |

### Motor

| Property | Type | Default | Notes |
|---|---|---|---|
| `Motor` | uint8_t (flag) | false |  |
| `Motor Speed` | float | 0 |  |

## C++ API

```cpp
#include "Public/GameCore/Components/PhysicsConstraintComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<GameCore::PhysicsConstraintComponent>();        // nullptr if absent
Owner.AddComponent<GameCore::PhysicsConstraintComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<GameCore::PhysicsConstraintComponent>();
Owner.RemoveComponent<GameCore::PhysicsConstraintComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Physics Constraint")      -- nil if absent
local c = entity:AddComponent("Physics Constraint")
entity:HasComponent("Physics Constraint")
entity:RemoveComponent("Physics Constraint")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |
| `c:GetTargetName()` -> string<br>`c:SetTargetName(string)` | Property. |
| `c:GetLimitMin()` -> number<br>`c:SetLimitMin(number)` | Property. |
| `c:GetLimitMax()` -> number<br>`c:SetLimitMax(number)` | Property. |
| `c:GetMotor()` -> boolean<br>`c:SetMotor(boolean)` | Property. |
| `c:GetMotorSpeed()` -> number<br>`c:SetMotorSpeed(number)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
