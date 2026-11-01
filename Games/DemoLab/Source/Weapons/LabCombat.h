#ifndef DEMOLAB_LABCOMBAT_H
#define DEMOLAB_LABCOMBAT_H

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

#include "Core/LabTypes.h"

#include <unordered_map>

namespace GameCore { class World; }
struct LabWeaponComponent;

/*
 * Inventory and combat (TDD §5.3-5.6), on the character's root.
 *
 *   LabInventoryComponent  three weapon slots, ammo, the equipped slot.
 *                           Replicated; changed on the authority only.
 *   LabCombatComponent     attack state that drives presentation on every
 *                           process (counters, not anim params), the server's
 *                           timers, and every action RPC a client sends.
 *
 * Replication takes no arrays but char[], so the slots are unrolled; reach
 * them with the *At() helpers.
 */

struct LabInventoryComponent {
    GAMECORE_COMPONENT_BODY()
    uint8_t Weapon0 = 0, Weapon1 = 0, Weapon2 = 0;        // registry ids; 0 = empty
    int32_t Mag0 = 0, Mag1 = 0, Mag2 = 0;
    int32_t Reserve0 = 0, Reserve1 = 0, Reserve2 = 0;
    uint8_t Throwable = 0;
    uint8_t ThrowCount = 0;
    uint8_t Equipped = LabWeapons::UNARMED;               // slot index; UNARMED = none
    uint8_t PendingEquip = LabWeapons::NO_EQUIP;          // server: slot being switched to
    float   SwitchTimer = 0.f;                             // server: seconds left in the switch

    uint8_t&       WeaponAt(int InSlot);
    uint8_t        WeaponAt(int InSlot) const;
    int32_t&       MagAt(int InSlot);
    int32_t        MagAt(int InSlot) const;
    int32_t&       ReserveAt(int InSlot);
    int32_t        ReserveAt(int InSlot) const;
    /* The equipped weapon's id, 0 when unarmed. */
    uint8_t        EquippedWeapon() const;
};

struct LabCombatComponent {
    GAMECORE_COMPONENT_BODY()

    // Replicated: presentation follows these on every process.
    uint8_t AttackSeq = 0;          // ++ per attack (shot, swing)
    uint8_t ComboIndex = 0;
    uint8_t ReloadSeq = 0;
    uint8_t HitSeq = 0;             // ++ per damage taken
    uint8_t LastHitPart = 0;
    EngineCore::Vector3 LastHitDir;
    uint8_t LastHitHeavy = 0;
    uint8_t bReloading = 0;
    uint8_t StaggerFrames = 0;      // input frames the character cannot move (LabMove); set by a heavy hit
    uint8_t ThrowSeq = 0;           // ++ per throw
    float   Spread = 0.f;           // degrees; owner-predicted, server-authoritative

    // Server only.
    float   Cooldown = 0.f;
    float   ReloadTimer = 0.f;
    float   MeleeTimer = 0.f;
    uint8_t MeleePhase = 0;         // 0 idle, 1 windup, 2 active, 3 recovery
    uint8_t BurstLeft = 0;
    uint32_t MeleeViewTimeMs = 0;
    GameCore::EntityID MeleeHits[8];

    // Owning process only (not replicated, not saved).
    float   LocalCooldown = 0.f;
    float   HitMarker = 0.f;        // seconds the crosshair hit marker shows
    float   ThrowCharge = 0.f;      // 0..1 while Throw is held
    uint8_t bCharging = 0;

    // === RPCs ===============================================================
    void ServerFire(const GameCore::RpcContext& InContext, const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDir, uint8_t InSeq);
    void ServerMelee(const GameCore::RpcContext& InContext);
    void ServerReload(const GameCore::RpcContext& InContext);
    void ServerEquip(const GameCore::RpcContext& InContext, uint8_t InSlot);
    void ServerInteract(const GameCore::RpcContext& InContext, GameCore::EntityID InTarget);
    void ServerThrow(const GameCore::RpcContext& InContext, const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDir, float InCharge);

    void MulticastFireFx(const GameCore::RpcContext& InContext, const EngineCore::Vector3& InImpact, uint8_t InSurface);
    void ClientHitConfirm(const GameCore::RpcContext& InContext, float InDamage, bool bInKill, bool bInHead);
    void MulticastBurstFx(const GameCore::RpcContext& InContext, const EngineCore::Vector3& InAt,
                          const GameCore::RpcString<96>& InFx, const GameCore::RpcString<96>& InSound, float InRadius);
};

/* Shot surfaces for MulticastFireFx. */
enum ELabSurface : uint8_t { PS_None = 0, PS_World = 1, PS_Lab = 2 };

namespace LabCombat {
    /* Authority: the logic behind each RPC, also called directly by a local
     * authority (standalone, listen host, server bots). */
    void Fire(GameCore::World& InWorld, GameCore::EntityID InAvatar, const GameCore::RpcContext* InCall,
              const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDir, uint8_t InSeq);
    void Melee(GameCore::World& InWorld, GameCore::EntityID InAvatar, uint32_t InViewTimeMs);
    void Reload(GameCore::World& InWorld, GameCore::EntityID InAvatar);
    void Equip(GameCore::World& InWorld, GameCore::EntityID InAvatar, uint8_t InSlot);
    void Throw(GameCore::World& InWorld, GameCore::EntityID InAvatar, const EngineCore::Vector3& InOrigin,
               const EngineCore::Vector3& InDir, float InCharge);

    /* Authority: a burst's flash and sound on every process (through
     * InInstigator's combat RPC; locally if it is gone). */
    void BurstFx(GameCore::World& InWorld, GameCore::EntityID InInstigator, const EngineCore::Vector3& InAt,
                 const char* InFx, const char* InSound, float InRadius);
    /* Authority: a loud thing happened (shots, bursts): AI hears it. */
    void ReportNoise(GameCore::World& InWorld, GameCore::EntityID InSource, const EngineCore::Vector3& InAt, float InRange);

    /* Authority: into the first empty slot (or the equipped one, swapped, if
     * full); full mag and reserve; equips it when unarmed. A throwable goes
     * to the throwable pouch instead: MagSize more, up to ReserveMax.
     * False if unknown. */
    bool GiveWeapon(GameCore::World& InWorld, GameCore::EntityID InAvatar, uint8_t InWeaponID, bool bInEquip = false);
    /* Authority: tops up every slot holding InWeaponID (0: every slot). */
    void GiveAmmo(GameCore::World& InWorld, GameCore::EntityID InAvatar, uint8_t InWeaponID, int32_t InRounds);

    /* The equipped weapon's data, nullptr when unarmed. Every process. */
    const LabWeaponComponent* EquippedData(GameCore::World& InWorld, GameCore::EntityID InAvatar);

    /* Systems (LabGameModeBase registers them). */
    void TickInput(GameCore::World& InWorld, float InSeconds);          // every process: locally controlled avatars
    void TickServer(GameCore::World& InWorld, float InSeconds);         // authority: cooldowns, reloads, switches, melee windows

    /* Every process: weapon instances, sockets, anim params, ragdoll state.
     * The state it keeps per avatar lives in the caller's map. */
    struct Presented
    {
        GameCore::EntityID Instance[LabWeapons::SLOTS];
        uint8_t ID[LabWeapons::SLOTS] = {};
        uint8_t SeenAttackSeq = 0, SeenReloadSeq = 0, SeenHitSeq = 0;
        uint8_t bSeenInit = 0;
        uint8_t bShownDead = 0;
        float   StaggerTimer = 0.f;
        float   HitReactTimer = 0.f;  // seconds before a light hit may start another HitReact

        // The weapon hand (AimHand IK, TDD §5.7 done procedurally).
        uint8_t IKAttackSeq = 0, IKHitSeq = 0, IKThrowSeq = 0;
        float   ThrowT = -1.f;        // seconds into a throw's overhand arc; < 0: none
        float   AimBlend = 0.f;       // 0..1 toward the raised aim
        float   ReloadBlend = 0.f;
        float   FireHold = 0.f;       // seconds the gun stays up after a shot
        float   Recoil = 0.f;         // 1 at a shot, eases to 0
        float   Flinch = 0.f;         // 1 at a hit taken
        float   SwingT = -1.f;        // seconds into a melee swing; < 0: none
        float   SwingLength = 0.f, SwingWindup = 0.f, SwingActive = 0.f;
        float   SwingSide = 1.f;      // +1 forehand (right to left), -1 back
    };
    using PresentedMap = std::unordered_map<uint64_t, Presented>;
    void TickPresentation(GameCore::World& InWorld, float InSeconds, PresentedMap& InOutPresented);
    /* Destroys every presented weapon instance (a level reset, an unload). */
    void ClearPresentation(GameCore::World& InWorld, PresentedMap& InOutPresented);

    /* Authority, after a save is restored: what each process makes for
     * itself and a save would have doubled -- weapon instances, effects, the
     * arc preview, projectiles in flight. */
    void ClearTransients(GameCore::World& InWorld, PresentedMap& InOutPresented);

    /* The weapon instance in a character's hand, or unset. */
    GameCore::EntityID HeldInstance(GameCore::World& InWorld, GameCore::EntityID InAvatar);
}

#endif //DEMOLAB_LABCOMBAT_H
