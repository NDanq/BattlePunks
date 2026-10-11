# Anim Socket

Reference for the **Anim Socket** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Anim Socket` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::AnimSocketComponent` |
| Header | `#include "Public/RenderCore/Components/AnimSocketComponent.h"` |
| Size | 112 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `AnimSocketComponent`.

### Socket

| Property | Type | Default | Notes |
|---|---|---|---|
| `Bone` | string | (empty) |  |
| `Position Offset` | Vector3 | (0, 0, 0) |  |
| `Rotation Offset` | Vector3 | (0, 0, 0) |  |

## C++ API

```cpp
#include "Public/RenderCore/Components/AnimSocketComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::AnimSocketComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::AnimSocketComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::AnimSocketComponent>();
Owner.RemoveComponent<RenderCore::AnimSocketComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Anim Socket")      -- nil if absent
local c = entity:AddComponent("Anim Socket")
entity:HasComponent("Anim Socket")
entity:RemoveComponent("Anim Socket")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetBone()` -> string<br>`c:SetBone(string)` | Property. |
| `c:GetPositionOffset()` -> x, y, z<br>`c:SetPositionOffset(x, y, z)` | Property. |
| `c:GetRotationOffset()` -> x, y, z<br>`c:SetRotationOffset(x, y, z)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
