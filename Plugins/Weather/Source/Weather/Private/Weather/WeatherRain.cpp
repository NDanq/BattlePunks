#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>

#include "Public/Core/CoreConfig.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/CameraComponent.h"
#include "Public/RenderCore/Components/ParticleSystemComponent.h"
#include "Public/RenderCore/Components/SkyLightComponent.h"
#include "Public/RenderCore/CustomRenderPass.h"
#include "Public/RenderCore/RenderView.h"
#include "Public/Weather/Weather.h"
#include "Public/Weather/WeatherComponent.h"

/* The rain: a Particle System entity kept round the eye, its system the
 * shipped Rain.nmparticle blown by the wind, its spawn rate the component's
 * Rain, its colour the Sky Light's. The eye is the last frame's main view,
 * which a pass at BeforeScene reads on the render thread -- so the rain
 * follows the editor's viewport camera as well as a game's. */

/* Times every drop: 0 none (scalability), 0.5 half. */
NIMBLE_CONFIG_FLOAT(GWeatherRainDensity, "Weather", "RainDensity", 1.0f, nullptr);

namespace Weather
{
    namespace
    {
        /* The last main view's eye, render thread to game thread. */
        struct EyeBox
        {
            std::mutex          Mutex;
            bool                bSeen = false;
            EngineCore::Vector3 Eye{ 0.0, 0.0, 0.0 };
        };

        class EyePass final : public RenderCore::CustomRenderPass
        {
        public:
            explicit EyePass(std::shared_ptr<EyeBox> InBox) : Box(std::move(InBox)) {}

            const char* GetName() const override { return "WeatherEye"; }

            void Execute(RenderCore::RenderPassContext& InContext) override
            {
                std::lock_guard<std::mutex> Lock(Box->Mutex);
                Box->Eye   = InContext.GetView().Origin;
                Box->bSeen = true;
            }

        private:
            std::shared_ptr<EyeBox> Box;
        };

        /* A World's rain. Game thread only, as World hooks are. */
        struct RainState
        {
            RenderCore::RenderPassHandle Pass = 0;
            std::shared_ptr<EyeBox>      Eye;
            GameCore::EntityID           Entity;
            /* The system as last read from its file (or the editor's save),
             * and the blown copy stored over it: a new pointer from the cache
             * means someone replaced ours, so we blow that one. */
            std::shared_ptr<const RenderCore::ParticleSystemDesc> Base;
            std::shared_ptr<const RenderCore::ParticleSystemDesc> Stored;
            EngineCore::Vector3 StoredDrift{ 1e9, 0.0, 0.0 };
        };

        std::map<const GameCore::World*, RainState>& Rains()
        {
            static std::map<const GameCore::World*, RainState> Map;
            return Map;
        }

        uint8_t Byte(const float InValue)
        {
            return uint8_t((std::min)(255.0f, (std::max)(0.0f, std::round(InValue))));
        }

        /* The drops' colour: the sky's light, which is most of what lights
         * falling water; not under a dim sky's tenth, for a night. */
        EngineCore::Color RainTint(GameCore::World& InWorld)
        {
            EngineCore::Color Tint(200, 205, 215, 255);
            bool bFound = false;
            InWorld.Each<RenderCore::SkyLightComponent>([&](GameCore::Entity, RenderCore::SkyLightComponent& InSky)
            {
                if(bFound) { return; }
                bFound = true;
                const float Scale = (std::min)(1.0f, (std::max)(0.1f, InSky.Intensity * 1.5f));
                Tint = EngineCore::Color(Byte(InSky.SkyColor.R * Scale), Byte(InSky.SkyColor.G * Scale),
                                         Byte(InSky.SkyColor.B * Scale), 255);
            });
            return Tint;
        }

        bool Moved(const EngineCore::Vector3& InA, const EngineCore::Vector3& InB, const double InBy)
        {
            const EngineCore::Vector3 D = InA - InB;
            return D.GetX() * D.GetX() + D.GetY() * D.GetY() + D.GetZ() * D.GetZ() > InBy * InBy;
        }

        /* The system under RAIN_SYSTEM_PATH blown by InDrift; re-stored when
         * the drift changes or the cache holds someone else's. */
        void StoreRainSystem(RainState& InOutRain, const EngineCore::Vector3& InDrift)
        {
            const std::shared_ptr<const RenderCore::ParticleSystemDesc> Current = RenderCore::ParticleSystemCache::Find(RAIN_SYSTEM_PATH);
            if(!Current)
            {
                return;   // the file is missing: warned once by the cache
            }
            const bool bReplaced = Current != InOutRain.Stored;
            if(bReplaced)
            {
                InOutRain.Base = Current;   // the file, or an editor save of it
            }
            if(!bReplaced && !Moved(InDrift, InOutRain.StoredDrift, 5.0))
            {
                return;
            }
            std::shared_ptr<const RenderCore::ParticleSystemDesc> Blown = BlowRain(*InOutRain.Base, InDrift);
            RenderCore::ParticleSystemCache::Store(RAIN_SYSTEM_PATH, Blown);
            InOutRain.Stored      = Blown;
            InOutRain.StoredDrift = InDrift;
        }
    }

    std::shared_ptr<RenderCore::ParticleSystemDesc> BlowRain(const RenderCore::ParticleSystemDesc& InBase,
                                                             const EngineCore::Vector3& InDrift)
    {
        std::shared_ptr<RenderCore::ParticleSystemDesc> Out = std::make_shared<RenderCore::ParticleSystemDesc>(InBase);
        for(RenderCore::ParticleEmitterDesc& Emitter : Out->Emitters)
        {
            Emitter.Velocity = Emitter.Velocity + InDrift;
            Emitter.Modules  = uint8_t(Emitter.Modules | RenderCore::PEM_Velocity);
        }
        return Out;
    }

    EngineCore::Vector3 RainOrigin(const EngineCore::Vector3& InEye, const EngineCore::Vector3& InDrift)
    {
        const double Fall = double(RAIN_HEIGHT) / double(RAIN_FALL_SPEED);   // seconds to the eye's height
        return EngineCore::Vector3(InEye.GetX() - InDrift.GetX() * Fall, InEye.GetY() + double(RAIN_HEIGHT),
                                   InEye.GetZ() - InDrift.GetZ() * Fall);
    }

    bool GetRainEye(GameCore::World& InWorld, EngineCore::Vector3& OutEye)
    {
        const auto Found = Rains().find(&InWorld);
        if(Found != Rains().end() && Found->second.Eye)
        {
            std::lock_guard<std::mutex> Lock(Found->second.Eye->Mutex);
            if(Found->second.Eye->bSeen)
            {
                OutEye = Found->second.Eye->Eye;
                return true;
            }
        }
        bool bCamera = false;
        InWorld.Each<RenderCore::CameraComponent>([&](GameCore::Entity InEntity, RenderCore::CameraComponent& InCamera)
        {
            if(bCamera || !InCamera.bActive) { return; }
            bCamera = true;
            OutEye  = InEntity.GetWorldPose().Position;
        });
        if(!bCamera) { OutEye = EngineCore::Vector3(0.0, 0.0, 0.0); }
        return bCamera;
    }

    void AttachWorld(GameCore::World& InWorld)
    {
        if(Rains().count(&InWorld) != 0)
        {
            return;
        }
        /* The eye's pass waits for the first Weather (UpdateRain): a World
         * without one carries nothing of this plugin's. */
        Rains()[&InWorld].Eye = std::make_shared<EyeBox>();
    }

    void DetachWorld(GameCore::World& InWorld)
    {
        RenderCore::ClearRenderWeather(&InWorld);
        const auto Found = Rains().find(&InWorld);
        if(Found == Rains().end())
        {
            return;
        }
        if(Found->second.Pass != 0)
        {
            RenderCore::UnregisterCustomRenderPass(Found->second.Pass);
        }
        if(Found->second.Entity.IsSet() && InWorld.IsAlive(Found->second.Entity))
        {
            InWorld.DestroyEntity(Found->second.Entity);
        }
        Rains().erase(Found);
    }

    void UpdateRain(GameCore::World& InWorld, const WeatherComponent* InWeather)
    {
        const auto Found = Rains().find(&InWorld);
        if(Found == Rains().end())
        {
            return;   // not attached: a World the host never hooked
        }
        RainState& Rain = Found->second;
        if(InWeather && Rain.Pass == 0)
        {
            Rain.Pass = RenderCore::RegisterCustomRenderPass(&InWorld, std::make_shared<EyePass>(Rain.Eye),
                                                             RenderCore::ERenderHook::BeforeScene, -1000);
        }

        const float Density = (std::max)(0.0f, GWeatherRainDensity.GetFloat());
        const float Amount  = InWeather && InWeather->bRainParticles ? InWeather->Rain * Density : 0.0f;
        RenderCore::ParticleSystemComponent* Emitter = nullptr;
        if(Rain.Entity.IsSet() && InWorld.IsAlive(Rain.Entity))
        {
            Emitter = InWorld.GetEntity(Rain.Entity).GetComponent<RenderCore::ParticleSystemComponent>();
        }
        if(Amount <= 0.0f)
        {
            if(Emitter) { Emitter->bEmitting = 0; }   // the drops out fall on
            return;
        }

        const EngineCore::Vector3 Drift = WindDirection(InWeather->WindYaw) * double(RAIN_WIND_SPEED * (std::max)(0.0f, InWeather->WindStrength));
        StoreRainSystem(Rain, Drift);

        if(!Emitter)
        {
            GameCore::Entity Entity = InWorld.CreateEntity("WeatherRain");
            Entity.AddComponent<GameCore::TransformComponent>();
            RenderCore::ParticleSystemComponent Init;
            std::snprintf(Init.System, sizeof(Init.System), "%s", RAIN_SYSTEM_PATH);
            Emitter     = Entity.AddComponent(Init);
            Rain.Entity = Entity.GetID();
        }
        if(!Emitter)
        {
            return;
        }

        EngineCore::Vector3 Eye;
        GetRainEye(InWorld, Eye);
        if(GameCore::TransformComponent* Transform = InWorld.GetEntity(Rain.Entity).GetComponent<GameCore::TransformComponent>())
        {
            Transform->SetWorldPosition(RainOrigin(Eye, Drift));
        }
        Emitter->bEmitting      = 1;
        Emitter->SpawnRateScale = Amount;
        Emitter->Tint           = RainTint(InWorld);
    }
}
