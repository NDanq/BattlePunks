# Post Process

Reference for the **Post Process** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Post Process` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::PostProcessComponent` |
| Header | `#include "Public/RenderCore/Components/PostProcessComponent.h"` |
| Size | 160 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `PostProcessComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Stack` | string | (empty) | asset: post process stack (.pp.nmgraph) |
| `Priority` | int32_t | 0 |  |
| `Blend Distance` | float | 0 |  |
| `Weight` | float | 1 |  |
| `Enabled` | uint8_t (flag) | true |  |

## C++ API

```cpp
#include "Public/RenderCore/Components/PostProcessComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::PostProcessComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::PostProcessComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::PostProcessComponent>();
Owner.RemoveComponent<RenderCore::PostProcessComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Post Process")      -- nil if absent
local c = entity:AddComponent("Post Process")
entity:HasComponent("Post Process")
entity:RemoveComponent("Post Process")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetStack()` -> string<br>`c:SetStack(string)` | Property. |
| `c:GetPriority()` -> integer<br>`c:SetPriority(integer)` | Property. |
| `c:GetBlendDistance()` -> number<br>`c:SetBlendDistance(number)` | Property. |
| `c:GetWeight()` -> number<br>`c:SetWeight(number)` | Property. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
