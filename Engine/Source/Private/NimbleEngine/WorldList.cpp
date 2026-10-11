#include "Public/NimbleEngine/WorldList.h"

#include <cctype>
#include <cstring>
#include <memory>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/PlayerStart.h"
#include "Public/NimbleEngine/SaveGame.h"

/* The project's world list, the game-mode registry, and the level each
 * listed world becomes (WorldLevel). See WorldList.h. */

namespace NimbleEngine
{
namespace
{
    bool SameText(const std::string& InA, const char* InB)
    {
        const size_t Length = std::strlen(InB);
        if(InA.size() != Length) { return false; }
        for(size_t Index = 0; Index < Length; ++Index)
        {
            if(std::tolower(static_cast<unsigned char>(InA[Index])) != std::tolower(static_cast<unsigned char>(InB[Index])))
            {
                return false;
            }
        }
        return true;
    }

    bool ParseBool(const std::string& InText, const bool bInDefault)
    {
        if(SameText(InText, "true") || SameText(InText, "1") || SameText(InText, "yes") || SameText(InText, "on"))   { return true; }
        if(SameText(InText, "false") || SameText(InText, "0") || SameText(InText, "no") || SameText(InText, "off")) { return false; }
        return bInDefault;
    }

    // === Game modes =========================================================

    /* A plain array, as the level registry: registering from a module's
     * OnStartup needs nothing constructed first. */
    enum : size_t { MAX_GAME_MODES = 64 };

    struct GameModeType
    {
        const char*       Name    = nullptr;
        GameModeCreateFn  Create  = nullptr;
        GameModeDestroyFn Destroy = nullptr;
    };

    GameModeType GGameModes[MAX_GAME_MODES] = {};
    size_t       GGameModeCount = 0;

    const GameModeType* FindGameMode(const char* InName)
    {
        if(InName == nullptr) { return nullptr; }
        for(size_t Index = 0; Index < GGameModeCount; ++Index)
        {
            if(SameText(GGameModes[Index].Name, InName)) { return &GGameModes[Index]; }
        }
        return nullptr;
    }

    // === The level a listed world becomes ===================================

    class WorldLevel final : public ILevel
    {
    public:
        explicit WorldLevel(WorldEntry InEntry) : Entry(std::move(InEntry)) {}

        const char* GetName() const override      { return Entry.Name.c_str(); }
        const char* GetWorldFile() const override { return Entry.File.c_str(); }

        const char* GetStartupScript() const override { return Entry.StartupScript.c_str(); }

        bool OnLoad(GameSession& InSession) override
        {
            (void)InSession;
            if(!GameCore::LoadContentWorld(Entry.File.c_str(), GetWorld()))
            {
                CORE_LOG_ERROR("[Level] '" + Entry.Name + "' could not load its world " + Entry.File)
                return false;
            }

            /* After the world: the mode's OnLoad (GameSession) sees it loaded. */
            if(!Entry.GameMode.empty())
            {
                Type = FindGameMode(Entry.GameMode.c_str());
                if(!Type)
                {
                    CORE_LOG_ERROR("[Level] '" + Entry.Name + "' names game mode '" + Entry.GameMode
                                   + "', which the game module does not register")
                    return false;
                }
                Mode = Type->Create();
            }
            return true;
        }

        void OnUnload() override
        {
            /* GameSession has run the mode's OnUnload already. */
            if(Mode && Type)
            {
                Type->Destroy(Mode);
            }
            Mode = nullptr;
            Type = nullptr;
        }

        void OnPrepareForLoadedGame() override
        {
            if(Mode) { Mode->OnPrepareForLoadedGame(); }
        }

        IGameMode* GetGameMode() override  { return Mode; }
        GameState* GetGameState() override { return Mode ? Mode->GetGameState() : nullptr; }

    private:
        WorldEntry          Entry;
        const GameModeType* Type = nullptr;
        IGameMode*          Mode = nullptr;
    };

    /* Owned here, so their names (Entry.Name) live as long as they are registered. */
    std::vector<std::unique_ptr<WorldLevel>>& WorldLevels()
    {
        static std::vector<std::unique_ptr<WorldLevel>>* Levels = new std::vector<std::unique_ptr<WorldLevel>>();
        return *Levels;
    }
}

std::vector<WorldEntry> ReadWorldList()
{
    std::vector<WorldEntry> List;
    EngineCore::ConfigForEachEntry(WORLD_SECTION_PREFIX,
        [](const char* InSection, const char* InKey, const char* InValue, void* InUser)
        {
            auto& Out = *static_cast<std::vector<WorldEntry>*>(InUser);
            const std::string Name = std::string(InSection).substr(std::strlen(WORLD_SECTION_PREFIX));
            WorldEntry* Entry = nullptr;
            for(WorldEntry& Each : Out)
            {
                if(SameText(Each.Name, Name.c_str())) { Entry = &Each; break; }
            }
            if(!Entry)
            {
                Out.push_back(WorldEntry());
                Entry = &Out.back();
                Entry->Name = Name;
            }

            const std::string Key = InKey;
            if(SameText(Key, "File"))               { Entry->File = InValue; }
            else if(SameText(Key, "GameMode"))      { Entry->GameMode = InValue; }
            else if(SameText(Key, "StartupScript")) { Entry->StartupScript = InValue; }
            else if(SameText(Key, "Cook"))          { Entry->bCook = ParseBool(InValue, true); }
            else
            {
                CORE_LOG_WARN(std::string("[Worlds] ") + InSection + "." + InKey + " is not a world setting "
                              "(File, GameMode, StartupScript, Cook); ignored")
            }
        }, &List);

    for(auto It = List.begin(); It != List.end();)
    {
        if(It->Name.empty() || It->File.empty())
        {
            CORE_LOG_ERROR("[Worlds] [" + std::string(WORLD_SECTION_PREFIX) + It->Name + "] has no File; it is not a level")
            It = List.erase(It);
            continue;
        }
        ++It;
    }
    return List;
}

void RegisterGameMode(const char* InName, const GameModeCreateFn InCreate, const GameModeDestroyFn InDestroy)
{
    if(InName == nullptr || InName[0] == '\0' || !InCreate || !InDestroy)
    {
        CORE_LOG_ERROR("RegisterGameMode: a mode needs a name and both functions; ignoring it")
        return;
    }
    if(FindGameMode(InName))
    {
        CORE_LOG_ERROR(std::string("RegisterGameMode: '") + InName + "' is already registered; keeping the first")
        return;
    }
    if(GGameModeCount >= MAX_GAME_MODES)
    {
        CORE_LOG_ERROR(std::string("RegisterGameMode: the registry is full; '") + InName + "' is not registered")
        return;
    }
    GGameModes[GGameModeCount++] = GameModeType{ InName, InCreate, InDestroy };
}

void UnregisterGameMode(const char* InName)
{
    for(size_t Index = 0; Index < GGameModeCount; ++Index)
    {
        if(!SameText(GGameModes[Index].Name, InName)) { continue; }
        /* Shifted, not swapped: the editor lists them in registration order. */
        for(size_t Next = Index + 1; Next < GGameModeCount; ++Next)
        {
            GGameModes[Next - 1] = GGameModes[Next];
        }
        GGameModes[--GGameModeCount] = GameModeType();
        return;
    }
}

bool HasGameMode(const char* InName) { return FindGameMode(InName) != nullptr; }

size_t GetGameModeCount() { return GGameModeCount; }

const char* GetGameModeName(const size_t InIndex)
{
    return InIndex < GGameModeCount ? GGameModes[InIndex].Name : nullptr;
}

bool IsNetworkedGameMode(const char* InName)
{
    const GameModeType* Type = FindGameMode(InName);
    if(!Type)
    {
        return false;
    }
    IGameMode* Mode = Type->Create();
    const bool bNetworked = dynamic_cast<NetGameMode*>(Mode) != nullptr;
    Type->Destroy(Mode);
    return bNetworked;
}

void RegisterWorldLevels()
{
    UnregisterWorldLevels();

    /* The project-wide script is gone: each world names its own. */
    EngineCore::ConfigForEachEntry("Scripting", [](const char* InSection, const char* InKey, const char* InValue, void*)
    {
        if(SameText(InSection, "Scripting") && SameText(InKey, "StartupScript") && InValue[0] != '\0')
        {
            CORE_LOG_WARN(std::string("[Worlds] [Scripting] StartupScript=") + InValue + " is no longer read: set StartupScript "
                          "on each world that runs it ([World_*], Project Settings > Worlds)")
        }
    }, nullptr);

    for(WorldEntry& Entry : ReadWorldList())
    {
        if(!Entry.GameMode.empty() && !HasGameMode(Entry.GameMode.c_str()))
        {
            /* Still listed: its load fails with the same message. */
            CORE_LOG_ERROR("[Worlds] '" + Entry.Name + "' names game mode '" + Entry.GameMode
                           + "', which the game module does not register")
        }
        WorldLevels().push_back(std::make_unique<WorldLevel>(std::move(Entry)));
        RegisterLevel(*WorldLevels().back());
    }
    if(WorldLevels().empty())
    {
        CORE_LOG_WARN("[Worlds] the project lists no worlds -- add them in the editor's Project Settings > Worlds")
    }
}

void UnregisterWorldLevels()
{
    for(const std::unique_ptr<WorldLevel>& Level : WorldLevels())
    {
        UnregisterLevel(*Level);
    }
    WorldLevels().clear();
}
}
