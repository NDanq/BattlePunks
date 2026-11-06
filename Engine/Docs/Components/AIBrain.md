# AI Brain

Reference for the **AI Brain** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `AI Brain` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `AICore::AIBrainComponent` |
| Header | `#include "Public/AICore/Components/AIBrainComponent.h"` |
| Size | 344 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `AIBrainComponent`.

### AI Brain

| Property | Type | Default | Notes |
|---|---|---|---|
| `Tree` | string | (empty) | asset: behavior tree (.nmbt); the behavior tree it runs; **Edit** opens it in the behavior tree editor |
| `Blackboard` | string | (empty) | starting values: `Key=Value;Key=Value`, each parsed by its key's type |
| `Tick Interval` | float | 0 | seconds between ticks; 0 every frame |
| `Auto Start` | uint8_t (flag) | true | starts with the level; else `AI.StartBrain` |

## C++ API

```cpp
#include "Public/AICore/Components/AIBrainComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<AICore::AIBrainComponent>();        // nullptr if absent
Owner.AddComponent<AICore::AIBrainComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<AICore::AIBrainComponent>();
Owner.RemoveComponent<AICore::AIBrainComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("AI Brain")      -- nil if absent
local c = entity:AddComponent("AI Brain")
entity:HasComponent("AI Brain")
entity:RemoveComponent("AI Brain")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetTree()` -> string<br>`c:SetTree(string)` | Property. |
| `c:GetTickInterval()` -> number<br>`c:SetTickInterval(number)` | Property. |
| `c:GetAutoStart()` -> boolean<br>`c:SetAutoStart(boolean)` | Property. |
| `c:GetRunning()` -> boolean | Read-only property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
