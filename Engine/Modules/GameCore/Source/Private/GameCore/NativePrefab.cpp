#include "Public/GameCore/NativePrefab.h"

#include <cstring>
#include <string>
#include <vector>

#include "Public/GameCore/GameCoreLog.h"

/*
 * The native prefab registry. A vector, not a map: a game has a handful, and
 * registration order is what GetNativePrefabName lists. Constant-initialised
 * through a function-local static, so a static-init registrar in another
 * image finds it ready.
 */
namespace GameCore
{
namespace
{
    struct NativeEntry
    {
        std::string          Name;
        NativePrefabBuildFn  Build = nullptr;
        NativePrefabBehavior Behavior;
    };

    std::vector<NativeEntry>& Entries()
    {
        static std::vector<NativeEntry> List;
        return List;
    }

    uint32_t& Generation()
    {
        static uint32_t Value = 1;
        return Value;
    }

    NativeEntry* Find(const char* InName)
    {
        if(!InName) { return nullptr; }
        for(NativeEntry& Each : Entries())
        {
            if(Each.Name == InName) { return &Each; }
        }
        return nullptr;
    }
}

// === PrefabBuilder ===========================================================

PrefabBuilder::PrefabBuilder(World& InScratch)
    : Scratch(&InScratch)
{
}

PrefabBuilder::Node PrefabBuilder::Root(const char* InName)
{
    if(!RootID.IsSet())
    {
        RootID = Scratch->CreateEntity(InName ? InName : "Root").GetID();
    }
    Node Made;
    Made.Handle = Scratch->GetEntity(RootID);
    Made.Owner  = this;
    return Made;
}

PrefabBuilder::Node PrefabBuilder::Node::Child(const char* InName) const
{
    Node Made;
    Made.Owner  = Owner;
    Made.Handle = Owner->Scratch->CreateEntity(InName ? InName : "Entity");
    Made.Handle.SetParent(Handle.GetID(), false);
    return Made;
}

void PrefabBuilder::Script(const char* InPath)
{
    ScriptPath = InPath ? InPath : "";
}

void PrefabBuilder::TickMode(const EPrefabTickMode InMode)
{
    Mode = InMode;
}

// === The registry ============================================================

bool RegisterNativePrefab(const char* InName, const NativePrefabBuildFn InBuild, const NativePrefabBehavior& InBehavior)
{
    const size_t Length = InName ? std::strlen(InName) : 0;
    if(Length == 0 || Length >= NATIVE_PREFAB_NAME_MAX || !InBuild)
    {
        GAMECORE_LOG_ERROR(std::string("RegisterNativePrefab: refusing '") + (InName ? InName : "") + "' (empty, too long, or no builder)")
        return false;
    }
    if(Find(InName))
    {
        GAMECORE_LOG_ERROR(std::string("RegisterNativePrefab: '") + InName + "' is already registered")
        return false;
    }
    Entries().push_back({ InName, InBuild, InBehavior });
    ++Generation();

    /* A recompile: anything spawned from now on gets the new layout. */
    const std::string Path = std::string(NATIVE_PREFAB_PREFIX) + InName;
    if(IsPrefabLoaded(Path.c_str()))
    {
        ReloadPrefab(Path.c_str());
    }
    return true;
}

void UnregisterNativePrefab(const char* InName)
{
    std::vector<NativeEntry>& List = Entries();
    for(auto It = List.begin(); It != List.end(); ++It)
    {
        if(It->Name == (InName ? InName : ""))
        {
            List.erase(It);
            ++Generation();
            return;
        }
    }
}

const NativePrefabBehavior* FindNativePrefabBehavior(const char* InName)
{
    const NativeEntry* Entry = Find(InName);
    return Entry ? &Entry->Behavior : nullptr;
}

uint32_t GetNativePrefabGeneration()
{
    return Generation();
}

size_t GetNativePrefabCount()
{
    return Entries().size();
}

const char* GetNativePrefabName(const size_t InIndex)
{
    return InIndex < Entries().size() ? Entries()[InIndex].Name.c_str() : "";
}

bool BuildNativePrefab(const char* InName, PrefabAsset& OutAsset)
{
    const NativeEntry* Entry = Find(InName);
    if(!Entry)
    {
        GAMECORE_LOG_WARN(std::string("BuildNativePrefab: no native prefab '") + (InName ? InName : "") + "' is registered")
        return false;
    }

    World Scratch;
    PrefabBuilder Builder(Scratch);
    Entry->Build(Builder);
    if(!Builder.RootID.IsSet())
    {
        GAMECORE_LOG_WARN(std::string("BuildNativePrefab: '") + InName + "' made no root entity")
        return false;
    }

    PrefabAsset Built;
    if(!CapturePrefab(Scratch, Builder.RootID, Built))
    {
        return false;
    }
    Built.SetName(InName);
    Built.SetNativeBehavior(InName);
    Built.SetScriptPath(Builder.ScriptPath);
    Built.SetTickMode(Builder.Mode);
    OutAsset = Built;
    return true;
}
}
