# Spot Light

Reference for the **Spot Light** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Spot Light` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::SpotLightComponent` |
| Header | `#include "Public/RenderCore/Components/SpotLightComponent.h"` |
| Size | 56 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `SpotLightComponent`.

### Light

| Property | Type | Default | Notes |
|---|---|---|---|
| `Color` | Color | (255, 255, 255, 255) |  |
| `Intensity` | float | 10 |  |
| `Range` | float | 1000 |  |
| `Inner Cone Angle` | float | 20 |  |
| `Outer Cone Angle` | float | 30 |  |
| `Active` | uint8_t (flag) | true |  |

### Shadows

| Property | Type | Default | Notes |
|---|---|---|---|
| `Cast Shadows` | uint8_t (flag) | true |  |
| `Technique` | uint8_t | 0 | choice: Standard, Modulated |
| `Modulated Tint` | Color | (40, 50, 80, 255) |  |
| `Depth Bias` | float | 0.002 |  |
| `Normal Bias` | float | 1.5 |  |

### Baked Lighting

| Property | Type | Default | Notes |
|---|---|---|---|
| `Source Radius` | float | 4 |  |

## C++ API

```cpp
#include "Public/RenderCore/Components/SpotLightComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::SpotLightComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::SpotLightComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::SpotLightComponent>();
Owner.RemoveComponent<RenderCore::SpotLightComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Spot Light")      -- nil if absent
local c = entity:AddComponent("Spot Light")
entity:HasComponent("Spot Light")
entity:RemoveComponent("Spot Light")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetColor()` -> r, g, b, a<br>`c:SetColor(r, g, b, a)` | Property. |
| `c:GetIntensity()` -> number<br>`c:SetIntensity(number)` | Property. |
| `c:GetRange()` -> number<br>`c:SetRange(number)` | Property. |
| `c:GetInnerConeAngle()` -> number<br>`c:SetInnerConeAngle(number)` | Property. |
| `c:GetOuterConeAngle()` -> number<br>`c:SetOuterConeAngle(number)` | Property. |
| `c:GetActive()` -> boolean<br>`c:SetActive(boolean)` | Property. |
| `c:GetCastShadows()` -> boolean<br>`c:SetCastShadows(boolean)` | Property. |
| `c:GetShadowTechnique()` -> integer<br>`c:SetShadowTechnique(integer)` | Property. |
| `c:GetShadowTint()` -> r, g, b, a<br>`c:SetShadowTint(r, g, b, a)` | Property. |
| `c:GetShadowBias()` -> number<br>`c:SetShadowBias(number)` | Property. |
| `c:GetShadowNormalBias()` -> number<br>`c:SetShadowNormalBias(number)` | Property. |
| `c:GetSourceRadius()` -> number<br>`c:SetSourceRadius(number)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
