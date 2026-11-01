#include "World/LabMotion.h"

#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/ParticleSystemComponent.h"
#include "Public/RenderCore/Components/PointLightComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/Components/SpotLightComponent.h"

#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif

#include "World/LabLogic.h"
#include "World/LabSignals.h"
#include "LabVersion.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace GameCore;
using EngineCore::Vector3;

GAMECORE_DEFINE_COMPONENT(LabMotionComponent, "LabMotion")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabMotionComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("Spin", C.Spin);
    Ar.Field("SpinOn", C.SpinOn);
    Ar.Field("AlarmOn", C.AlarmOn);
    Ar.Field("AlarmScale", C.AlarmScale);
    Ar.Field("Flicker", C.Flicker);
    Ar.Field("FlickerDuty", C.FlickerDuty);
    Ar.Field("RampOn", C.RampOn);
    Ar.Field("RampSeconds", C.RampSeconds);
    Ar.Field("RampParam", C.RampParam);
    Ar.Field("RampSlot", C.RampSlot);
    Ar.Field("RampFrom", C.RampFrom);
    Ar.Field("RampTo", C.RampTo);
    Ar.Field("RampLight", C.RampLight);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabMotionComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("Spin", &LabMotionComponent::Spin);
    Props.Field("SpinOn", &LabMotionComponent::SpinOn);
    Props.Field("AlarmOn", &LabMotionComponent::AlarmOn);
    Props.Field("AlarmScale", &LabMotionComponent::AlarmScale);
    Props.Field("Flicker", &LabMotionComponent::Flicker);
    Props.Field("FlickerDuty", &LabMotionComponent::FlickerDuty);
    Props.Field("RampOn", &LabMotionComponent::RampOn);
    Props.Field("RampSeconds", &LabMotionComponent::RampSeconds);
    Props.Field("RampParam", &LabMotionComponent::RampParam);
    Props.Field("RampSlot", &LabMotionComponent::RampSlot);
    Props.Field("RampFrom", &LabMotionComponent::RampFrom);
    Props.Field("RampTo", &LabMotionComponent::RampTo);
    Props.Field("RampLight", &LabMotionComponent::RampLight);
}
#endif

namespace
{
    /* A repeatable random number in [0, 1) for an entity and a time slot. */
    float Hash01(const uint64_t InA, const uint64_t InB)
    {
        uint64_t X = InA * 0x9E3779B97F4A7C15ull ^ (InB + 0x632BE59BD9B4E019ull);
        X ^= X >> 31; X *= 0xBF58476D1CE4E5B9ull; X ^= X >> 27; X *= 0x94D049BB133111EBull; X ^= X >> 33;
        return float(X >> 40) / float(1ull << 24);
    }

    float Wrap360(const float InDegrees)
    {
        const float W = std::fmod(InDegrees, 360.f);
        return W < 0.f ? W + 360.f : W;
    }
}

void LabMotion::Tick(World& InWorld, const float InSeconds)
{
    InWorld.Each<LabMotionComponent>([&](Entity InEntity, LabMotionComponent& M)
    {
        TransformComponent* Transform = InEntity.GetComponent<TransformComponent>();
        RenderCore::RenderComponent* Render = InEntity.GetComponent<RenderCore::RenderComponent>();
        RenderCore::PointLightComponent* Point = InEntity.GetComponent<RenderCore::PointLightComponent>();
        RenderCore::SpotLightComponent* Spot = InEntity.GetComponent<RenderCore::SpotLightComponent>();
        RenderCore::ParticleSystemComponent* Particles = InEntity.GetComponent<RenderCore::ParticleSystemComponent>();
        if(!M.bStarted)
        {
            M.bStarted = 1;
            if(Transform) { M.Rest = Transform->Rotation; }
            M.LightRest = Point ? Point->Intensity : (Spot ? Spot->Intensity : 0.f);
            if(Particles) { M.TintRest = Particles->Tint.A; }
        }
        M.Clock += InSeconds;

        /* Spin. */
        if(Transform && (M.Spin.GetX() != 0.0 || M.Spin.GetY() != 0.0 || M.Spin.GetZ() != 0.0))
        {
            const bool bSpin = !M.SpinOn[0] || LabSignals::Get(InWorld, M.SpinOn);
            if(bSpin)
            {
                const float Scale = (M.AlarmOn[0] && LabSignals::Get(InWorld, M.AlarmOn)) ? M.AlarmScale : 1.f;
                M.Angle = Vector3(Wrap360(float(M.Angle.GetX() + M.Spin.GetX() * InSeconds * Scale)),
                                  Wrap360(float(M.Angle.GetY() + M.Spin.GetY() * InSeconds * Scale)),
                                  Wrap360(float(M.Angle.GetZ() + M.Spin.GetZ() * InSeconds * Scale)));
                Transform->Rotation = Vector3(M.Rest.GetX() + M.Angle.GetX(), M.Rest.GetY() + M.Angle.GetY(),
                                              M.Rest.GetZ() + M.Angle.GetZ());
            }
        }

        /* Ramp. */
        float LightScale = 1.f;
        bool bRampLight = false;
        if(M.RampOn[0])
        {
            if(LabSignals::Get(InWorld, M.RampOn)) { M.Ramp = std::min(1.f, M.Ramp + InSeconds / std::max(0.01f, M.RampSeconds)); }
            if(Render && M.RampParam[0])
            {
                const float V = M.RampFrom + (M.RampTo - M.RampFrom) * M.Ramp;
                Render->SetMaterialParameter(M.RampSlot, M.RampParam, V, V, V, V);
            }
            if(M.RampLight > 0.f) { bRampLight = true; }
        }

        /* Flicker. */
        bool bOn = true;
        float Jump = 1.f;
        if(M.Flicker > 0.f)
        {
            const uint64_t Slot = uint64_t(M.Clock * M.Flicker);
            const uint64_t Id = InEntity.GetID().Pack();
            bOn = Hash01(Id, Slot) < M.FlickerDuty;
            Jump = 0.55f + 0.9f * Hash01(Id + 7, Slot);
            if(Render)
            {
                bool bWanted = true;
                if(const LabSignalToggleComponent* Toggle = InEntity.GetComponent<LabSignalToggleComponent>())
                {
                    const bool bSig = LabSignals::Get(InWorld, Toggle->Channel);
                    bWanted = Toggle->bShowWhenOn ? bSig : !bSig;
                }
                const uint8_t Visible = (bWanted && bOn) ? 1 : 0;
                if(Render->bVisible != Visible) { Render->bVisible = Visible; }
            }
            if(Particles)
            {
                Particles->Tint.A = bOn ? uint8_t(std::min(255.f, float(M.TintRest) * std::min(1.f, Jump))) : 0;
            }
        }
        if(bRampLight || M.Flicker > 0.f)
        {
            LightScale = (bRampLight ? M.Ramp * M.RampLight / std::max(0.001f, M.LightRest) : 1.f) * (bOn ? Jump : 0.f);
            if(Point) { Point->Intensity = M.LightRest * LightScale; }
            if(Spot)  { Spot->Intensity = M.LightRest * LightScale; }
        }
    });
}
