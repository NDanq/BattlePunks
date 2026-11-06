# Rigid Body

Reference for the **Rigid Body** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Rigid Body` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `GameCore::RigidBodyComponent` |
| Header | `#include "Public/GameCore/Components/RigidBodyComponent.h"` |
| Size | 240 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `RigidBodyComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Enabled` | uint8_t (flag) | true |  |
| `Motion` | uint8_t | 0 | choice: Dynamic, Kinematic, Static |

### Shape

| Property | Type | Default | Notes |
|---|---|---|---|
| `Shape` | uint8_t | 0 | choice: Auto, Box, Sphere, Capsule, Cylinder, Convex hull, Triangle mesh, Height field |
| `Shape Source` | uint8_t | 0 | choice: Auto, Bounds component, Render component mesh |

### Material

| Property | Type | Default | Notes |
|---|---|---|---|
| `Mass` | float | 0 |  |
| `Friction` | float | 0.5 |  |
| `Restitution` | float | 0 |  |
| `Linear Damping` | float | 0.05 |  |
| `Angular Damping` | float | 0.05 |  |
| `Gravity Factor` | float | 1 |  |

### Simulation

| Property | Type | Default | Notes |
|---|---|---|---|
| `Continuous` | uint8_t (flag) | false |  |
| `Sensor` | uint8_t (flag) | false |  |
| `Allow Sleeping` | uint8_t (flag) | true |  |
| `Initial Linear Velocity` | Vector3 | (0, 0, 0) |  |
| `Initial Angular Velocity` | Vector3 | (0, 0, 0) |  |

### Filtering

| Property | Type | Default | Notes |
|---|---|---|---|
| `Layer` | int32_t | 1 |  |
| `Mask` | int32_t | -1 |  |

### State

| Property | Type | Default | Notes |
|---|---|---|---|
| `Linear Velocity` | Vector3 | (0, 0, 0) | read-only |
| `Angular Velocity` | Vector3 | (0, 0, 0) | read-only |

## C++ API

```cpp
#include "Public/GameCore/Components/RigidBodyComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<GameCore::RigidBodyComponent>();        // nullptr if absent
Owner.AddComponent<GameCore::RigidBodyComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<GameCore::RigidBodyComponent>();
Owner.RemoveComponent<GameCore::RigidBodyComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Rigid Body")      -- nil if absent
local c = entity:AddComponent("Rigid Body")
entity:HasComponent("Rigid Body")
entity:RemoveComponent("Rigid Body")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |
| `c:GetMass()` -> number<br>`c:SetMass(number)` | Property. |
| `c:GetFriction()` -> number<br>`c:SetFriction(number)` | Property. |
| `c:GetRestitution()` -> number<br>`c:SetRestitution(number)` | Property. |
| `c:GetGravityFactor()` -> number<br>`c:SetGravityFactor(number)` | Property. |
| `c:GetLayer()` -> integer<br>`c:SetLayer(integer)` | Property. |
| `c:GetMask()` -> integer<br>`c:SetMask(integer)` | Property. |
| `c:GetSensor()` -> boolean<br>`c:SetSensor(boolean)` | Property. |
| `c:GetAwake()` -> integer | Read-only property. |
| `c:GetLinearVelocity()` -> Velocity:Vector3 *(no effects)* | Method. |
| `c:GetAngularVelocity()` -> Velocity:Vector3 *(no effects)* | Method. |
| `c:SetLinearVelocity(Velocity:Vector3)` | Method. |
| `c:SetAngularVelocity(Velocity:Vector3)` | Method. |
| `c:AddImpulse(Impulse:Vector3)` | Method. |
| `c:AddForce(Force:Vector3)` | Method. |
| `c:AddAngularImpulse(Impulse:Vector3)` | Method. |
| `c:AddTorque(Torque:Vector3)` | Method. |
| `c:GetMotion()` -> Motion:string *(no effects)* | Method. |
| `c:SetMotion(Motion:string)` | Method. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
