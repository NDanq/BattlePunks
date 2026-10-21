# Destructible

Reference for the **Destructible** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

A Destructible breaks into Voronoi pieces cut the way its material breaks -- chunks, splinters, shards -- when physics impacts or scripts hit it hard enough. `Engine/Source/Public/NimbleEngine/Destruction.h` has the whole design; WorldsLab's DestructionLab level shows each material.

| | |
|---|---|
| Name | `Destructible` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `NimbleEngine::DestructibleComponent` |
| Header | `#include "Public/NimbleEngine/Destruction.h"` |
| Size | 272 bytes |
| Add Component | yes, under Physics |

## Properties

What the Details panel shows. The C++ members are public fields of `DestructibleComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Enabled` | uint8_t (flag) | true |  |

### Fracture

| Property | Type | Default | Notes |
|---|---|---|---|
| `Material` | string | `Concrete` | a preset: Concrete, Wood, Glass, Stone, Metal, or a project's `[Destruction_<Name>]` |
| `Mode` | uint8_t | 0 | choice: Runtime (cut when it breaks, around the hit), Baked (pieces from `Fracture`) |
| `Fracture` | string | (empty) | Baked: the `.nmfrac` the editor console's `destruction bake` wrote |
| `Pieces` | int32_t | 0 | 0: the material's |
| `Seed` | int32_t | 0 | 0: one from the entity, the same on every machine |

### Damage

| Property | Type | Default | Notes |
|---|---|---|---|
| `Strength` | float | 0 | kg * units / s in one hit to break it; 0: the material's |
| `Health` | float | 0 | kg * units / s of hits in all; 0: the material's |

### Structure

| Property | Type | Default | Notes |
|---|---|---|---|
| `Structural` | uint8_t (flag) | false | Static bodies only: breaks around each hit, the rest stands |
| `Anchored` | uint8_t (flag) | true | Structural: pieces on its base hold the rest up |

### Debris

| Property | Type | Default | Notes |
|---|---|---|---|
| `Lifetime` | float | 0 | seconds a fallen piece lives; 0: the material's |

### State

| Property | Type | Default | Notes |
|---|---|---|---|
| `Damage` | float | 0 | read-only |
| `Broken` | uint8_t (flag) | false | read-only |

## C++ API

```cpp
#include "Public/NimbleEngine/Destruction.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<NimbleEngine::DestructibleComponent>();        // nullptr if absent
Owner.AddComponent<NimbleEngine::DestructibleComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<NimbleEngine::DestructibleComponent>();
Owner.RemoveComponent<NimbleEngine::DestructibleComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Destructible")      -- nil if absent
local c = entity:AddComponent("Destructible")
entity:HasComponent("Destructible")
entity:RemoveComponent("Destructible")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |
| `c:GetMaterial()` -> string<br>`c:SetMaterial(string)` | Property. |
| `c:GetStrength()` -> number<br>`c:SetStrength(number)` | Property. |
| `c:GetHealth()` -> number<br>`c:SetHealth(number)` | Property. |
| `c:GetDamage()` -> number | Read-only property. |
| `c:GetBroken()` -> integer | Read-only property. |
| `c:IsBroken()` -> Broken:bool *(no effects)* | Method. |
| `c:Break(Point:Vector3, Impulse:Vector3)` | Method. |
| `c:ApplyDamage(Point:Vector3, Impulse:Vector3)` | Method. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
