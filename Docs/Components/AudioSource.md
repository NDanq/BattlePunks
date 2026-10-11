# Audio Source

Reference for the **Audio Source** component: its C++ class and its Lua API. Written by the editor from the component's registration; edit it freely, it is not overwritten.

| | |
|---|---|
| Name | `Audio Source` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |
| C++ class | `GameCore::AudioSourceComponent` |
| Header | `#include "Public/GameCore/Components/AudioSourceComponent.h"` |
| Size | 208 bytes |
| Add Component | yes |

## Properties

What the Details panel shows. The C++ members are public fields of `AudioSourceComponent`.

| Property | Type | Default | Notes |
|---|---|---|---|
| `Sound` | string | (empty) | asset: sound (.nmsound) |
| `Volume` | float | 1 |  |
| `Pitch` | float | 1 |  |
| `Auto Play` | uint8_t (flag) | true |  |
| `Force Bus` | uint8_t | 0 | choice: The Sound's, Master, Music, SFX, UI, Voice, Ambient |
| `Force 2D / 3D` | uint8_t | 0 | choice: The Sound's, 2D, 3D |

### Override

| Property | Type | Default | Notes |
|---|---|---|---|
| `Override` | uint8_t (flag) | false |  |
| `Loop` | uint8_t (flag) | false |  |
| `Spatial` | uint8_t (flag) | true |  |
| `Bus` | uint8_t | 2 | choice: Master, Music, SFX, UI, Voice, Ambient |
| `Min Distance` | float | 100 |  |
| `Max Distance` | float | 5000 |  |
| `Rolloff` | float | 1 |  |
| `Doppler Factor` | float | 1 |  |

### State

| Property | Type | Default | Notes |
|---|---|---|---|
| `Playing` | uint8_t (flag) | false | read-only |
| `Playback Seconds` | float | 0 | read-only |
| `Length Seconds` | float | 0 | read-only |

## C++ API

```cpp
#include "Public/GameCore/Components/AudioSourceComponent.h"

GameCore::Entity Owner = World.GetEntity(Id);

auto* C = Owner.GetComponent<GameCore::AudioSourceComponent>();        // nullptr if absent
Owner.AddComponent<GameCore::AudioSourceComponent>();                 // defaults; or pass an initialised one
Owner.HasComponent<GameCore::AudioSourceComponent>();
Owner.RemoveComponent<GameCore::AudioSourceComponent>();
```

The component is plain data (trivially copyable): set its public fields directly. Its methods, and any field not listed above, are in the header.

## Lua API

```lua
local c = entity:GetComponent("Audio Source")      -- nil if absent
local c = entity:AddComponent("Audio Source")
entity:HasComponent("Audio Source")
entity:RemoveComponent("Audio Source")
```

`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error.

| Call | Description |
|---|---|
| `c:GetEntity()` -> Entity | The owner. |
| `c:GetSound()` -> string<br>`c:SetSound(string)` | Property. |
| `c:GetVolume()` -> number<br>`c:SetVolume(number)` | Property. |
| `c:GetPitch()` -> number<br>`c:SetPitch(number)` | Property. |
| `c:GetAutoPlay()` -> boolean<br>`c:SetAutoPlay(boolean)` | Property. |
| `c:GetOverride()` -> boolean<br>`c:SetOverride(boolean)` | Property. |
| `c:GetLoop()` -> boolean<br>`c:SetLoop(boolean)` | Property. |
| `c:GetSpatial()` -> boolean<br>`c:SetSpatial(boolean)` | Property. |
| `c:GetMinDistance()` -> number<br>`c:SetMinDistance(number)` | Property. |
| `c:GetMaxDistance()` -> number<br>`c:SetMaxDistance(number)` | Property. |
| `c:GetRolloff()` -> number<br>`c:SetRolloff(number)` | Property. |
| `c:GetDopplerFactor()` -> number<br>`c:SetDopplerFactor(number)` | Property. |
| `c:GetForceBus()` -> integer<br>`c:SetForceBus(integer)` | Property. |
| `c:GetForceSpatial()` -> integer<br>`c:SetForceSpatial(integer)` | Property. |
| `c:Play()` | Method. |
| `c:Stop()` | Method. |
| `c:Pause()` | Method. |
| `c:Resume()` | Method. |
| `c:FadeTo(Gain:number, Seconds:number)` | Method. |
| `c:Seek(Seconds:number)` | Method. |
| `c:IsPlaying()` -> Playing:bool *(no effects)* | Method. |
| `c:IsPaused()` -> Paused:bool *(no effects)* | Method. |
| `c:GetPlaybackSeconds()` -> Seconds:number *(no effects)* | Method. |
| `c:GetLengthSeconds()` -> Seconds:number *(no effects)* | Method. |
| `c:GetBus()` -> Bus:string *(no effects)* | Method. |
| `c:SetBus(Bus:string)` | Method. |

Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value.
