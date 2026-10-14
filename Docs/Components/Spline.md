# Spline

Reference for the **Spline** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Spline` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `GameCore::SplineComponent` |
| Header | `#include "Public/GameCore/Components/SplineComponent.h"` |
| Size | 1056 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `SplineComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Type` | uint8_t | 0 | choice: Curve, Linear |
| `Closed` | uint8_t (flag) | false |  |
| `Width` | float | 0 |  |
| `Points` | Vector3 | (64 elements) | array of 64 |

## C++ API

```cpp
#include "Public/GameCore/Components/SplineComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<GameCore::SplineComponent>();        // nullptr if absent
Owner.AddComponent<GameCore::SplineComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<GameCore::SplineComponent>();
Owner.RemoveComponent<GameCore::SplineComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Spline")      -- nil if absent
local c = entity:AddComponent("Spline")
entity:HasComponent("Spline")
entity:RemoveComponent("Spline")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetClosed()` -> boolean<br>`c:SetClosed(boolean)` | Property. |
| `c:GetType()` -> integer<br>`c:SetType(integer)` | Property. |
| `c:GetWidth()` -> number<br>`c:SetWidth(number)` | Property. |
| `c:GetPointCount()` -> Count:integer *(no effects)* | Method. |
| `c:GetPoint(Index:integer)` -> Point:Vector3 *(no effects)* | Method. |
| `c:SetPoint(Index:integer, Point:Vector3)` -> Set:bool | Method. |
| `c:AddPoint(Point:Vector3)` -> Added:bool | Method. |
| `c:InsertPoint(Index:integer, Point:Vector3)` -> Inserted:bool | Method. |
| `c:RemovePoint(Index:integer)` -> Removed:bool | Method. |
| `c:ClearPoints()` | Method. |
| `c:GetLength()` -> Length:number *(no effects)* | Method. |
| `c:GetLocationAtDistance(Distance:number)` -> Location:Vector3 *(no effects)* | Method. |
| `c:GetDirectionAtDistance(Distance:number)` -> Direction:Vector3 *(no effects)* | Method. |
| `c:FindDistanceClosestTo(Location:Vector3)` -> Distance:number *(no effects)* | Method. |
| `c:IsInside(Location:Vector3)` -> Inside:bool *(no effects)* | Method. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
