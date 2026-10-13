#ifndef NIMBLEENGINE_PLAYERSTART_H
#define NIMBLEENGINE_PLAYERSTART_H

#include <cstdint>

#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"

namespace GameCore { class World; }

/*
 * Where players' avatars come into the level (Avatar.h), and a mode that
 * does it for you. AVATARS.md has the whole flow.
 */
namespace NimbleEngine
{
    class GameSession;

    enum : size_t { PLAYER_START_PREFAB_MAX = 128 };   // including the NUL

    /* "PlayerStart": a place (its Transform) an avatar spawns at. */
    struct NE_API PlayerStartComponent
    {
        GAMECORE_COMPONENT_BODY()

        char    AvatarPrefab[PLAYER_START_PREFAB_MAX] = "";   // what spawns here; "" for the mode's
        uint8_t PlayerSlot = 0;   // 1..4: kept for that player (by join order); 0 any
        uint8_t bEnabled   = 1;
    };

    /* The start for the InOrdinal'th player (0 first): one kept for its
     * slot, else the enabled ones in turn. Unset if the level has none. */
    NE_API GameCore::EntityID FindPlayerStart(GameCore::World& InWorld, uint32_t InOrdinal);

    /*
     * Authority only. Spawns an avatar for InPlayer at InStart (the origin
     * if unset) and possesses it. The prefab is InPrefab, else InStart's,
     * else [Avatar] DefaultPrefab. The avatar's Net component, if any, is
     * owned by InPlayer's client from the start, so it goes when the client
     * does. Unset on failure, logged.
     */
    NE_API GameCore::EntityID SpawnAvatarFor(GameCore::World& InWorld, GameCore::EntityID InPlayer, const char* InPrefab = nullptr,
                                             GameCore::EntityID InStart = GameCore::EntityID());

    /* Every AvatarHarness with AutoPossess N is taken by local player N
     * (made if needed). Authority and a local user only -- not on a client
     * or a dedicated server. GameSession runs it as a level starts, before
     * the mode's OnStart, so placed avatars work with any mode and in the
     * editor's Play. */
    NE_API void ApplyAutoPossess(GameCore::World& InWorld);

    /*
     * Default rules for a game with avatars; derive from it and add yours
     * (call the base when overriding these):
     *
     *   OnStart              each local player ([Avatar] LocalPlayers, 1) gets
     *                        an avatar, unless one was placed for it
     *                        (AutoPossess) -- not on a dedicated server
     *   OnClientConnected    the client's player gets an avatar
     *
     * Spawns at FindPlayerStart(join order). GetAvatarPrefab picks the
     * prefab; OnAvatarSpawned dresses the result (colour, name).
     */
    class NE_API NetGameMode : public IGameMode
    {
    public:
        bool OnLoad(GameSession& InSession) override;
        void OnStart() override;
        void OnClientConnected(int32_t InClient, GameCore::EntityID InPlayer) override;

        /* GetAvatarPrefab asked for no particular player: what this mode spawns
         * unless a Player Start or [Avatar] DefaultPrefab decides. For tools
         * (Collaboration's content report), not play. */
        const char* PeekAvatarPrefab() { return GetAvatarPrefab(GameCore::EntityID()); }

    protected:
        /* nullptr: the start's, else [Avatar] DefaultPrefab. */
        virtual const char* GetAvatarPrefab(GameCore::EntityID InPlayer) { (void)InPlayer; return nullptr; }

        /* InAvatar spawned for InPlayer, the InOrdinal'th to join, and is
         * possessed. Server only. */
        virtual void OnAvatarSpawned(GameCore::EntityID InPlayer, GameCore::EntityID InAvatar, uint32_t InOrdinal)
        {
            (void)InPlayer; (void)InAvatar; (void)InOrdinal;
        }

        /* An avatar for InPlayer, the next in join order. */
        GameCore::EntityID SpawnFor(GameCore::EntityID InPlayer);

        GameSession* Session        = nullptr;
        uint32_t     NextOrdinal    = 0;
        bool         bWarnedNoStart = false;   // once a level: no enabled Player Start
    };
}

#endif //NIMBLEENGINE_PLAYERSTART_H
