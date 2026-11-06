# AvatarHarness

Reference for the **AvatarHarness** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `AvatarHarness` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `NimbleEngine::AvatarHarnessComponent` |
| Header | `#include "Public/NimbleEngine/Avatar.h"` |
| Size | 280 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `AvatarHarnessComponent`.

### Avatar

| Property | Type | Default | Notes |
|---|---|---|---|
| `InputMap` | string | `Default` |  |
| `AutoPossess` | uint8_t | 0 | choice: Disabled, Player 1, Player 2, Player 3, Player 4 |
| `ViewTarget` | uint8_t (flag) | true |  |
| `Requestable` | uint8_t (flag) | false |  |
| `Enabled` | uint8_t (flag) | true |  |
| `Simulation` | string | (empty) |  |

## C++ API

```cpp
#include "Public/NimbleEngine/Avatar.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<NimbleEngine::AvatarHarnessComponent>();        // nullptr if absent
Owner.AddComponent<NimbleEngine::AvatarHarnessComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<NimbleEngine::AvatarHarnessComponent>();
Owner.RemoveComponent<NimbleEngine::AvatarHarnessComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("AvatarHarness")      -- nil if absent
local c = entity:AddComponent("AvatarHarness")
entity:HasComponent("AvatarHarness")
entity:RemoveComponent("AvatarHarness")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetInputMap()` -> string<br>`c:SetInputMap(string)` | Property. |
| `c:GetSimulation()` -> string<br>`c:SetSimulation(string)` | Property. |
| `c:GetAutoPossess()` -> integer<br>`c:SetAutoPossess(integer)` | Property. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |
| `c:GetViewTarget()` -> boolean<br>`c:SetViewTarget(boolean)` | Property. |
| `c:GetRequestable()` -> boolean<br>`c:SetRequestable(boolean)` | Property. |
| `c:IsDown(Action:string)` -> Down:bool *(no effects)* | Method. |
| `c:WasPressed(Action:string)` -> Pressed:bool *(no effects)* | Method. |
| `c:WasReleased(Action:string)` -> Released:bool *(no effects)* | Method. |
| `c:GetAxis(Axis:string)` -> Value:number *(no effects)* | Method. |
| `c:IsPossessed()` -> Possessed:bool *(no effects)* | Method. |
| `c:IsLocallyControlled()` -> Local:bool *(no effects)* | Method. |
| `c:GetPossessor()` -> Player:Entity *(no effects)* | Method. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
