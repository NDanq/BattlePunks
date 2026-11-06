# Reflection Probe

Reference for the **Reflection Probe** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Reflection Probe` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::ReflectionProbeComponent` |
| Header | `#include "Public/RenderCore/Components/ReflectionProbeComponent.h"` |
| Size | 80 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `ReflectionProbeComponent`.

### Influence

| Property | Type | Default | Notes |
|---|---|---|---|
| `Radius` | float | 1000 |  |
| `Box Extent` | Vector3 | (0, 0, 0) |  |
| `Intensity` | float | 1 |  |

### Capture

| Property | Type | Default | Notes |
|---|---|---|---|
| `Resolution` | int32_t | 128 |  |
| `Frequency` | uint8_t | 2 | choice: Every Frame, Interval, Once, Manual |
| `Interval` | float | 1 |  |
| `Near Clip` | float | 5 |  |
| `Far Clip` | float | 20000 |  |
| `Capture Shadows` | uint8_t (flag) | true |  |
| `Capture Now` | uint8_t (flag) | false |  |
| `Enabled` | uint8_t (flag) | true |  |

## Capturing (GLOBAL_ILLUMINATION.md, Phase 7)

- A capture is six faces, rendered within the frame's capture budget (`[Rendering] CaptureBudgetMs`, shared with the GI Volumes, 2 ms on High): a few faces a frame, the probe's old capture lighting until the sixth is in.
- With `[Rendering] StaticCaptures` (on), Every Frame and Interval probes capture again only when what they would see has changed: a static mesh edited, a dynamic one moved within Far Clip, the lighting changed (the sun by `CaptureSunStepDegrees`, 1 degree; a light's colour, intensity or place; the sky). Every probe but a Manual one also captures again when the lighting changes. Capture Now always captures.
- Capture Shadows uses the frame's shared capture shadow maps (one set for every capture view, kept from frame to frame while nothing changes), covering the probe's influence and half as much again round it.
- With `[Rendering] SimpleCaptures` (on) the faces leave out translucent surfaces, particles and custom passes, and anything under `CaptureCullPixels` (2) pixels of the face.

## C++ API

```cpp
#include "Public/RenderCore/Components/ReflectionProbeComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::ReflectionProbeComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::ReflectionProbeComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::ReflectionProbeComponent>();
Owner.RemoveComponent<RenderCore::ReflectionProbeComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Reflection Probe")      -- nil if absent
local c = entity:AddComponent("Reflection Probe")
entity:HasComponent("Reflection Probe")
entity:RemoveComponent("Reflection Probe")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetRadius()` -> number<br>`c:SetRadius(number)` | Property. |
| `c:GetBoxExtent()` -> x, y, z<br>`c:SetBoxExtent(x, y, z)` | Property. |
| `c:GetIntensity()` -> number<br>`c:SetIntensity(number)` | Property. |
| `c:GetResolution()` -> integer<br>`c:SetResolution(integer)` | Property. |
| `c:GetFrequency()` -> integer<br>`c:SetFrequency(integer)` | Property. |
| `c:GetInterval()` -> number<br>`c:SetInterval(number)` | Property. |
| `c:GetNearClip()` -> number<br>`c:SetNearClip(number)` | Property. |
| `c:GetFarClip()` -> number<br>`c:SetFarClip(number)` | Property. |
| `c:GetCaptureShadows()` -> boolean<br>`c:SetCaptureShadows(boolean)` | Property. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |
| `c:GetCaptureNow()` -> boolean<br>`c:SetCaptureNow(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
