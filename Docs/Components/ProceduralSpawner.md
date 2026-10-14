# Procedural Spawner

Reference for the **Procedural Spawner** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Procedural Spawner` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::ProceduralSpawnerComponent` |
| Header | `#include "Public/RenderCore/Components/ProceduralSpawnerComponent.h"` |
| Size | 3808 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `ProceduralSpawnerComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Visible` | uint8_t (flag) | true |  |

### Source

| Property | Type | Default | Notes |
|---|---|---|---|
| `Source` | uint8_t | 2 | choice: Spline Path, Spline Area, Box, Mesh Surface |
| `Project` | uint8_t | 3 | choice: None, Terrain, Static Meshes, Terrain and Meshes |
| `Box Extent` | Vector3 | (1000, 500, 1000) |  |
| `Spacing` | float | 200 |  |
| `Jitter` | float | 1 |  |
| `Project Range` | float | 5000 |  |
| `Path Offset` | float | 0 |  |
| `Both Sides` | uint8_t (flag) | false |  |
| `Seed` | int32_t | 1 |  |

### Filters

| Property | Type | Default | Notes |
|---|---|---|---|
| `Density` | float | 1 |  |
| `Noise Scale` | float | 0 |  |
| `Noise Threshold` | float | 0.5 |  |
| `Max Slope` | float | 90 |  |
| `Min Height` | float | -1e+06 |  |
| `Max Height` | float | 1e+06 |  |
| `Terrain Layer` | uint8_t | 0 | choice: Any, Layer 0, Layer 1, Layer 2, Layer 3 |
| `Layer Threshold` | float | 0.5 |  |
| `Exclusions` | string | (8 elements) | array of 8 |

### Instances

| Property | Type | Default | Notes |
|---|---|---|---|
| `Meshes` | struct[8] | (8 elements) | array of 8 |
| `Meshes[].Mesh` | char[N] | | asset: mesh (.nmmesh) |
| `Meshes[].Far Mesh` | char[N] | | asset: mesh (.nmmesh) |
| `Meshes[].Material` | char[N] | | asset: material graph (.material.nmgraph) |
| `Meshes[].Weight` | float | |  |
| `Meshes[].Scale` | float | |  |
| `Scale Min` | float | 0.8 |  |
| `Scale Max` | float | 1.2 |  |
| `Align to Normal` | float | 0 |  |
| `Random Yaw` | uint8_t (flag) | true |  |
| `Align to Path` | uint8_t (flag) | false |  |
| `Sink` | float | 0 |  |
| `Color Variation` | float | 0.1 |  |

### Drawing

| Property | Type | Default | Notes |
|---|---|---|---|
| `Cast Shadows` | uint8_t (flag) | true |  |
| `Cell Size` | float | 4000 |  |
| `LOD Distance` | float | 0 |  |
| `LOD 1 Density` | float | 0.5 |  |
| `LOD 2 Density` | float | 0.25 |  |
| `Cull Distance` | float | 0 |  |
| `Max Instances` | int32_t | 200000 |  |

### State

| Property | Type | Default | Notes |
|---|---|---|---|
| `Instance Count` | int32_t | 0 | read-only |
| `Generate ms` | float | 0 | read-only |

## C++ API

```cpp
#include "Public/RenderCore/Components/ProceduralSpawnerComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::ProceduralSpawnerComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::ProceduralSpawnerComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::ProceduralSpawnerComponent>();
Owner.RemoveComponent<RenderCore::ProceduralSpawnerComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Procedural Spawner")      -- nil if absent
local c = entity:AddComponent("Procedural Spawner")
entity:HasComponent("Procedural Spawner")
entity:RemoveComponent("Procedural Spawner")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetVisible()` -> boolean<br>`c:SetVisible(boolean)` | Property. |
| `c:GetSource()` -> integer<br>`c:SetSource(integer)` | Property. |
| `c:GetProject()` -> integer<br>`c:SetProject(integer)` | Property. |
| `c:GetSeed()` -> integer<br>`c:SetSeed(integer)` | Property. |
| `c:GetBoxExtent()` -> x, y, z<br>`c:SetBoxExtent(x, y, z)` | Property. |
| `c:GetSpacing()` -> number<br>`c:SetSpacing(number)` | Property. |
| `c:GetJitter()` -> number<br>`c:SetJitter(number)` | Property. |
| `c:GetDensity()` -> number<br>`c:SetDensity(number)` | Property. |
| `c:GetNoiseScale()` -> number<br>`c:SetNoiseScale(number)` | Property. |
| `c:GetNoiseThreshold()` -> number<br>`c:SetNoiseThreshold(number)` | Property. |
| `c:GetMaxSlope()` -> number<br>`c:SetMaxSlope(number)` | Property. |
| `c:GetScaleMin()` -> number<br>`c:SetScaleMin(number)` | Property. |
| `c:GetScaleMax()` -> number<br>`c:SetScaleMax(number)` | Property. |
| `c:GetAlignToNormal()` -> number<br>`c:SetAlignToNormal(number)` | Property. |
| `c:GetCullDistance()` -> number<br>`c:SetCullDistance(number)` | Property. |
| `c:GetInstanceCount()` -> integer | Read-only property. |
| `c:Regenerate()` | Method. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
