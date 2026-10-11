# Possessor

Reference for the **Possessor** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Possessor` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `NimbleEngine::PossessorComponent` |
| Header | `#include "Public/NimbleEngine/Avatar.h"` |
| Size | 32 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `PossessorComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `LocalPlayer` | uint8_t | 0 | choice: Player 1, Player 2, Player 3, Player 4 |

## C++ API

```cpp
#include "Public/NimbleEngine/Avatar.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<NimbleEngine::PossessorComponent>();        // nullptr if absent
Owner.AddComponent<NimbleEngine::PossessorComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<NimbleEngine::PossessorComponent>();
Owner.RemoveComponent<NimbleEngine::PossessorComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Possessor")      -- nil if absent
local c = entity:AddComponent("Possessor")
entity:HasComponent("Possessor")
entity:RemoveComponent("Possessor")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetLocalPlayer()` -> integer<br>`c:SetLocalPlayer(integer)` | Property. |
| `c:GetAvatar()` -> Avatar:Entity *(no effects)* | Method. |
| `c:Possess(Avatar:Entity)` -> Accepted:bool | Method. |
| `c:Unpossess()` -> Accepted:bool | Method. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
