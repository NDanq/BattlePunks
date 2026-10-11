# Camera

Reference for the **Camera** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Camera` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::CameraComponent` |
| Header | `#include "Public/RenderCore/Components/CameraComponent.h"` |
| Size | 32 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `CameraComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `ZNear` | float | -1 |  |
| `ZFar` | float | 1000 |  |
| `Active` | uint8_t (flag) | true |  |
| `Projection` | uint8_t | 0 | choice: Orthographic, Perspective |
| `FovY` | float | 60 |  |

## C++ API

```cpp
#include "Public/RenderCore/Components/CameraComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::CameraComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::CameraComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::CameraComponent>();
Owner.RemoveComponent<RenderCore::CameraComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Camera")      -- nil if absent
local c = entity:AddComponent("Camera")
entity:HasComponent("Camera")
entity:RemoveComponent("Camera")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetZNear()` -> number<br>`c:SetZNear(number)` | Property. |
| `c:GetZFar()` -> number<br>`c:SetZFar(number)` | Property. |
| `c:GetActive()` -> boolean<br>`c:SetActive(boolean)` | Property. |
| `c:GetProjection()` -> integer<br>`c:SetProjection(integer)` | Property. |
| `c:GetFovY()` -> number<br>`c:SetFovY(number)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
