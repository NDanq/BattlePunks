#ifndef GAMECORE_NATIVEPREFAB_H
#define GAMECORE_NATIVEPREFAB_H

#include <cstddef>
#include <cstdint>

#include "Public/GameCore/Entity.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/TickPhase.h"
#include "Public/GameCore/World.h"

/*
 * Native prefabs (PREFABS.md section 4): a prefab declared in C++ by a game
 * module, the way it declares script libraries (ScriptLibrary.h). Its layout
 * comes from a builder function; its behaviour, optionally, from a table of
 * function pointers.
 *
 *     static void BuildMarker(GameCore::PrefabBuilder& B)
 *     {
 *         GameCore::PrefabBuilder::Node Root = B.Root("Marker");
 *         Root.Add<GameCore::TransformComponent>();
 *         GameCore::PrefabBuilder::Node Mesh = Root.Child("Mesh");
 *         Mesh.Add<GameCore::TransformComponent>();
 *         Mesh.Add<RenderCore::RenderComponent>();
 *         B.Script("Scripts/Marker.lua");     // optional: runs after the callbacks
 *     }
 *
 *     static void MarkerPrePhysics(GameCore::PrefabInstance Self, float Dt, void*) { ... }
 *
 *     static GameCore::NativePrefabBehavior MarkerBehavior()
 *     {
 *         GameCore::NativePrefabBehavior B;
 *         B.OnPhase[GameCore::TP_PrePhysics] = &MarkerPrePhysics;
 *         return B;
 *     }
 *
 *     GAMECORE_DEFINE_NATIVE_PREFAB_BEHAVIOR(Marker, "Sandbox.Marker", BuildMarker, MarkerBehavior())
 *
 * It is the prefab path "native:Sandbox.Marker" everywhere a path goes:
 * SpawnPrefab, a level file, the editor. The registry builds it on first use.
 *
 * BEHAVIOUR runs through the script driver (ScriptEngine::BindWorld), like a
 * prefab's script: OnStart once, OnPhase[P] in each phase P that has one,
 * OnOverlapBegin/End, OnDestroy -- the callbacks first, then the script, in
 * each. A null entry is a phase the instance does not tick in, as a missing
 * Lua function is. A function table, not a virtual class: a game module is a
 * separate image (DESIGN.md's Systems section).
 *
 * LIFETIME. The macro registers at static initialisation and unregisters when
 * the image unloads. Callbacks are found by name each time they run, so an
 * instance whose native prefab has gone simply stops calling into it -- never
 * into unmapped code. The built layout stays in the registry, so an editor
 * that unloaded the game module still shows (and places) it; registering
 * again rebuilds it, so a recompile reaches every new spawn.
 *
 * Game thread only.
 */
namespace GameCore
{
    struct NativePrefabBehavior
    {
        void (*OnStart)(PrefabInstance InSelf, void* InUserData) = nullptr;
        void (*OnPhase[TP_MAX])(PrefabInstance InSelf, float InDeltaSeconds, void* InUserData) = {};
        void (*OnDestroy)(PrefabInstance InSelf, void* InUserData) = nullptr;

        /* InMine: which of the instance's entities overlapped InOther. */
        void (*OnOverlapBegin)(PrefabInstance InSelf, Entity InOther, Entity InMine, void* InUserData) = nullptr;
        void (*OnOverlapEnd)(PrefabInstance InSelf, Entity InOther, Entity InMine, void* InUserData) = nullptr;

        void* UserData = nullptr;
    };

    /* Declared (documented below) ahead of PrefabBuilder's friend declaration,
     * so the friend does not introduce it without GAMECORE_API: MSVC's C2375. */
    GAMECORE_API bool BuildNativePrefab(const char* InName, PrefabAsset& OutAsset);

    /*
     * What a builder function fills in: the entity tree, the script and the
     * tick mode. Entities are made in a scratch World and captured
     * (CapturePrefab), so a component may name another node's entity
     * (Node::GetID()) and that reference becomes part of the prefab.
     */
    class GAMECORE_API PrefabBuilder
    {
    public:
        class GAMECORE_API Node   // MSVC does not export a nested class with its outer one
        {
        public:
            /* Adds InInit (a component); the existing one if it is already there. */
            template<typename T>
            T* Add(const T& InInit = T()) const
            {
                if(T* Existing = Handle.GetComponent<T>()) { return Existing; }
                return Handle.AddComponent(InInit);
            }

            /* A child entity named InName. */
            Node Child(const char* InName) const;

            EntityID GetID() const { return Handle.GetID(); }
            Entity   GetEntity() const { return Handle; }

        private:
            friend class PrefabBuilder;
            Entity         Handle;
            PrefabBuilder* Owner = nullptr;
        };

        explicit PrefabBuilder(World& InScratch);

        /* The root entity, made on the first call; later calls return it. */
        Node Root(const char* InName);

        void Script(const char* InPath);
        void TickMode(EPrefabTickMode InMode);

    private:
        // GAMECORE_API here too, or an importing module sees inconsistent linkage (C4273)
        friend GAMECORE_API bool BuildNativePrefab(const char* InName, PrefabAsset& OutAsset);

        World*          Scratch = nullptr;
        EntityID        RootID;
        const char*     ScriptPath = "";
        EPrefabTickMode Mode = EPrefabTickMode::Tick;
    };

    using NativePrefabBuildFn = void (*)(PrefabBuilder& InBuilder);

    enum : size_t { NATIVE_PREFAB_NAME_MAX = 64 };   // including the NUL

    /* False (and logged) for an empty, too-long or duplicate name, or no builder. */
    GAMECORE_API bool RegisterNativePrefab(const char* InName, NativePrefabBuildFn InBuild, const NativePrefabBehavior& InBehavior);
    GAMECORE_API void UnregisterNativePrefab(const char* InName);

    /* nullptr unless InName is registered now. */
    GAMECORE_API const NativePrefabBehavior* FindNativePrefabBehavior(const char* InName);

    /* Bumped by every register and unregister: a cached behaviour pointer is
     * good while this is unchanged. */
    GAMECORE_API uint32_t GetNativePrefabGeneration();

    /* The registered native prefabs, in registration order. */
    GAMECORE_API size_t      GetNativePrefabCount();
    GAMECORE_API const char* GetNativePrefabName(size_t InIndex);

    /* Runs InName's builder into OutAsset (named InName, its native behaviour
     * InName). False if InName is not registered or built nothing. */
    GAMECORE_API bool BuildNativePrefab(const char* InName, PrefabAsset& OutAsset);

    struct NativePrefabRegistrar
    {
        NativePrefabRegistrar(const char* InName, NativePrefabBuildFn InBuild, const NativePrefabBehavior& InBehavior)
            : Name(InName)
        {
            RegisterNativePrefab(InName, InBuild, InBehavior);
        }
        ~NativePrefabRegistrar() { UnregisterNativePrefab(Name); }

        NativePrefabRegistrar(const NativePrefabRegistrar&)            = delete;
        NativePrefabRegistrar& operator=(const NativePrefabRegistrar&) = delete;

        const char* Name;
    };
}

/* A layout only. Once per prefab, at namespace scope in a .cpp. */
#define GAMECORE_DEFINE_NATIVE_PREFAB(Ident, NameLiteral, BuildFn)                                        \
    static ::GameCore::NativePrefabRegistrar GNativePrefab_##Ident(NameLiteral, BuildFn, ::GameCore::NativePrefabBehavior());

/* A layout and behaviour; BehaviorExpr is a NativePrefabBehavior value. */
#define GAMECORE_DEFINE_NATIVE_PREFAB_BEHAVIOR(Ident, NameLiteral, BuildFn, BehaviorExpr)                 \
    static ::GameCore::NativePrefabRegistrar GNativePrefab_##Ident(NameLiteral, BuildFn, BehaviorExpr);

#endif //GAMECORE_NATIVEPREFAB_H
