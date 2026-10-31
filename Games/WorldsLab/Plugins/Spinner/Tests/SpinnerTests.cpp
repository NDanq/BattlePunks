/*
 * SpinnerTests -- the plugin guide's worked example, as a game runs it: the
 * build's manifest beside this executable, the plugin's config, its library
 * started by the host, and its system turning an entity on a World.
 */
#include <cmath>
#include <cstdio>
#include <string>

#include "Public/Core/CoreConfig.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Plugin.h"
#include "Public/Spinner/SpinnerComponent.h"

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const std::string& What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What.c_str());
    }
}

int main()
{
    EngineCore::FilesystemProvider  Disk;
    EngineCore::LayeredFileProvider Assets(&Disk, false);
    EngineCore::SetFileProvider(&Assets);

    Expect(NimbleEngine::Plugins::Discover(std::string(), false) >= 1, "the manifest lists the plugins");
    NimbleEngine::Plugins::MountContent(Assets);
    EngineCore::LoadConfigText("[Spinner]\nSpeedScale=2\n");   // as a project's file would
    Expect(NimbleEngine::Plugins::LoadModules(), "the libraries load");
    Expect(Spinner::IsStarted() && NimbleEngine::Plugins::IsLoaded("Spinner"), "Spinner started");

    GameCore::World World;
    NimbleEngine::Plugins::AttachWorld(World);
    GameCore::Entity Top = World.CreateEntity("Top");
    Top.AddComponent<GameCore::TransformComponent>();
    Spinner::SpinnerComponent Init;
    Init.DegreesPerSecond = 30.0f;
    Top.AddComponent(Init);

    World.Tick(1.5f);
    const float Yaw = float(Top.GetComponent<GameCore::TransformComponent>()->Rotation.GetY());
    Expect(std::fabs(Yaw - 90.0f) < 0.01f, "30 deg/s x SpeedScale 2 x 1.5 s turns it 90 degrees");

    NimbleEngine::Plugins::DetachWorld(World);
    NimbleEngine::Plugins::UnmountContent(Assets);
    NimbleEngine::Plugins::Close();
    Expect(!Spinner::IsStarted(), "Close shut it down");

    EngineCore::SetFileProvider(nullptr);
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
