# Audio Listener

Reference for the **Audio Listener** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Audio Listener` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `GameCore::AudioListenerComponent` |
| Header | `#include "Public/GameCore/Components/AudioListenerComponent.h"` |
| Size | 24 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `AudioListenerComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Active` | uint8_t (flag) | true |  |

## C++ API

```cpp
#include "Public/GameCore/Components/AudioListenerComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<GameCore::AudioListenerComponent>();        // nullptr if absent
Owner.AddComponent<GameCore::AudioListenerComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<GameCore::AudioListenerComponent>();
Owner.RemoveComponent<GameCore::AudioListenerComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Audio Listener")      -- nil if absent
local c = entity:AddComponent("Audio Listener")
entity:HasComponent("Audio Listener")
entity:RemoveComponent("Audio Listener")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetActive()` -> boolean<br>`c:SetActive(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
