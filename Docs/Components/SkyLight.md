# Sky Light

Reference for the **Sky Light** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Sky Light` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::SkyLightComponent` |
| Header | `#include "Public/RenderCore/Components/SkyLightComponent.h"` |
| Size | 176 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `SkyLightComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `SkyColor` | Color | (120, 130, 150, 255) |  |
| `GroundColor` | Color | (60, 55, 50, 255) |  |
| `Intensity` | float | 0.6 |  |
| `Active` | uint8_t (flag) | true |  |

### Environment

| Property | Type | Default | Notes |
|---|---|---|---|
| `Environment` | string | (empty) | asset: texture (.nmtexture) |
| `Environment Intensity` | float | 1 |  |
| `Environment Rotation` | float | 0 |  |
| `Draw Background` | uint8_t (flag) | true |  |

## C++ API

```cpp
#include "Public/RenderCore/Components/SkyLightComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::SkyLightComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::SkyLightComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::SkyLightComponent>();
Owner.RemoveComponent<RenderCore::SkyLightComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Sky Light")      -- nil if absent
local c = entity:AddComponent("Sky Light")
entity:HasComponent("Sky Light")
entity:RemoveComponent("Sky Light")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetSkyColor()` -> r, g, b, a<br>`c:SetSkyColor(r, g, b, a)` | Property. |
| `c:GetGroundColor()` -> r, g, b, a<br>`c:SetGroundColor(r, g, b, a)` | Property. |
| `c:GetIntensity()` -> number<br>`c:SetIntensity(number)` | Property. |
| `c:GetActive()` -> boolean<br>`c:SetActive(boolean)` | Property. |
| `c:GetEnvironment()` -> string<br>`c:SetEnvironment(string)` | Property. |
| `c:GetEnvironmentIntensity()` -> number<br>`c:SetEnvironmentIntensity(number)` | Property. |
| `c:GetEnvironmentRotation()` -> number<br>`c:SetEnvironmentRotation(number)` | Property. |
| `c:GetDrawBackground()` -> boolean<br>`c:SetDrawBackground(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
