# AI Stimulus

Reference for the **AI Stimulus** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `AI Stimulus` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `AICore::AIStimulusComponent` |
| Header | `#include "Public/AICore/Components/PerceptionComponent.h"` |
| Size | 24 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `AIStimulusComponent`.

### AI Stimulus

| Property | Type | Default | Notes |
|---|---|---|---|
| `Team` | uint8_t | 1 | 0 is neutral: nobody's enemy |
| `Enabled` | uint8_t (flag) | true | off: unseen and unheard |
| `Target Height` | float | 110 | cm above the origin where eyes aim (the chest) |

## C++ API

```cpp
#include "Public/AICore/Components/PerceptionComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<AICore::AIStimulusComponent>();        // nullptr if absent
Owner.AddComponent<AICore::AIStimulusComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<AICore::AIStimulusComponent>();
Owner.RemoveComponent<AICore::AIStimulusComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("AI Stimulus")      -- nil if absent
local c = entity:AddComponent("AI Stimulus")
entity:HasComponent("AI Stimulus")
entity:RemoveComponent("AI Stimulus")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetTeam()` -> integer<br>`c:SetTeam(integer)` | Property. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
