# Animation

Reference for the **Animation** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Animation` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `RenderCore::AnimationComponent` |
| Header | `#include "Public/RenderCore/Components/AnimationComponent.h"` |
| Size | 584 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `AnimationComponent`.

### Animation

| Property | Type | Default | Notes |
|---|---|---|---|
| `Animation` | string | (empty) | asset: animation (.nmanim, .nmanimstate) |
| `Play Rate` | float | 1 |  |
| `Paused` | uint8_t (flag) | false |  |

### Parameters

| Property | Type | Default | Notes |
|---|---|---|---|
| `Parameters` | struct[12] | (12 elements) | array of 12 |
| `Parameters[].Name` | char[N] | |  |
| `Parameters[].Value` | float | |  |

## C++ API

```cpp
#include "Public/RenderCore/Components/AnimationComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<RenderCore::AnimationComponent>();        // nullptr if absent
Owner.AddComponent<RenderCore::AnimationComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<RenderCore::AnimationComponent>();
Owner.RemoveComponent<RenderCore::AnimationComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Animation")      -- nil if absent
local c = entity:AddComponent("Animation")
entity:HasComponent("Animation")
entity:RemoveComponent("Animation")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetAnimation()` -> string<br>`c:SetAnimation(string)` | Property. |
| `c:GetPlayRate()` -> number<br>`c:SetPlayRate(number)` | Property. |
| `c:GetPaused()` -> boolean<br>`c:SetPaused(boolean)` | Property. |
| `c:SetFloat(Name:string, Value:number)` -> Set:bool | Method. |
| `c:SetBool(Name:string, Value:bool)` -> Set:bool | Method. |
| `c:SetTrigger(Name:string)` -> Set:bool | Method. |
| `c:GetFloat(Name:string)` -> Value:number *(no effects)* | Method. |
| `c:GetBool(Name:string)` -> Value:bool *(no effects)* | Method. |
| `c:GetBoneLocation(Bone:string)` -> Location:Vector3, Found:bool *(no effects)* | Method. |
| `c:GetState(Layer:integer)` -> State:string *(no effects)* | Method. |
| `c:IsInTransition(Layer:integer)` -> Blending:bool *(no effects)* | Method. |
| `c:GetTime()` -> Seconds:number *(no effects)* | Method. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
