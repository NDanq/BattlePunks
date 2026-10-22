# Rect Light

Reference for the **Rect Light** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Rect Light` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::RectLightComponent` |
| Header | `#include "Public/RenderCore/Components/RectLightComponent.h"` |
| Size | 40 bytes |
| Add Component | yes, under Rendering/Lights |

## What it does

A glowing rectangle centred on its entity, facing along the entity's forward (+Z): only its front lights. It is shaded by linearly transformed cosines (`Engine/Modules/RenderCore/GLOBAL_ILLUMINATION.md`, Phase 3), so highlights on smooth surfaces take the rectangle's shape and its light falls off with the solid angle it covers. It casts no shadow yet, and volumetric fog lights it as a point. With `[Rendering] AreaLights` off it lights as a point light at its centre.

## Properties

What the Details panel shows. The C++ members are public fields of `RectLightComponent`.

### Light

| Property | Type | Default | Notes |
|---|---|---|---|
| `Color` | Color | (255, 255, 255, 255) | The light's colour, display space |
| `Intensity` | float | 4 | Its surface's brightness: a larger rectangle lights more |
| `Width` | float | 200 | World units along the entity's +X (times its scale) |
| `Height` | float | 100 | World units along the entity's +Y (times its scale) |
| `Range` | float | 1500 | Its light is windowed to nothing here: where the light list stops counting it |
| `Active` | uint8_t (flag) | true |  |

## C++ API

```cpp
#include "Public/RenderCore/Components/RectLightComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::RectLightComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::RectLightComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::RectLightComponent>();
Owner.RemoveComponent<RenderCore::RectLightComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Rect Light")      -- nil if absent
local c = entity:AddComponent("Rect Light")
entity:HasComponent("Rect Light")
entity:RemoveComponent("Rect Light")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetColor()` -> r, g, b, a<br>`c:SetColor(r, g, b, a)` | Property. |
| `c:GetIntensity()` -> number<br>`c:SetIntensity(number)` | Property. |
| `c:GetWidth()` -> number<br>`c:SetWidth(number)` | Property. |
| `c:GetHeight()` -> number<br>`c:SetHeight(number)` | Property. |
| `c:GetRange()` -> number<br>`c:SetRange(number)` | Property. |
| `c:GetActive()` -> boolean<br>`c:SetActive(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
