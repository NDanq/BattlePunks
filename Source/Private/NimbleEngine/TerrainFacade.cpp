#include "Public/NimbleEngine/TerrainFacade.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/Terrain.h"
#include "Public/GameCore/World.h"
#include "Public/Math/Matrix.h"
#include "Public/RenderCore/Components/TerrainComponent.h"

/* TerrainFacade.h has the contract. */

namespace
{
    /* InMatrix times the point (x, y, z, 1). */
    void TransformPoint(const EngineCore::Matrix4x4& InMatrix, const double InX, const double InY, const double InZ, float (&Out)[3])
    {
        for(int Row = 0; Row < 3; ++Row)
        {
            Out[Row] = float(double(InMatrix.Data[Row][0]) * InX + double(InMatrix.Data[Row][1]) * InY
                             + double(InMatrix.Data[Row][2]) * InZ + double(InMatrix.Data[Row][3]));
        }
    }

    /* InMatrix times the direction (x, y, z, 0). */
    void TransformDirection(const EngineCore::Matrix4x4& InMatrix, const double InX, const double InY, const double InZ, float (&Out)[3])
    {
        for(int Row = 0; Row < 3; ++Row)
        {
            Out[Row] = float(double(InMatrix.Data[Row][0]) * InX + double(InMatrix.Data[Row][1]) * InY
                             + double(InMatrix.Data[Row][2]) * InZ);
        }
    }

    /* A normal from terrain space to world: the inverse transpose, i.e. InInverse's columns. */
    EngineCore::Vector3 NormalToWorld(const EngineCore::Matrix4x4& InInverse, const float (&InNormal)[3])
    {
        double N[3];
        for(int Column = 0; Column < 3; ++Column)
        {
            N[Column] = double(InInverse.Data[0][Column]) * InNormal[0] + double(InInverse.Data[1][Column]) * InNormal[1]
                      + double(InInverse.Data[2][Column]) * InNormal[2];
        }
        const double Length = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
        if(Length <= 1e-12) { return EngineCore::Vector3(0.0, 1.0, 0.0); }
        return EngineCore::Vector3(N[0] / Length, N[1] / Length, N[2] / Length);
    }

    std::shared_ptr<GameCore::TerrainData> FindTerrain(GameCore::World& InWorld, const GameCore::EntityID InEntity, void*)
    {
        const auto* Terrain = InWorld.GetEntity(InEntity).GetComponent<RenderCore::TerrainComponent>();
        if(!Terrain || Terrain->Terrain[0] == '\0') { return nullptr; }
        return GameCore::TerrainAssetCache::Find(Terrain->Terrain);
    }

    struct TerrainProviderRegistrar
    {
        TerrainProviderRegistrar()  { NimbleEngine::InstallTerrainProvider(); }
        ~TerrainProviderRegistrar() { GameCore::SetTerrainProvider(nullptr); }
    };
    TerrainProviderRegistrar GTerrainProviderRegistrar;
}

namespace NimbleEngine
{
    bool RaycastTerrain(GameCore::World& InWorld, const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDirection,
                        const float InMaxDistance, TerrainRaycastHit& OutHit)
    {
        OutHit = TerrainRaycastHit();
        const double Length = std::sqrt(double(InDirection.GetX()) * InDirection.GetX() + double(InDirection.GetY()) * InDirection.GetY()
                                        + double(InDirection.GetZ()) * InDirection.GetZ());
        if(Length <= 0.0 || !(InMaxDistance > 0.0f)) { return false; }
        const double UX = InDirection.GetX() / Length, UY = InDirection.GetY() / Length, UZ = InDirection.GetZ() / Length;

        bool  bHit = false;
        float Best = InMaxDistance;
        InWorld.Each<RenderCore::TerrainComponent>([&](GameCore::Entity InEntity, RenderCore::TerrainComponent& InTerrain)
        {
            if(!InTerrain.bWalkable || InTerrain.Terrain[0] == '\0') { return; }
            const std::shared_ptr<GameCore::TerrainData> Data = GameCore::TerrainAssetCache::Find(InTerrain.Terrain);
            if(!Data || Data->IsEmpty()) { return; }

            /* Into terrain space: t along the local ray is t along the world one, as the direction maps linearly. */
            const EngineCore::Matrix4x4 World   = InEntity.GetWorldMatrix();
            const EngineCore::Matrix4x4 Inverse = EngineCore::Matrix4x4::Inverse(World);
            float Origin[3];
            float Direction[3];
            TransformPoint(Inverse, InOrigin.GetX(), InOrigin.GetY(), InOrigin.GetZ(), Origin);
            TransformDirection(Inverse, UX, UY, UZ, Direction);

            float Distance = 0.0f;
            float Normal[3];
            if(!Data->Raycast(Origin, Direction, Best, Distance, Normal) || Distance > Best) { return; }
            Best = Distance;
            bHit = true;
            OutHit.Entity   = InEntity.GetID();
            OutHit.Distance = Distance;
            OutHit.Point    = EngineCore::Vector3(InOrigin.GetX() + UX * Distance, InOrigin.GetY() + UY * Distance,
                                                  InOrigin.GetZ() + UZ * Distance);
            OutHit.Normal   = NormalToWorld(Inverse, Normal);
        });
        return bHit;
    }

    bool SampleGround(GameCore::World& InWorld, const EngineCore::Vector3& InPosition, float& OutHeight,
                      EngineCore::Vector3* OutNormal, const float InSearchAbove)
    {
        const float Above = std::max(InSearchAbove, 0.0f);
        const EngineCore::Vector3 From(InPosition.GetX(), InPosition.GetY() + double(Above), InPosition.GetZ());
        TerrainRaycastHit Hit;
        /* Down to any depth: a few kilometres under the start is past any terrain. */
        if(!RaycastTerrain(InWorld, From, EngineCore::Vector3(0.0, -1.0, 0.0), Above + 1.0e7f, Hit)) { return false; }
        OutHeight = float(Hit.Point.GetY());
        if(OutNormal) { *OutNormal = Hit.Normal; }
        return true;
    }

    void InstallTerrainProvider()
    {
        GameCore::TerrainProvider Provider;
        Provider.FindTerrain = &FindTerrain;
        GameCore::SetTerrainProvider(&Provider);
    }
}

/*
 * The "Terrain" script library: TerrainFacade.h for Lua and script graphs.
 */
GAMECORE_DEFINE_SCRIPT_LIBRARY(Terrain)
{
    Lib.Function("SampleGround", [](GameCore::ScriptCall& Call)
    {
        const EngineCore::Vector3 At = Call.GetVector3(1);
        if(Call.HasError()) { return; }
        float               Height = 0.0f;
        EngineCore::Vector3 Normal(0.0, 1.0, 0.0);
        const bool bHit = NimbleEngine::SampleGround(Call.GetWorld(), At, Height, &Normal);
        Call.PushBool(bHit);
        Call.PushNumber(bHit ? double(Height) : At.GetY());
        Call.PushVector3(Normal);
    }, "At:Vector3 -> Hit:bool, Height:number, Normal:Vector3")
       .Tooltip("The walkable terrain's surface height and normal straight below At (looking from well above it)");

    Lib.Function("Raycast", [](GameCore::ScriptCall& Call)
    {
        const EngineCore::Vector3 Origin    = Call.GetVector3(1);
        const EngineCore::Vector3 Direction = Call.GetVector3(4);
        const float               Distance  = Call.GetFloat(7);
        if(Call.HasError()) { return; }
        NimbleEngine::TerrainRaycastHit Hit;
        const bool bHit = NimbleEngine::RaycastTerrain(Call.GetWorld(), Origin, Direction, Distance, Hit);
        Call.PushBool(bHit);
        Call.PushEntity(Hit.Entity);
        Call.PushVector3(Hit.Point);
    }, "From:Vector3, Dir:Vector3, Len:number -> Hit:bool, Entity:Entity, At:Vector3")
       .Tooltip("The nearest walkable terrain along the ray within Len world units");
}
