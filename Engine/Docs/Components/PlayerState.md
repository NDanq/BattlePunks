# PlayerState

Reference for the **PlayerState** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `PlayerState` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `NimbleEngine::PlayerStateComponent` |
| Header | `#include "Public/NimbleEngine/PlayerState.h"` |
| Size | 64 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `PlayerStateComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `PlayerName` | string | `Player` |  |
| `PlayerIndex` | uint8_t | 0 |  |
| `Score` | int32_t | 0 |  |
| `Lives` | int32_t | 0 |  |
| `Active` | uint8_t (flag) | true |  |

## C++ API

```cpp
#include "Public/NimbleEngine/PlayerState.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<NimbleEngine::PlayerStateComponent>();        // nullptr if absent
Owner.AddComponent<NimbleEngine::PlayerStateComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<NimbleEngine::PlayerStateComponent>();
Owner.RemoveComponent<NimbleEngine::PlayerStateComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("PlayerState")      -- nil if absent
local c = entity:AddComponent("PlayerState")
entity:HasComponent("PlayerState")
entity:RemoveComponent("PlayerState")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetPlayerName()` -> string<br>`c:SetPlayerName(string)` | Property. |
| `c:GetPlayerIndex()` -> integer | Read-only property. |
| `c:GetScore()` -> integer | Read-only property. |
| `c:GetLives()` -> integer | Read-only property. |
| `c:GetActive()` -> boolean<br>`c:SetActive(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
