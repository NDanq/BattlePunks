# Particle System

Reference for the **Particle System** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Particle System` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::ParticleSystemComponent` |
| Header | `#include "Public/RenderCore/Components/ParticleSystemComponent.h"` |
| Size | 192 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `ParticleSystemComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `System` | string | (empty) | asset: particle system (.nmparticle) |
| `Emitting` | uint8_t (flag) | true |  |
| `Tint` | Color | (255, 255, 255, 255) |  |
| `Spawn Rate Scale` | float | 1 |  |
| `Time Scale` | float | 1 |  |
| `Seed` | int32_t | 0 |  |

### Beams

| Property | Type | Default | Notes |
|---|---|---|---|
| `Use Beam Target` | uint8_t (flag) | false |  |
| `Beam Target` | Vector3 | (0, 0, 0) |  |

## C++ API

```cpp
#include "Public/RenderCore/Components/ParticleSystemComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::ParticleSystemComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::ParticleSystemComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::ParticleSystemComponent>();
Owner.RemoveComponent<RenderCore::ParticleSystemComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Particle System")      -- nil if absent
local c = entity:AddComponent("Particle System")
entity:HasComponent("Particle System")
entity:RemoveComponent("Particle System")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetSystem()` -> string<br>`c:SetSystem(string)` | Property. |
| `c:GetEmitting()` -> boolean<br>`c:SetEmitting(boolean)` | Property. |
| `c:GetTint()` -> r, g, b, a<br>`c:SetTint(r, g, b, a)` | Property. |
| `c:GetSpawnRateScale()` -> number<br>`c:SetSpawnRateScale(number)` | Property. |
| `c:GetTimeScale()` -> number<br>`c:SetTimeScale(number)` | Property. |
| `c:GetSeed()` -> integer<br>`c:SetSeed(integer)` | Property. |
| `c:GetUseBeamTarget()` -> boolean<br>`c:SetUseBeamTarget(boolean)` | Property. |
| `c:GetBeamTarget()` -> x, y, z<br>`c:SetBeamTarget(x, y, z)` | Property. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
