# Input

Reference for the **Input** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Input` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `NimbleEngine::InputComponent` |
| Header | `#include "Public/NimbleEngine/Input.h"` |
| Size | 128 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `InputComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Map` | string | `Default` |  |
| `PlayerIndex` | uint8_t | 0 |  |
| `Enabled` | uint8_t (flag) | true |  |

## C++ API

```cpp
#include "Public/NimbleEngine/Input.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<NimbleEngine::InputComponent>();        // nullptr if absent
Owner.AddComponent<NimbleEngine::InputComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<NimbleEngine::InputComponent>();
Owner.RemoveComponent<NimbleEngine::InputComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Input")      -- nil if absent
local c = entity:AddComponent("Input")
entity:HasComponent("Input")
entity:RemoveComponent("Input")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetMap()` -> string<br>`c:SetMap(string)` | Property. |
| `c:GetPlayerIndex()` -> integer<br>`c:SetPlayerIndex(integer)` | Property. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |
| `c:IsDown(Action:string)` -> Down:bool *(no effects)* | Method. |
| `c:WasPressed(Action:string)` -> Pressed:bool *(no effects)* | Method. |
| `c:WasReleased(Action:string)` -> Released:bool *(no effects)* | Method. |
| `c:GetAxis(Axis:string)` -> Value:number *(no effects)* | Method. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
