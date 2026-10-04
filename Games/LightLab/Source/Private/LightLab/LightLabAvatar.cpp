#include "Private/LightLab/LightLabAvatar.h"

#include <cstdio>

#include "Public/NimbleEngine/NetFacade.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/RenderComponents.h"

/* LightLabAvatar.h has the design. */

namespace
{
    /* How often a client reports its camera. The orbs move at that rate on
     * the others' screens; the server's tick is faster, so each report is
     * sent on promptly. */
    constexpr double PoseIntervalSeconds = 0.1;

    const char* const OrbMesh     = "Engine/Content/Meshes/Sphere.nmmesh";
    const char* const OrbMaterial = "Content/Materials/Glow.material.nmgraph";
    constexpr double  OrbDiameter = 60.0;

    double GSinceLastPose = 0.0;
}

GAMECORE_DEFINE_COMPONENT(LightLabAvatarComponent, "LightLabAvatar")

/* Nothing authored, but a serializer is what sends a component to clients
 * when its entity appears there. */
GAMECORE_DEFINE_COMPONENT_SERIALIZER(LightLabAvatarComponent, 1)
{
    (void)Ar;
    (void)C;
}

GAMECORE_DEFINE_COMPONENT_RPCS(LightLabAvatarComponent)
{
    Rpc.Server<&LightLabAvatarComponent::ServerSetPose>("ServerSetPose");
}

void LightLabAvatarComponent::ServerSetPose(const EngineCore::Vector3& InPosition, const EngineCore::Vector3& InRotation)
{
    /* NetCore has already checked the sender owns this orb. */
    if(GameCore::TransformComponent* Transform = Owner.GetSibling<GameCore::TransformComponent>())
    {
        Transform->Position = InPosition;
        Transform->Rotation = InRotation;
    }
}

namespace LightLabAvatar
{
    void Add(GameCore::World& InWorld, const GameCore::EntityID InPlayer)
    {
        const GameCore::Entity Player = InWorld.GetEntity(InPlayer);
        if(!Player.IsAlive()) { return; }

        GameCore::TransformComponent Transform;
        Transform.Scale = EngineCore::Vector3(OrbDiameter, OrbDiameter, OrbDiameter);
        Player.AddComponent(Transform);

        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", OrbMesh);
        std::snprintf(Render.Materials[0], sizeof(Render.Materials[0]), "%s", OrbMaterial);
        Player.AddComponent(Render);

        Player.AddComponent<LightLabAvatarComponent>();
    }

    void Update(GameCore::World& InWorld, const GameCore::EntityID InView, const double InDeltaSeconds)
    {
        if(!NimbleEngine::IsClient()) { return; }

        GSinceLastPose += InDeltaSeconds;
        const bool bSend = GSinceLastPose >= PoseIntervalSeconds;
        if(bSend) { GSinceLastPose = 0.0; }

        const GameCore::TransformComponent* View = InWorld.GetEntity(InView).GetComponent<GameCore::TransformComponent>();
        InWorld.Each<LightLabAvatarComponent>([&](GameCore::Entity InOrb, LightLabAvatarComponent&)
        {
            const GameCore::NetComponent* Net = InOrb.GetComponent<GameCore::NetComponent>();
            if(!Net || !NimbleEngine::IsLocallyOwned(*Net)) { return; }

            /* Our own: never drawn here (the camera is inside it), only steered. */
            if(RenderCore::RenderComponent* Render = InOrb.GetComponent<RenderCore::RenderComponent>()) { Render->bVisible = 0; }
            if(bSend && View)
            {
                GAMECORE_RPC(LightLabAvatarComponent, ServerSetPose)(InOrb, View->Position, View->Rotation);
            }
        });
    }
}

size_t LightLabAvatar::CountRemote(GameCore::World& InWorld)
{
    size_t Count = 0;
    InWorld.Each<LightLabAvatarComponent>([&](GameCore::Entity InOrb, LightLabAvatarComponent&)
    {
        const GameCore::NetComponent* Net = InOrb.GetComponent<GameCore::NetComponent>();
        if(Net && !NimbleEngine::IsLocallyOwned(*Net)) { ++Count; }
    });
    return Count;
}
