# Scene Capture Probe

Reference for the **Scene Capture Probe** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Scene Capture Probe` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::SceneCaptureProbeComponent` |
| Header | `#include "Public/RenderCore/Components/SceneCaptureProbeComponent.h"` |
| Size | 184 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `SceneCaptureProbeComponent`.

### Capture

| Property | Type | Default | Notes |
|---|---|---|---|
| `Target` | string | (empty) | asset: render target (.nmrt2d, .nmrt3d) |
| `Mode` | uint8_t | 1 | choice: Planar Mirror, Omnidirectional |
| `Resolution` | int32_t | 256 |  |
| `Frequency` | uint8_t | 0 | choice: Every Frame, Interval, Once, Manual |
| `Interval` | float | 1 |  |
| `Capture Now` | uint8_t (flag) | false |  |
| `Capture Shadows` | uint8_t (flag) | false |  |

### Extent

| Property | Type | Default | Notes |
|---|---|---|---|
| `Near Clip` | float | 5 |  |
| `Far Clip` | float | 5000 |  |
| `Extent X` | float | 200 |  |
| `Extent Z` | float | 200 |  |
| `Hide Owner` | uint8_t (flag) | true |  |
| `Enabled` | uint8_t (flag) | true |  |

## C++ API

```cpp
#include "Public/RenderCore/Components/SceneCaptureProbeComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::SceneCaptureProbeComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::SceneCaptureProbeComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::SceneCaptureProbeComponent>();
Owner.RemoveComponent<RenderCore::SceneCaptureProbeComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Scene Capture Probe")      -- nil if absent
local c = entity:AddComponent("Scene Capture Probe")
entity:HasComponent("Scene Capture Probe")
entity:RemoveComponent("Scene Capture Probe")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetTarget()` -> string<br>`c:SetTarget(string)` | Property. |
| `c:GetMode()` -> integer<br>`c:SetMode(integer)` | Property. |
| `c:GetResolution()` -> integer<br>`c:SetResolution(integer)` | Property. |
| `c:GetFrequency()` -> integer<br>`c:SetFrequency(integer)` | Property. |
| `c:GetInterval()` -> number<br>`c:SetInterval(number)` | Property. |
| `c:GetNearClip()` -> number<br>`c:SetNearClip(number)` | Property. |
| `c:GetFarClip()` -> number<br>`c:SetFarClip(number)` | Property. |
| `c:GetExtentX()` -> number<br>`c:SetExtentX(number)` | Property. |
| `c:GetExtentZ()` -> number<br>`c:SetExtentZ(number)` | Property. |
| `c:GetHideOwner()` -> boolean<br>`c:SetHideOwner(boolean)` | Property. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |
| `c:GetCaptureNow()` -> boolean<br>`c:SetCaptureNow(boolean)` | Property. |
| `c:GetCaptureShadows()` -> boolean<br>`c:SetCaptureShadows(boolean)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
