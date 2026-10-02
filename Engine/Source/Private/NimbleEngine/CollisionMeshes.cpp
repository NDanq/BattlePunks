#include <map>
#include <string>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderComponents.h"

/*
 * GameCore's CollisionMeshProvider, from RenderCore: a CS_Mesh collider's
 * shape is the bounds of its RenderComponent's mesh, as the mesh file's header
 * records them (no vertices loaded). A RenderComponent without a mesh draws a
 * unit quad, so that is its shape. Installed when this image loads -- the game
 * executable and the editor both link it -- and removed when it unloads.
 *
 * Headers are cached by path, failures too (warned once): a collider whose
 * mesh never changes reads its file once.
 */
namespace
{
    std::map<std::string, std::pair<bool, GameCore::AABB>>& Cache()
    {
        static std::map<std::string, std::pair<bool, GameCore::AABB>> Entries;
        return Entries;
    }

    const char* GetMeshPath(GameCore::World& InWorld, const GameCore::EntityID InEntity, void*)
    {
        const auto* Render = InWorld.GetEntity(InEntity).GetComponent<RenderCore::RenderComponent>();
        return Render ? Render->Mesh : nullptr;
    }

    bool GetMeshBounds(const char* InPath, GameCore::AABB& OutLocal, void*)
    {
        const std::string Path = InPath ? InPath : "";
        if(Path.empty())
        {
            /* What WorldBridge draws without a mesh: the unit quad, centred. */
            OutLocal = GameCore::AABB(EngineCore::Vector3(-0.5, -0.5, 0.0), EngineCore::Vector3(0.5, 0.5, 0.0));
            return true;
        }

        auto Found = Cache().find(Path);
        if(Found == Cache().end())
        {
            RenderCore::MeshHeader Header;
            const bool bRead = RenderCore::LoadMeshHeader(Path, Header);
            if(!bRead)
            {
                CORE_LOG_WARN("[Collision] no mesh header in '" + Path + "'; colliders using it keep their Bounds")
            }
            const GameCore::AABB Box(EngineCore::Vector3(Header.BoundsMin[0], Header.BoundsMin[1], Header.BoundsMin[2]),
                                     EngineCore::Vector3(Header.BoundsMax[0], Header.BoundsMax[1], Header.BoundsMax[2]));
            Found = Cache().emplace(Path, std::make_pair(bRead, Box)).first;
        }
        if(!Found->second.first) { return false; }
        OutLocal = Found->second.second;
        return true;
    }

    struct CollisionMeshRegistrar
    {
        CollisionMeshRegistrar()  { NimbleEngine::InstallCollisionMeshProvider(); }
        ~CollisionMeshRegistrar() { GameCore::SetCollisionMeshProvider(nullptr); }
    };
    CollisionMeshRegistrar GCollisionMeshRegistrar;
}

namespace NimbleEngine
{
    void InstallCollisionMeshProvider()
    {
        GameCore::CollisionMeshProvider Provider;
        Provider.GetMeshPath   = &GetMeshPath;
        Provider.GetMeshBounds = &GetMeshBounds;
        GameCore::SetCollisionMeshProvider(&Provider);
    }
}
