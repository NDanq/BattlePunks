# PlayerStart

Reference for the **PlayerStart** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `PlayerStart` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `NimbleEngine::PlayerStartComponent` |
| Header | `#include "Public/NimbleEngine/PlayerStart.h"` |
| Size | 152 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `PlayerStartComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `AvatarPrefab` | string | (empty) | asset: prefab (.nmprefab) |
| `PlayerSlot` | uint8_t | 0 | choice: Any, Player 1, Player 2, Player 3, Player 4 |
| `Enabled` | uint8_t (flag) | true |  |

## C++ API

```cpp
#include "Public/NimbleEngine/PlayerStart.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<NimbleEngine::PlayerStartComponent>();        // nullptr if absent
Owner.AddComponent<NimbleEngine::PlayerStartComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<NimbleEngine::PlayerStartComponent>();
Owner.RemoveComponent<NimbleEngine::PlayerStartComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("PlayerStart")      -- nil if absent
local c = entity:AddComponent("PlayerStart")
entity:HasComponent("PlayerStart")
entity:RemoveComponent("PlayerStart")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetAvatarPrefab()` -> string<br>`c:SetAvatarPrefab(string)` | Property. |
| `c:GetPlayerSlot()` -> integer<br>`c:SetPlayerSlot(integer)` | Property. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
