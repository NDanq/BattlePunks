# Sequence Player

Reference for the **Sequence Player** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Sequence Player` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `Sequencer::SequencePlayerComponent` |
| Header | `#include "Public/Sequencer/SequencePlayerComponent.h"` |
| Size | 168 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `SequencePlayerComponent`.

### Sequence

| Property | Type | Default | Notes |
|---|---|---|---|
| `Sequence` | string | (empty) | asset: sequence (.nmseq) |
| `Play Rate` | float | 1 |  |
| `Auto Play` | uint8_t (flag) | true |  |
| `Loop` | uint8_t (flag) | false |  |
| `Restore State` | uint8_t (flag) | false |  |
| `Destroy When Finished` | uint8_t (flag) | false |  |
| `Cinematic` | uint8_t (flag) | false |  |
| `Skippable` | uint8_t (flag) | true |  |

## C++ API

```cpp
#include "Public/Sequencer/SequencePlayerComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<Sequencer::SequencePlayerComponent>();        // nullptr if absent
Owner.AddComponent<Sequencer::SequencePlayerComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<Sequencer::SequencePlayerComponent>();
Owner.RemoveComponent<Sequencer::SequencePlayerComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Sequence Player")      -- nil if absent
local c = entity:AddComponent("Sequence Player")
entity:HasComponent("Sequence Player")
entity:RemoveComponent("Sequence Player")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetSequence()` -> string<br>`c:SetSequence(string)` | Property. |
| `c:GetPlayRate()` -> number<br>`c:SetPlayRate(number)` | Property. |
| `c:GetAutoPlay()` -> boolean<br>`c:SetAutoPlay(boolean)` | Property. |
| `c:GetLoop()` -> boolean<br>`c:SetLoop(boolean)` | Property. |
| `c:GetRestoreState()` -> boolean<br>`c:SetRestoreState(boolean)` | Property. |
| `c:GetCinematic()` -> boolean<br>`c:SetCinematic(boolean)` | Property. |
| `c:GetSkippable()` -> boolean<br>`c:SetSkippable(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
