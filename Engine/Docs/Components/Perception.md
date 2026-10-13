# Perception

Reference for the **Perception** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Perception` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `AICore::PerceptionComponent` |
| Header | `#include "Public/AICore/Components/PerceptionComponent.h"` |
| Size | 48 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `PerceptionComponent`.

### Perception

| Property | Type | Default | Notes |
|---|---|---|---|
| `Sight Range` | float | 3000 | cm |
| `FOV Half Angle` | float | 70 | degrees either side of facing |
| `Eye Height` | float | 160 | cm above the entity's origin: where sight rays start |
| `Hearing Range` | float | 2500 | cm, scaled by a noise's loudness |
| `Forget Seconds` | float | 6 | how long what is no longer perceived is remembered |
| `Sight Interval` | float | 0.2 | seconds between one agent's sight checks; staggered across agents |
| `Team` | uint8_t | 1 | 0 is neutral; perceives the AI Stimuli of other non-zero teams |
| `Enabled` | uint8_t (flag) | true |  |

## C++ API

```cpp
#include "Public/AICore/Components/PerceptionComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<AICore::PerceptionComponent>();        // nullptr if absent
Owner.AddComponent<AICore::PerceptionComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<AICore::PerceptionComponent>();
Owner.RemoveComponent<AICore::PerceptionComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Perception")      -- nil if absent
local c = entity:AddComponent("Perception")
entity:HasComponent("Perception")
entity:RemoveComponent("Perception")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetSightRange()` -> number<br>`c:SetSightRange(number)` | Property. |
| `c:GetHearingRange()` -> number<br>`c:SetHearingRange(number)` | Property. |
| `c:GetTeam()` -> integer<br>`c:SetTeam(integer)` | Property. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
