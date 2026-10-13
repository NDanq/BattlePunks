# Nav Agent

Reference for the **Nav Agent** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Nav Agent` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `AICore::NavAgentComponent` |
| Header | `#include "Public/AICore/Components/NavAgentComponent.h"` |
| Size | 128 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `NavAgentComponent`.

### Nav Agent

| Property | Type | Default | Notes |
|---|---|---|---|
| `Radius` | float | 35 | cm; the crowd keeps agents this far apart |
| `Height` | float | 180 | cm |
| `Max Speed` | float | 400 | cm/s |
| `Max Acceleration` | float | 2000 | cm/s² |
| `Acceptance Radius` | float | 60 | cm from the target that counts as arrived |
| `Separation Weight` | float | 2 | how hard it keeps clear of neighbours |
| `Avoidance` | uint8_t | 2 | choice: None, Low, Medium, High; DetourCrowd's obstacle-avoidance quality; higher costs more |
| `Drive Transform` | uint8_t (flag) | false | on: the agent moves its own entity; off: a character controller does, fed by `SteerInput` |
| `Mesh Yaw` | float | 0 | degrees added to its facing; 180 for a mesh modelled facing −Z |
| `Turn Rate` | float | 540 | degrees a second, turning a driven agent |

## C++ API

```cpp
#include "Public/AICore/Components/NavAgentComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<AICore::NavAgentComponent>();        // nullptr if absent
Owner.AddComponent<AICore::NavAgentComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<AICore::NavAgentComponent>();
Owner.RemoveComponent<AICore::NavAgentComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Nav Agent")      -- nil if absent
local c = entity:AddComponent("Nav Agent")
entity:HasComponent("Nav Agent")
entity:RemoveComponent("Nav Agent")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetMaxSpeed()` -> number<br>`c:SetMaxSpeed(number)` | Property. |
| `c:GetAcceptanceRadius()` -> number<br>`c:SetAcceptanceRadius(number)` | Property. |
| `c:GetDriveTransform()` -> boolean<br>`c:SetDriveTransform(boolean)` | Property. |
| `c:GetStatus()` -> integer | Read-only property. |
| `c:GetDesiredVelocity()` -> x, y, z | Read-only property. |
| `c:GetTarget()` -> x, y, z | Read-only property. |
| `c:GetRemainingDistance()` -> number | Read-only property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
