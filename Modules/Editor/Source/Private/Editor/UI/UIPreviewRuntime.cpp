#if WITH_EDITOR

#include "Private/Editor/UI/UIPreviewRuntime.h"

#include "Public/Core/CoreConfig.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/UI/UISystem.h"

UIPreviewRuntime::UIPreviewRuntime() = default;

UIPreviewRuntime::~UIPreviewRuntime()
{
    /* Stop(InSystem) first, while the system exists; this only lets go. */
    if(Scripts)
    {
        NimbleEngine::SetScriptPreviewWorld(nullptr);
        Scripts->UnbindWorld();
        Scripts->Shutdown();
    }
}

bool UIPreviewRuntime::Start(UI::UISystem& InSystem)
{
    if(Scripts) return true;

    /* The config as it is: what the preview's graph changes is put back. */
    Config.clear();
    for(const EngineCore::ConfigVar* Var = EngineCore::ConfigGetFirst(); Var; Var = Var->GetNext())
    {
        Config.push_back({ Var->GetSection(), Var->GetName(), Var->ToString() });
    }

    World   = std::make_unique<GameCore::World>();
    Scripts = std::make_unique<GameCore::ScriptEngine>();
    if(!Scripts->Initialize())
    {
        Scripts.reset();
        World.reset();
        return false;
    }
    Scripts->BindWorld(World.get());   // the libraries: UI, Config, Game, Input, Events...
    NimbleEngine::SetScriptPreviewWorld(World.get());
    ErrorsAtStart = Scripts->GetErrorCount();

    InSystem.SetScriptEngine(Scripts.get());
    InSystem.SetWorld(World.get());
    InSystem.SetExecuteActions(true);
    return true;
}

void UIPreviewRuntime::Stop(UI::UISystem& InSystem)
{
    if(!Scripts) return;
    InSystem.CloseAll(true);              // OnClose, while the engine is here
    InSystem.RunQueuedActions();
    InSystem.SetScriptEngine(nullptr);    // the graph objects go
    InSystem.SetWorld(nullptr);
    InSystem.SetExecuteActions(false);

    NimbleEngine::SetScriptPreviewWorld(nullptr);
    Scripts->UnbindWorld();
    Scripts->Shutdown();
    Scripts.reset();
    World.reset();

    for(const SavedVar& Each : Config)
    {
        EngineCore::ConfigVar* Var = EngineCore::ConfigFindVar(Each.Section.c_str(), Each.Name.c_str());
        if(Var && !Var->HoldsText(Each.Value)) Var->SetFromString(Each.Value);
    }
    Config.clear();
}

uint32_t UIPreviewRuntime::GetErrorCount() const
{
    return Scripts ? Scripts->GetErrorCount() - ErrorsAtStart : 0;
}

void UIPreviewRuntime::ForgetGraph(const std::string& InGraphPath)
{
    if(Scripts && !InGraphPath.empty()) Scripts->ForgetClass(InGraphPath.c_str());
}

#endif // WITH_EDITOR
