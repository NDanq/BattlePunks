# GI Volume

Reference for the **GI Volume** component: its C++ class and its Lua API. Written in the editor's component-doc format; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `GI Volume` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::GIVolumeComponent` |
| Header | `#include "Public/RenderCore/Components/GIVolumeComponent.h"` |
| Add Component | yes, under Rendering/Lights |

## What it does

Dynamic diffuse global illumination (`Engine/Modules/RenderCore/GLOBAL_ILLUMINATION.md`, Phase 6, after DDGI): a grid of irradiance probes filling a box round its entity. Its probes render the scene round them in six small views, a face at a time within the frame's capture budget (`[Rendering] CaptureBudgetMs`, shared with the reflection probes; `GLOBAL_ILLUMINATION.md`, Phase 7), lit by the live lights, the sky and the volume itself, so light bounces again with every round through the grid. Once a few rounds have passed with nothing changed the volume stops updating; a change to the lighting (a light turned on, the sun moving a degree) lights the probes again from what they saw, many a frame, and a change to the static geometry round it draws them again. The volume after the nearest fills in the background meanwhile, so it is ready when the camera gets there. Inside the box, Lit surfaces that are not baked take their diffuse ambient light from the eight probes round them, in place of the reflection probes', weighted so a probe behind a wall does not light through it. The volume starts lighting only once every probe has been rendered once.

It needs an HDR scene with image-based lighting. One volume lights the scene at a time: the enabled one nearest the camera. `[Rendering] DynamicGI` off turns every volume off.

## Properties

What the Details panel shows. The C++ members are public fields of `GIVolumeComponent`.

### GI Volume

| Property | Type | Default | Notes |
|---|---|---|---|
| `Extent` | Vector3 | (1000, 400, 1000) | Half sizes of the box, world units, round the entity's position (not rotated or scaled) |
| `Probe Spacing` | float | 200 | Roughly how far apart the probes are, world units; at most 32 x 8 x 32 probes, at least 2 along each axis |
| `Intensity` | float | 1 | Scales the volume's light |
| `Enabled` | uint8_t (flag) | true |  |

Changing the box or the spacing starts the probes over.

## C++ API

```cpp
#include "Public/RenderCore/Components/GIVolumeComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::GIVolumeComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::GIVolumeComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::GIVolumeComponent>();
Owner.RemoveComponent<RenderCore::GIVolumeComponent>();
```

The component is plain data (trivially copyable): set its public fields directly.

## Lua API

```lua
local c = entity:GetComponent("GI Volume")      -- nil if absent
local c = entity:AddComponent("GI Volume")
entity:HasComponent("GI Volume")
entity:RemoveComponent("GI Volume")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetExtent()` -> x, y, z<br>`c:SetExtent(x, y, z)` | Property. |
| `c:GetProbeSpacing()` -> number<br>`c:SetProbeSpacing(number)` | Property. |
| `c:GetIntensity()` -> number<br>`c:SetIntensity(number)` | Property. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
