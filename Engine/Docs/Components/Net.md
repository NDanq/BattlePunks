# Net

Reference for the **Net** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Net` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `GameCore::NetComponent` |
| Header | `#include "Public/GameCore/Components/NetComponent.h"` |
| Size | 32 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `NetComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Replicate` | uint8_t (flag) | true |  |
| `Predicted` | uint8_t (flag) | false |  |

## C++ API

```cpp
#include "Public/GameCore/Components/NetComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<GameCore::NetComponent>();        // nullptr if absent
Owner.AddComponent<GameCore::NetComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<GameCore::NetComponent>();
Owner.RemoveComponent<GameCore::NetComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Net")      -- nil if absent
local c = entity:AddComponent("Net")
entity:HasComponent("Net")
entity:RemoveComponent("Net")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetNetId()` -> integer | Read-only property. |
| `c:GetOwnerClient()` -> integer | Read-only property. |
| `c:GetReplicate()` -> boolean<br>`c:SetReplicate(boolean)` | Property. |
| `c:GetPredicted()` -> boolean<br>`c:SetPredicted(boolean)` | Property. |
| `c:GetInputAck()` -> integer | Read-only property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
