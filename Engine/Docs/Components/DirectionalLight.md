# Directional Light

Reference for the **Directional Light** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Directional Light` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::DirectionalLightComponent` |
| Header | `#include "Public/RenderCore/Components/DirectionalLightComponent.h"` |
| Size | 56 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `DirectionalLightComponent`.

### Light

| Property | Type | Default | Notes |
|---|---|---|---|
| `Color` | Color | (255, 255, 255, 255) |  |
| `Intensity` | float | 3 |  |
| `Active` | uint8_t (flag) | true |  |

### Shadows

| Property | Type | Default | Notes |
|---|---|---|---|
| `Cast Shadows` | uint8_t (flag) | true |  |
| `Technique` | uint8_t | 2 | choice: Standard, Modulated, Cascaded |
| `Modulated Tint` | Color | (40, 50, 80, 255) |  |
| `Depth Bias` | float | 0.0015 |  |
| `Normal Bias` | float | 2 |  |
| `Shadow Distance` | float | 4000 |  |
| `Cascade Count` | int32_t | 4 |  |
| `Cascade Split Lambda` | float | 0.75 |  |

### Baked Lighting

| Property | Type | Default | Notes |
|---|---|---|---|
| `Source Angle` | float | 0.75 |  |

## C++ API

```cpp
#include "Public/RenderCore/Components/DirectionalLightComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::DirectionalLightComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::DirectionalLightComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::DirectionalLightComponent>();
Owner.RemoveComponent<RenderCore::DirectionalLightComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Directional Light")      -- nil if absent
local c = entity:AddComponent("Directional Light")
entity:HasComponent("Directional Light")
entity:RemoveComponent("Directional Light")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetColor()` -> r, g, b, a<br>`c:SetColor(r, g, b, a)` | Property. |
| `c:GetIntensity()` -> number<br>`c:SetIntensity(number)` | Property. |
| `c:GetActive()` -> boolean<br>`c:SetActive(boolean)` | Property. |
| `c:GetCastShadows()` -> boolean<br>`c:SetCastShadows(boolean)` | Property. |
| `c:GetShadowTechnique()` -> integer<br>`c:SetShadowTechnique(integer)` | Property. |
| `c:GetShadowTint()` -> r, g, b, a<br>`c:SetShadowTint(r, g, b, a)` | Property. |
| `c:GetShadowBias()` -> number<br>`c:SetShadowBias(number)` | Property. |
| `c:GetShadowNormalBias()` -> number<br>`c:SetShadowNormalBias(number)` | Property. |
| `c:GetShadowDistance()` -> number<br>`c:SetShadowDistance(number)` | Property. |
| `c:GetCascadeCount()` -> integer<br>`c:SetCascadeCount(integer)` | Property. |
| `c:GetCascadeSplitLambda()` -> number<br>`c:SetCascadeSplitLambda(number)` | Property. |
| `c:GetSourceAngle()` -> number<br>`c:SetSourceAngle(number)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
