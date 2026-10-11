# Transform

Reference for the **Transform** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Transform` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `GameCore::TransformComponent` |
| Header | `#include "Public/GameCore/Components/TransformComponent.h"` |
| Size | 64 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `TransformComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Position` | Vector3 | (0, 0, 0) |  |
| `Rotation` | Vector3 | (0, 0, 0) |  |
| `Scale` | Vector3 | (1, 1, 1) |  |

## C++ API

```cpp
#include "Public/GameCore/Components/TransformComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<GameCore::TransformComponent>();        // nullptr if absent
Owner.AddComponent<GameCore::TransformComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<GameCore::TransformComponent>();
Owner.RemoveComponent<GameCore::TransformComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Transform")      -- nil if absent
local c = entity:AddComponent("Transform")
entity:HasComponent("Transform")
entity:RemoveComponent("Transform")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetPosition()` -> x, y, z<br>`c:SetPosition(x, y, z)` | Property. |
| `c:GetRotation()` -> x, y, z<br>`c:SetRotation(x, y, z)` | Property. |
| `c:GetScale()` -> x, y, z<br>`c:SetScale(x, y, z)` | Property. |
| `c:Translate(Delta:Vector3)` | Method. |
| `c:GetWorldPosition()` -> Position:Vector3 *(no effects)* | Method. |
| `c:SetWorldPosition(Position:Vector3)` | Method. |
| `c:GetWorldRotation()` -> Rotation:Vector3 *(no effects)* | Method. |
| `c:SetWorldRotation(Rotation:Vector3)` | Method. |
| `c:GetWorldScale()` -> Scale:Vector3 *(no effects)* | Method. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
