# Vehicle

Reference for the **Vehicle** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Vehicle` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `GameCore::VehicleComponent` |
| Header | `#include "Public/GameCore/Components/VehicleComponent.h"` |
| Size | 144 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `VehicleComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Enabled` | uint8_t (flag) | true |  |

### Drivetrain

| Property | Type | Default | Notes |
|---|---|---|---|
| `Drive` | uint8_t | 0 | choice: Rear wheels, Front wheels, All wheels |
| `Engine Torque` | float | 500 |  |
| `Max RPM` | float | 6000 |  |
| `Max Steer Angle` | float | 30 |  |
| `Brake Torque` | float | 1500 |  |
| `Hand Brake Torque` | float | 4000 |  |

### Suspension

| Property | Type | Default | Notes |
|---|---|---|---|
| `Min Length` | float | 10 |  |
| `Max Length` | float | 40 |  |
| `Frequency` | float | 1.5 |  |
| `Damping` | float | 0.5 |  |
| `Anti-Roll Stiffness` | float | 1000 |  |
| `Max Pitch/Roll Angle` | float | 80 |  |

### Wheels

| Property | Type | Default | Notes |
|---|---|---|---|
| `Wheel Radius` | float | 0 |  |
| `Wheel Width` | float | 0 |  |

### State

| Property | Type | Default | Notes |
|---|---|---|---|
| `Speed` | float | 0 | read-only |
| `Engine RPM` | float | 0 | read-only |
| `Gear` | int32_t | 0 | read-only |

## C++ API

```cpp
#include "Public/GameCore/Components/VehicleComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<GameCore::VehicleComponent>();        // nullptr if absent
Owner.AddComponent<GameCore::VehicleComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<GameCore::VehicleComponent>();
Owner.RemoveComponent<GameCore::VehicleComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Vehicle")      -- nil if absent
local c = entity:AddComponent("Vehicle")
entity:HasComponent("Vehicle")
entity:RemoveComponent("Vehicle")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetEnabled()` -> boolean<br>`c:SetEnabled(boolean)` | Property. |
| `c:GetForward()` -> number<br>`c:SetForward(number)` | Property. |
| `c:GetRight()` -> number<br>`c:SetRight(number)` | Property. |
| `c:GetBrake()` -> number<br>`c:SetBrake(number)` | Property. |
| `c:GetHandBrake()` -> number<br>`c:SetHandBrake(number)` | Property. |
| `c:GetEngineTorque()` -> number<br>`c:SetEngineTorque(number)` | Property. |
| `c:GetMaxSteerAngle()` -> number<br>`c:SetMaxSteerAngle(number)` | Property. |
| `c:GetSpeed()` -> number | Read-only property. |
| `c:GetEngineRPM()` -> number | Read-only property. |
| `c:GetGear()` -> integer | Read-only property. |
| `c:GetWheelsOnGround()` -> integer | Read-only property. |
| `c:SetInput(Forward:number, Right:number, Brake:number, HandBrake:number)` | Method. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
