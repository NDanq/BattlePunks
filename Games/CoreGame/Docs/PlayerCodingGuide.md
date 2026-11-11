# BattlePunks Player — Coding Guide (Weapons, Inventory, Cosmetics)

This is a hands-on guide for you to implement yourself. It follows the design we agreed on:
- build on `Games/CoreGame`
- networked PvP, with the server as the authority
- skinned and rigid cosmetics
- an engine clip-override map for weapon anims

Every API named below exists in this repo. Paths are listed so you can read the real thing before you write your version. **DemoLab is your reference implementation.** Most steps say "port X from DemoLab", so keep `Games/DemoLab/Source/Weapons/LabCombat.cpp` open the whole time.

---

## 0. Ground rules for this engine (read first)

| Rule | Why it matters to you |
|---|---|
| Components must be **trivially copyable** | No `std::string` or `std::vector`. Use `char Path[96]`, fixed arrays and `uint8_t` enums. |
| Replication can't send arrays (except `char[]`) | Inventory slots get unrolled into `Weapon0`, `Weapon1`, …, with `WeaponAt(i)` helpers. See `LabInventoryComponent` (`LabCombat.h:29`). |
| At most 64 replicated fields per component | Split state across components (Inventory, Combat, Cosmetics). |
| Adding or removing component type T **invalidates every `T*`** | After spawning anything, re-fetch with `GetComponent<T>()`. DemoLab does this everywhere, e.g. `LabCombat.cpp:1514`. |
| Only the inventory replicates | Weapon and cosmetic *entities* are spawned locally by every process from replicated ids. They have no Net component. |
| Authority does all logic | Clients send `Server*` RPCs. Logic lives in free functions (`BPCombat::Fire(World&, …)`) so a listen host or bot can call them directly. |

Component boilerplate pattern (copy it from `LabCombat.cpp:333-380`):
```cpp
GAMECORE_DEFINE_COMPONENT(BPFooComponent, "BPFoo")
GAMECORE_DEFINE_COMPONENT_SERIALIZER(BPFooComponent, 1) { Ar.Field("X", C.X); }
#if WITH_NETCORE
GAMECORE_DEFINE_COMPONENT_REPLICATION(BPFooComponent) { Rep.Field("X", &BPFooComponent::X); }
GAMECORE_DEFINE_COMPONENT_RPCS(BPFooComponent) { Rpc.Server<&BPFooComponent::ServerDo>("ServerDo").RateLimit(4); }
#endif
#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(BPFooComponent) { Props.Field("X", &BPFooComponent::X); }
#endif
```
Call RPCs with `GAMECORE_RPC(BPFooComponent, ServerDo)(World.GetEntity(Id), args...)`.

---

## Step 1 — Project setup

1. **Track the game.** In the root `.gitignore`, add `!/Games/CoreGame/` below `/Games/*`. Also ignore `Games/CoreGame/{Build,Binaries,cmake-build-*}/`.
2. **Enable Jolt.** In `Games/CoreGame/CMakePresets.json`, set `NIMBLE_WITH_JOLTPHYSICS=ON` in each preset's `cacheVariables`. NetCore is already on by default.
3. **Split the source.** Today everything lives in `Games/CoreGame/CoreGameGame.cpp`. Create `Games/CoreGame/Source/` and add its files to the game module in `Games/CoreGame/CMakeLists.txt`; see how DemoLab's CMakeLists globs `Source/`.
   ```
   Source/BPTypes.h                    enums, ids, bone-name and path sizes
   Source/Player/BPPlayer.{h,cpp}      native:BP.Player prefab + BPCharacterComponent
   Source/Weapons/BPWeapon.{h,cpp}     BPWeaponComponent, BPDamagePointComponent, registry, native:BP.Weapon
   Source/Weapons/BPProjectile.{h,cpp} native:BP.Projectile
   Source/Inventory/BPInventory.{h,cpp}
   Source/Combat/BPCombat.{h,cpp}      RPCs, server logic, input, presentation
   Source/Combat/BPHealth.{h,cpp}
   Source/Cosmetics/BPCosmetics.{h,cpp}
   Source/GameModes/BPGameMode.{h,cpp} registers systems; port LabGameModeBase
   ```
4. **Build once** before writing gameplay code:
   ```bash
   cmake --preset editor-debug && cmake --build --preset editor-debug
   ```

---

## Step 2 — The player prefab (walking + camera, networked)

Port `BuildLabPlayer` (`Games/DemoLab/Source/Core/LabPlayer.cpp:150-212`) into `Source/Player/BPPlayer.cpp` as `native:BP.Player`. Keep this component order on the root:

`Transform` → `Net{bPredicted=1}` → `AvatarHarness{InputMap="BPPlayer", Simulation="CharacterMove"}` → `CharacterMover` → `Render(body mesh)` → `Animation(BP_Humanoid.nmanimstate)` → `Ragdoll{bHitBoxes=1, bDropProps=0}` → `LagCompensated{Radius 35, Top 180}` → `BPHealth` → `BPInventory` → `BPCombat` → `BPCosmetics`, then the child `CameraBoom` → `Camera`.

- **Important:** Render and Animation must sit on the **root**. Sockets read the parent's pose, and the mover writes anim params on the root.
- Input map: copy DemoLab's `NIMBLE_DEFINE_INPUT_MAP` (find it with `grep -rn NIMBLE_DEFINE_INPUT_MAP Games/DemoLab`). Add these actions: `Fire`, `Aim`, `Melee`, `Throw`, `Reload`, `Interact`, `Slot1..Slot4`, `NextWeapon`, `PrevWeapon`.
- Game mode: port `LabGameModeBase` to spawn avatars through `NetGameMode` / `SpawnAvatarFor` at `PlayerStart`s (read `Engine/Source/AVATARS.md`). Register it with `NIMBLE_REGISTER_GAME_MODE` and name it in `Config/DefaultEngine.ini [World_CoreGame]`.
- To start, use the CrashDummy mesh and anim state from `Engine/Content/Characters/CrashDummy/`.

✅ **Checkpoint:** in Play-in-Editor with a listen server and 1 client, both players run, jump and aim with the camera boom.

---

## Step 3 — Weapon definition

### 3a. `BPTypes.h`
```cpp
enum class EWeaponCategory : uint8_t { Melee, Projectile, Throwable };
enum class EAnimSet       : uint8_t { Unarmed, OneHand, TwoHand, Heavy, Throw };  // one graph branch each
enum class EHolster       : uint8_t { Back, HipL, HipR, Belt };
#define BP_PATH (96)
#define BP_BONE (32)
using WeaponID = uint16_t;   // 0 = none; registry ids are append-only forever
```

### 3b. `BPWeaponComponent` (on the weapon prefab root)
Start from `LabWeaponComponent` (`Games/DemoLab/Source/Weapons/LabWeapon.h:24`). Change these fields:
- `WeaponType` → `Category` (EWeaponCategory)
- **add** `char AnimOverride[BP_PATH]`, the `.nmanimoverride` this weapon applies (Step 7)
- **add** `char OffHandIKBone[BP_BONE]` for two-handed grips
- **add** `uint8_t Holster` (EHolster). The registry maps it to a bone and offset, and per-weapon `HolsterPos`/`HolsterRot` still fine-tune it.
- **remove** `Reach` and `ArcDegrees`; damage points replace them
- keep the hand and holster bone/pos/rot, the cadence and ammo fields, melee windup/active/recovery/combo, and projectile/throw speed

### 3c. Damage points (new)
```cpp
struct BPDamagePointComponent {
    GAMECORE_COMPONENT_BODY()
    float   Radius      = 8.f;     // cm, swept-sphere approximation
    float   DamageScale = 1.f;     // blade edge 1.0, hilt 0.3, ...
    uint8_t Role        = 0;       // 0 Strike, 1 Muzzle, 2 Release (throwable spawn)
};
```
In the Prefab Editor, add children to the weapon prefab, each holding `Transform` and `BPDamagePoint`. A sword gets 3 to 4 points along the blade, a pistol gets one `Muzzle`, and a grenade gets one `Release`.

### 3d. Registry
Port `LabWeapons::Find / FindByPrefab / GetData / ResetCache` (`LabWeapon.cpp`). It's a static table `{ WeaponID, "Content/Prefabs/Weapons/X.nmprefab" }`. `GetData` spawns a hidden definition instance once per World, copies its `BPWeaponComponent` and caches it.

Also cache the **local offsets of each damage point** relative to the weapon root at that time (`Role`, `Pos`, `Radius`, `DamageScale`, max 8). The server can then compute melee sweeps from the hand-socket pose without walking prefab children every tick.

### 3e. Base prefab
```cpp
static void BuildBPWeapon(PrefabBuilder& B) {
    auto Root = B.Root("Weapon");
    Root.Add<TransformComponent>();
    Root.Add<RenderCore::RenderComponent>();
    Root.Add<RenderCore::AnimSocketComponent>();   // Bone set at runtime by Seat()
    Root.Add<BPWeaponComponent>();
}
GAMECORE_DEFINE_NATIVE_PREFAB(BPWeapon, "BP.Weapon", &BuildBPWeapon)
```
Content prefabs fork it in the editor: `Content/Prefabs/Weapons/{Wrench,PipePistol,Bomb}.nmprefab`. Each sets the mesh, stats and damage-point children.

### 3f. Projectile
Port `LabProjectile.*` → `native:BP.Projectile`, a Jolt `RigidBody` (Dynamic, `Continuous=1`). It holds a `BPProjectileComponent` with `Damage`, `Instigator`, `FuseSeconds`, `ExplodeRadius` and `bExplodeOnContact`. Handle the Jolt `ContactBegin` event on the authority. Unlike weapons, projectiles **do** carry a Net component, because their position matters to everyone.

---

## Step 4 — Inventory

`Source/Inventory/BPInventory.h`. Copy the shape of `LabInventoryComponent`:
```cpp
struct BPInventoryComponent {
    GAMECORE_COMPONENT_BODY()
    // weapons (4 slots, unrolled)
    uint16_t Weapon0=0, Weapon1=0, Weapon2=0, Weapon3=0;
    int32_t  Mag0=0, Mag1=0, Mag2=0, Mag3=0;
    int32_t  Reserve0=0, Reserve1=0, Reserve2=0, Reserve3=0;
    uint16_t Throwable=0; uint8_t ThrowCount=0;
    uint8_t  Equipped = BP_UNARMED;        // replicated slot index
    // server only
    uint8_t  PendingEquip = BP_NO_EQUIP; float SwitchTimer = 0.f;

    uint16_t& WeaponAt(int i);  int32_t& MagAt(int i);  int32_t& ReserveAt(int i);  // switch on i
    uint16_t  EquippedWeapon() const;
};
```
That's 15 replicated fields. Don't replicate `PendingEquip` or `SwitchTimer`.

Cosmetics go in a separate `BPCosmeticsComponent` (Step 8), which keeps each component's replication focused.

**Server API** (`namespace BPInventory`). Each function asserts authority:
- `bool GiveWeapon(World&, EntityID Avatar, WeaponID, bool bEquip)`: first empty slot, otherwise swap out the equipped weapon and drop it. Throwables stack into the pouch. Port `LabCombat::GiveWeapon`.
- `void GiveAmmo(World&, EntityID, WeaponID, int32_t Rounds)`
- `void DropWeapon(World&, EntityID, int Slot)`: spawns `native:BP.Pickup` (Net, Collision trigger, `WeaponID`, `Mag`, `Reserve`) at the hand, then clears the slot.
- `void RequestEquip(World&, EntityID, int Slot)`: sets `PendingEquip` and `SwitchTimer = Data->EquipSeconds`. `BPCombat::TickServer` commits `Equipped` when the timer runs out.

**Pickups:** a `Collision` component with `OnOverlapBegin`, or `ServerInteract(target)` for press-to-pick-up. Port `LabCombatComponent::ServerInteract`.

**Lua:** add `GAMECORE_DEFINE_COMPONENT_SCRIPT` so level scripts can read `Equipped` and ammo for HUDs. Remember that RPCs can't be declared in Lua.

---

## Step 5 — Combat: input → RPC → authority logic

`Source/Combat/BPCombat.h`. Start from `LabCombatComponent` (`LabCombat.h:50`):
- **Replicated sequence counters** (`AttackSeq`, `ComboIndex`, `ReloadSeq`, `ThrowSeq`, `HitSeq`, `LastHitPart`, `LastHitDir`). Presentation reacts when a counter **changes**, which survives packet loss better than a one-shot event.
- **Server-only:** `Cooldown`, `ReloadTimer`, `MeleeTimer`, `MeleePhase`, `MeleeHits[8]`, `MeleeViewTimeMs`, plus **new** `Vector3 PrevPointWS[8]` (last tick's damage-point positions).
- **Owner-only:** `LocalCooldown`, `ThrowCharge`, `bCharging`.
- **RPCs:** `ServerFire(origin, dir, seq)`, `ServerMelee()`, `ServerReload()`, `ServerEquip(slot)`, `ServerThrow(origin, dir, charge)`, `ServerInteract(target)`, `MulticastFireFx`, `ClientHitConfirm`. Copy the rate limits from `LabCombat.cpp:467`.

**Systems.** Register them in your game mode the way `LabGameModeBase.cpp:54-59` does:
```cpp
AddSystem<&BPGameMode::TickCombatInput>("BP.CombatInput", TP_PrePhysics);    // locally controlled avatars
AddSystem<&BPGameMode::TickCombatServer>("BP.CombatServer", TP_PrePhysics);  // authority timers + melee windows
AddSystem<&BPGameMode::TickProjectiles>("BP.Projectiles", TP_PrePhysics);
AddSystem<&BPGameMode::TickPresentation>("BP.Presentation", TP_PostPhysics); // every process
AddSystem<&BPGameMode::TickCosmetics>("BP.Cosmetics", TP_PostPhysics);       // every process
```

### Per-category authority logic
**Projectile** (`BPCombat::Fire`): check `Cooldown`, `Mag`, `bReloading`. Spawn `native:BP.Projectile` at the `Muzzle` point, then `++AttackSeq` and `--Mag`. For a hitscan variant, use `LagCompensatedRaycast` (`LagCompensation.h`) the way `LabCombat::Fire` does, and use `Ragdoll GetPartAt` for headshots.

**Melee** (`BPCombat::Melee` + `TickServer`): this is the new part. DemoLab only does an arc test.
1. `ServerMelee` sets `MeleePhase=1`, `MeleeTimer=Windup`, `++AttackSeq`, advances `ComboIndex`, records `MeleeViewTimeMs` from `RpcContext`, and clears `MeleeHits`.
2. When windup ends, `MeleePhase=2` (Active). Seed `PrevPointWS[i]` from the current hand pose.
3. Each Active tick, for every Strike point:
   1. Compute `Cur = HandSocketWorldPose * PointLocal`. The server evaluates animation too, so `AnimationComponent::GetBoneLocation` or the instance's world pose works there.
   2. Ray from `Prev` to `Cur` with `LagCompensatedRaycast` at `MeleeViewTimeMs`.
   3. The engine has **no sphere sweep**, so approximate the radius with up to 3 parallel rays offset by `Radius` on the swing plane, or 1 ray plus a capsule test against the `LagCompensated` capsule.
   4. Skip entities already in `MeleeHits`. Otherwise apply `Damage * DamageScale * PartMultiplier`, add them to `MeleeHits`, and call `ClientHitConfirm`.
   5. Set `Prev = Cur`.
4. Active → Recovery → idle. These are timer-driven and **never anim-timed**, which keeps the server deterministic.

**Throwable** (`BPCombat::Throw`): the owner accumulates `ThrowCharge` while Throw is held. On release it sends `ServerThrow(origin, dir, charge)`. The server clamps `charge` to 0..1, spawns the projectile at the `Release` point with speed `lerp(0.4, 1.0, charge) * ThrowSpeedMax`, then does `--ThrowCount` and `++ThrowSeq`.

**Reload** and **Equip** timers live in `TickServer`. Port them directly.

### Health
Port `LabHealth`: replicated `HP` and `bDead`. On death, `Ragdoll` takes over. Clear presentation sockets, as DemoLab does with its `bDead` check.

---

## Step 6 — Presentation: spawning weapons on sockets (every process)

Port `LabCombat::TickPresentation` (`LabCombat.cpp:1480-1570`). Keep a `std::unordered_map<uint64_t, Presented>` on the game mode, keyed by avatar. Per avatar:
1. **Spawn or despawn** one local instance per held slot, but only when `Presented.ID[slot] != Inventory.WeaponAt(slot)`. Spawn with `World.SpawnPrefab(path, TransformPose(), name)`, then `Root.SetParent(Avatar, false)`.
   - It has to be a **direct child** of the animated root.
   - Destroy with `DestroyPrefabInstance(Root.GetInstance())`.
2. **Seat it.** Copy `Seat()` from `LabCombat.cpp:1275`. The equipped slot goes on `HandBone`/`HandPos`/`HandRot`; the rest go on their holster bone. Changing `AnimSocketComponent::Bone` is the whole attach operation.
3. **Anim:**
   - set `AnimSet` (float), `Armed` (bool), and `AnimationComponent::Override = weapon.AnimOverride` (Step 7)
   - fire the `Fire`/`Melee`/`Reload`/`Throw` triggers when the replicated sequence counters change
   - don't re-trigger on the owner, who already fired the trigger locally when they pressed (`bOwner` check)
4. **Equip timing with notifies.** To make the weapon visibly move at the right frame, put `WeaponToHand` and `WeaponToHolster` AnimNotifies in the Equip/Holster clips. Subscribe to `AnimNotify` on the EventBus; `Engine/Content/Characters/CrashDummy/CrashDummyDemo.lua` shows the event and payload. Flip the socket when it fires.
   - Gameplay still trusts the server's `SwitchTimer`. If a notify is missed, step 2 snaps the socket to the hand once `Equipped` changes. That's the fallback.
5. **Off-hand IK:** for TwoHand or Heavy weapons, use `AnimationComponent::SetIKGoal` toward the weapon's `OffHandIKBone` child, or set the anim state's two-bone IK "off hand to bone" target.

✅ **Checkpoint:** melee works end to end with the Wrench. Two clients see the same holster and hand swaps, and hits land.

---

## Step 7 — ENGINE: clip-override map

**Goal:** a weapon swaps specific clips in the shared anim state without editing the graph.

**Design:** use an override map keyed by the *original clip path*, so the graph uses placeholder clips. This means **no anim state format change and no graph-editor change**.

The base graph's OneHand branch plays `Anims/Slots/OneHand_Fire.nmanim`. A pistol override maps that path to `Weapons/PipePistol/Fire.nmanim`.

### 7a. Asset: `AnimOverrideAsset.{h,cpp}` (RenderCore, next to `AnimStateAsset.*`)
```cpp
struct AnimClipOverride { std::string From; std::string To; };   // runtime paths
struct AnimOverrideData { std::vector<AnimClipOverride> Clips; };
```
- Serialize it like `AnimStateAsset.cpp` (versioned chunk, XML in the editor, binary when cooked).
- Add an `AnimOverrideAssetCache::Find(path)` that mirrors `AnimClipAssetCache`.
- Register the `.nmanimoverride` extension wherever `.nmanimstate` is registered (`grep -rn nmanimstate Engine/Modules`).

### 7b. Component: `Engine/Modules/RenderCore/Source/Public/RenderCore/Components/AnimationComponent.h`
- add `char Override[ANIMATION_PATH_MAX] = {};` after `Retarget`
- add it to the serializer (bump the version), replication and properties, wherever `Animation`/`Retarget` are listed

### 7c. Instance: `Engine/Modules/RenderCore/Source/Private/RenderCore/AnimInstance.cpp`
- The hook is **line 164**:
  `Runtime.Samples.push_back(MakeSample(AnimClipAssetCache::Find(Each.Clip), ...))`
- Store `Each.Clip` (the source path) on the runtime `Sample` (add a `std::string SourceClip`, or an index into a path table).
- Add `void AnimInstance::SetOverrides(std::shared_ptr<const AnimOverrideData>)`. For every layer → state → sample, choose `To` when `SourceClip` matches a `From`, otherwise `SourceClip`, and rebuild that sample with `MakeSample`. Keep `Position`/`PositionY`, and re-sort only if your sorting depends on the clip (it doesn't; it sorts by Position).
- Retargeted samples: `MakeSample` already handles a retarget per clip. Make sure the replacement clip goes through the same path.
- Mid-state swaps: the swap applies immediately to the next sample evaluation. Equip happens during the Equip transition, so pops are hidden. If one shows, defer it until the current state exits.

### 7d. System: `AnimationSystem.{h,cpp}`
- Add `std::string OverridePath` and `std::shared_ptr<const AnimOverrideData> Override` to `Entry` (`AnimationSystem.h:166`).
- Next to where `AnimationPath` and `Retarget` change detection happens (`AnimationSystem.cpp:~195-214`), compare `Component.Override`. On a change, load it and call `Instance->SetOverrides`. Also re-apply it after an instance rebuild.

### 7e. Tests and docs
- **Tests:** add cases to RenderCore's animation tests. Cover a match being replaced, no match staying unchanged, a swap and clear, and a missing `To` clip falling back to the source with a warning.
- **Docs:** add an "Clip overrides" section to `Engine/Modules/RenderCore/ANIMATION.md` and update `Engine/Docs/Components/Animation.md`.

Each weapon then gets e.g. `Content/Weapons/PipePistol/PipePistol.nmanimoverride` and points to it from `BPWeaponComponent::AnimOverride`.

---

## Step 8 — ENGINE: shared pose for skinned cosmetics

**Goal:** a skinned shirt mesh on a child entity deforms with the body.

### 8a. Component `PoseFollowerComponent` (RenderCore, "Pose Follower")
It's a marker with no fields; the leader is the parent. Define it the way `AnimSocketComponent` is defined.

### 8b. `WorldBridge::ExtractPoses` (`Engine/Modules/RenderCore/Source/Private/RenderCore/WorldBridge.cpp:~991`)
Inside the `Each<AnimationComponent>` lambda, after the leader's push:
```cpp
for(EntityID C = InEntity.GetFirstChild(); C.IsSet(); C = BoundWorld->GetEntity(C).GetNextSibling())
{
    const Entity Child = BoundWorld->GetEntity(C);
    if(!Child.GetComponent<PoseFollowerComponent>()) continue;
    const RenderComponent* CR = Child.GetComponent<RenderComponent>();
    if(CR && CR->Proxy.Ptr && SameSkeleton(*CR, *Render))     // mesh's skeleton == leader mesh's skeleton
        Now.push_back({ CR->Proxy.Ptr, &Instance->GetModelPose() });
}
```
- `Now` carries pointers, so the palette is shared and nothing is copied. The rest of the function (released proxies, blob packing) already handles extra entries.
- `SameSkeleton`: compare the skinned meshes' skeleton reference or bone count. Look at how `MeshAsset`'s `MSKN` chunk names its skeleton. If they don't match, log once and skip, so the follower stays in its bind pose.
- **Bounds:** in `RenderComponentProxy.cpp`, find where skinned bounds come from the pose and check that the follower's bounds use the leader's palette. A pose follower moves with its parent's transform, so keep the child's local transform at identity.

### 8c. Art pipeline
Clothing must be imported against the body's `.nmskel`. The full DCC-to-player flow is in **Appendix A**.

---

## Step 9 — Cosmetics gameplay

`Source/Cosmetics/BPCosmetics.h`:
```cpp
enum class ECosmeticSlot : uint8_t { Head, Torso, Legs, Hands, Feet, Back, Count };
struct BPCosmeticsComponent {
    GAMECORE_COMPONENT_BODY()
    uint16_t Head=0, Torso=0, Legs=0, Hands=0, Feet=0, Back=0;   // replicated cosmetic ids
    uint8_t  DyeHead=0, DyeTorso=0, DyeLegs=0, DyeHands=0, DyeFeet=0, DyeBack=0;  // optional
    uint16_t& At(ECosmeticSlot);
};
```
- **Registry:** `BPCosmetics::Find(id)` returns `{ Prefab, Slot, HideBodyMask }`. Ids are append-only.
- **Cosmetic prefabs:**
  - Skinned: root holds `Transform`, `Render(mesh imported against the body skeleton)` and `PoseFollower`.
  - Rigid: root holds `Transform`, `Render` and `AnimSocket(Bone="DEF-head", offsets)`. A multi-piece set has several such children.
  - Mixed: a skinned coat root plus rigid pauldron children.
  - For rigid sets, copy DemoLab's Brute armor (`Games/DemoLab/Source/AI/LabArmour.cpp`).
- **Presentation** (`TickCosmetics`, every process): same diffing as Step 6.1. Spawn on id change and parent to the avatar root.
- **Hiding body parts:** `RenderComponent` can't hide sections. Use `SetMaterial(section, "Materials/Invisible.nmmat")` for each section set in `HideBodyMask`, and restore it when the cosmetic is removed. Long-term, author the body as split sections (head, torso, arms, legs, feet).
- **Dyes:** `SetMaterialColor` / `SetMaterialParameter` on the cosmetic's Render.
- **Unlocks** ("obtained throughout gameplay"): the owned set is **not** a component. Keep it in per-player persistence (`GameSession` / `PlayerState` save; see `Engine/Source/GAMEFRAMEWORK.md`).
  - The server validates `ServerEquipCosmetic(slot, id)` against the owned set, then writes `BPCosmeticsComponent`.
  - Drops in the world (`BP.CosmeticPickup`) call `BPCosmetics::Unlock(player, id)` on the authority.

---

## Step 10 — Anim state asset

Build `Content/Characters/BPHumanoid/BP_Humanoid.nmanimstate` in the editor graph. Use the CrashDummy anim state as a template.
- **FullBody:** 2D locomotion blend on `Speed` and `Direction`, Jump, Crouch, driven by CharacterMover.
- **UpperBody:** masked override from the spine up, weighted by `Armed`.
  - One sub-machine per `AnimSet`, selected by the `AnimSet` param.
  - Each sub-machine has states Equip → Idle ⇄ Aim, Fire (trigger, Once), Melee1/2/3 (trigger plus `ComboIndex`), Reload, ThrowWindup → ThrowRelease, and Holster.
  - **Every state plays a placeholder clip** under `Anims/Slots/<Set>_<State>.nmanim`. These are what overrides key on.
- **Additive:** Recoil and HitReact.
- **Look-at:** AimPitch.
- **Params:** Speed, Direction, Grounded, AimPitch, Armed, AnimSet, Fire, Melee, ComboIndex, Reload, Throw. That's 11 of the 12 allowed, so leave the last one free.

---

## Appendix A — Clothing cosmetic pipeline (DCC → asset → player)

The whole pipeline rests on one rule: **every clothing mesh is skinned to the body's skeleton**. The importer enforces most of it (`Engine/Modules/Editor/Source/Private/Editor/Content/AssetImport.cpp:783-808`).

### A1. Authoring in Blender
**Master body file** (`Art/Characters/BPHumanoid/BPHumanoid.blend`):
- **Rig:** the CrashDummy Rigify rig; the engine uses its `DEF-*` deform bones.
- **Locked rest pose:** pick A-pose or T-pose and never change it.
- **Body split into material sections:** Head, Torso, Arms, Hands, Legs, Feet. `RenderComponent` can't hide a section, but it can swap a section's material, so hiding the body under clothing only works if the body is split this way. The limit is 8 material slots per mesh.
- **A range-of-motion action** for clipping tests: arms up, crouch, twist, run.

**One file per garment** (`Art/Cosmetics/<Slot>/<Name>/<Name>.blend`). **Link** the body and rig into it from the master file instead of copying them, so a rig change reaches every garment. Then:
1. Model over the body in the rest pose, with a small offset so it doesn't clip.
2. Parent the garment to the same armature. Copy the body's weights with a Data Transfer modifier (*Vertex Groups*, *Nearest Face Interpolated*), then fix the shoulders, elbows, hips and knees by hand.
3. **Limit each vertex to 4 influences and normalize.** The engine skins with 4 weights per vertex.
4. **Don't add new bones.** The import fails if the file has a bone the body lacks. If a cape or skirt needs extra bones, add them to the master rig.
5. Use 1–3 named material slots (`Jacket_Cloth`, `Jacket_Leather`). These are the dye targets.
6. Apply transforms, and scrub the range-of-motion action to check for clipping.

### A2. Export (glTF / GLB)
Select only the armature and the garment, then export with:
- *Selected Objects*
- *Export Deforming Bones Only*
- Skinning on, *Influence Limit* 4
- Apply Modifiers on
- **No animations** (the file should have no actions)

**Gotcha:** the importer only reads a skeleton when `ImportAnimations` is on (`GltfImporter.cpp:901`). So keep that setting on and make sure the file has no actions, or you'll get stray clips.

### A3. Import
In the Import Wizard, set **Existing Skeleton** to the body `.nmskel`. Headless:
```bash
NimbleEditor --import Art/Cosmetics/Torso/Jacket/Jacket.glb --import-to Content/Cosmetics/Torso/Jacket --import-settings ExistingSkeleton=Content/Characters/BPHumanoid/BPHumanoid_Skeleton.nmskel ImportAnimations=1 CombineMeshes=1
```
- Every bone in the file must exist in the body skeleton. If one doesn't, the import fails and names it.
- `RemapSkin` moves the skin onto the body's bone indices. That's what lets the garment share the body's pose.
- The asset remembers its source, so re-export and **Reimport** updates it in place. For one-click reimport of all clothing, script a loop over `Art/Cosmetics/**/*.glb`.
- Materials expose a tint parameter for dyes (`SetMaterialColor` / `SetMaterialParameter`).
- **Check:** open the `.nmmesh` in the mesh viewer and play a body clip on it. It should animate exactly like the body.

### A4. Cosmetic prefab
`Content/Cosmetics/<Slot>/<Name>/<Name>.nmprefab`. The root holds:

| Component | Setup |
|---|---|
| `Transform` | identity |
| `Render` | the garment `.nmmesh` plus its materials |
| `PoseFollower` | Step 8; takes the parent's pose |
| `BPCosmeticItem` | `Slot`, `HideBodyMask` (body sections to hide), `HideSlotMask` (e.g. a full helmet hides Hair) |

- **Rigid add-ons** (pauldrons, buckles) are children holding `Render` and `AnimSocket(Bone, offsets)`. They're imported as static meshes with no skeleton.
- **Registry:** append `{ id, prefab path }` to `BPCosmetics`. Ids are append-only.

### A5. Runtime attach
1. The server validates ownership and writes `BPCosmeticsComponent.<Slot> = id`. Only that id replicates.
2. `TickCosmetics` (every process) despawns the old piece, spawns the prefab, and calls `SetParent(avatarRoot)`. It must be a **direct child** of the entity with Render and Animation.
3. It swaps each `HideBodyMask` section to `Materials/Invisible.nmmat` on the body, and restores them on unequip.
4. It applies the dye to the cosmetic's Render.

Until Pose Follower exists, skinned pieces render in the bind pose. Rigid socketed pieces (helmets) work today.

### A6. Folders
```
Art/Cosmetics/<Slot>/<Name>/<Name>.blend, <Name>.glb          source
Content/Cosmetics/<Slot>/<Name>/<Name>.nmmesh, *.nmmat, textures, <Name>.nmprefab
```
**Lock the rest pose and skeleton, and split the body into sections, early.** Changing either later means re-weighting or re-importing every garment.

## Delivery order (each milestone is playable)
1. Steps 1–2: networked walking with the camera.
2. Steps 3–6 with **melee only** (Wrench): inventory, sockets, equip, swept damage points.
3. PipePistol (Projectile) and Bomb (Throwable) with Jolt projectiles. Then pickups and drop-on-death.
4. Step 7, the clip override. Move each weapon's anims onto `.nmanimoverride`.
5. Step 8 and then Step 9: one skinned shirt and one rigid helmet. Then unlocks and persistence.
6. Step 10 polish, plus HUD (UI module) for the ammo, slots and throw-charge meter.

## Verification checklist
- **Build:** Editor, Client and Server presets on RelWithDebInfo with Jolt on.
- **Engine tests:** override replace, fallback and swap; pose follower with matching vs mismatched skeletons.
- **Listen server plus 1 client:**
  - Equip each category; the socket swaps on the notify on both screens.
  - Melee hits register once per swing, with headshot multipliers.
  - Projectiles and throwables collide and deal damage.
  - Ammo, reload and drop all replicate.
- **Dedicated `CoreGameServer` plus 2 clients and a late joiner:** the joiner sees the correct weapons and cosmetics.
- **Cosmetics:** the skinned shirt deforms with locomotion with no T-pose; the helmet tracks the head; hidden body sections stay hidden.
- **Perf:** use the `perf-testing` skill with 8 fully dressed players to measure the extra skinned draws and pose sharing.

