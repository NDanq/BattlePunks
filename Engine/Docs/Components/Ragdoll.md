# Ragdoll

Reference for the **Ragdoll** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Ragdoll` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `NimbleEngine::RagdollComponent` |
| Header | `#include "Public/NimbleEngine/Ragdoll.h"` |
| Size | 192 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `RagdollComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Enabled` | uint8_t (flag) | true |  |
| `Hit Boxes` | uint8_t (flag) | true |  |
| `Start Active` | uint8_t (flag) | false |  |
| `Drop Props` | uint8_t (flag) | true |  |

### Body

| Property | Type | Default | Notes |
|---|---|---|---|
| `Mass` | float | 70 |  |
| `Thickness` | float | 1 |  |
| `Joint Friction` | float | 1 |  |

### Material

| Property | Type | Default | Notes |
|---|---|---|---|
| `Friction` | float | 0.8 |  |
| `Linear Damping` | float | 0.05 |  |
| `Angular Damping` | float | 0.5 |  |

### Filtering

| Property | Type | Default | Notes |
|---|---|---|---|
| `Layer` | int32_t | 1 |  |
| `Mask` | int32_t | -1 |  |

### State

| Property | Type | Default | Notes |
|---|---|---|---|
| `Active` | uint8_t (flag) | false | read-only |
| `Settled` | uint8_t (flag) | false | read-only |

## C++ API

```cpp
#include "Public/NimbleEngine/Ragdoll.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<NimbleEngine::RagdollComponent>();        // nullptr if absent
Owner.AddComponent<NimbleEngine::RagdollComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<NimbleEngine::RagdollComponent>();
Owner.RemoveComponent<NimbleEngine::RagdollComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Ragdoll")      -- nil if absent
local c = entity:AddComponent("Ragdoll")
entity:HasComponent("Ragdoll")
entity:RemoveComponent("Ragdoll")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |
| `c:GetHitBoxes()` -> boolean<br>`c:SetHitBoxes(boolean)` | Property. |
| `c:GetDropProps()` -> boolean<br>`c:SetDropProps(boolean)` | Property. |
| `c:GetMass()` -> number<br>`c:SetMass(number)` | Property. |
| `c:GetActive()` -> integer | Read-only property. |
| `c:GetSettled()` -> integer | Read-only property. |
| `c:Activate()` | Method. |
| `c:Reset()` | Method. |
| `c:IsActive()` -> Active:bool *(no effects)* | Method. |
| `c:IsSettled()` -> Settled:bool *(no effects)* | Method. |
| `c:AddImpulse(Impulse:Vector3, At:Vector3)` | Method. |
| `c:GetPartAt(At:Vector3)` -> Part:string | Method. |
| `c:ActivateWithImpulse(Impulse:Vector3, At:Vector3)` | Method. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
