#ifndef GAMECORE_SCRIPTLIBRARY_H
#define GAMECORE_SCRIPTLIBRARY_H

#include <cstddef>
#include <cstdint>

#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/GameCoreAPI.h"

/*
 * Script libraries: what a game module (or any image) exposes to Lua and to
 * script graphs that is not a component -- free functions, C++ variables and
 * typed events. Components themselves are exposed with ComponentScript.h.
 *
 *     static int32_t GScore = 0;
 *
 *     GAMECORE_DEFINE_SCRIPT_LIBRARY(Sandbox)
 *     {
 *         Lib.Function("SpawnPickup", &SpawnPickup, "Position:Vector3, Value:integer -> Pickup:Entity")
 *            .Tooltip("Creates a pickup worth Value at Position");
 *         Lib.Pure("RankForScore", &RankForScore, "Score:integer -> Rank:string");
 *         Lib.NativePure<&Distance>("Distance", "A, B -> Length");   // double Distance(const Vector3&, const Vector3&)
 *         Lib.Variable("Score", &GScore);                       // Sandbox.GetScore / SetScore
 *         Lib.Event("PickupCollected", "Pickup:Entity, Value:integer");
 *     }
 *
 * In Lua the library is a global table: Sandbox.SpawnPickup(x, y, z, 5),
 * Sandbox.GetScore(), Sandbox.SetScore(3). In a script graph each member is a
 * node under the library's name in the palette:
 *
 *   Function      a statement (exec in and out), for calls with effects
 *   Pure          a value node, evaluated where it is used -- no side effects
 *   Variable      Get <Name>, and Set <Name> unless read-only
 *   Event         On <Name>, an event node with typed outputs, and Emit <Name>.
 *                 Events travel on the World's EventBus under <Name>, so C++
 *                 emits them with World::GetEvents().Emit(Name, Args) and a
 *                 hand-written script with Events.On / Events.Emit.
 *
 * SIGNATURES are ComponentScript.h's: "Name:type, ..." optionally followed by
 * " -> Name:type, ...", with types number, integer, bool, string, Vector3,
 * Color, Box and Entity. They are read by tools; the runtime passes whatever
 * the caller gives, and the function reads it through its ScriptCall. A
 * Vector3 is three Lua values, a Color four, a Box six, as everywhere else in
 * the bindings. An event's parameters are at most four, of one-value types
 * (number, integer, bool, string, Entity): an EventBus event carries four.
 *
 * LIFETIME. The macro registers at static initialisation and its registrar's
 * destructor unregisters, so a library lives exactly as long as the image that
 * declared it: a game module's appears when the module loads and goes when it
 * unloads. Everything is copied into GameCore; only the function and variable
 * pointers point back into the image. A lua_State gets the libraries that
 * exist when a World is bound (ScriptEngine::BindWorld); a call into a library
 * that has since unloaded fails with a Lua error rather than jumping into
 * unmapped code.
 *
 * NO lua.h, for ComponentScript.h's reason: a function sees typed values only,
 * through ScriptCall, which never raises. Game thread only.
 */

namespace GameCore
{
    /* A library function. Arguments are ScriptCall's, numbered from 1. */
    using ScriptFunction = void (*)(ScriptCall& InCall);

    enum class EScriptLibraryMember : uint8_t
    {
        Function,       // a call with effects
        Pure,           // a call without effects
        Variable,       // a C++ variable, read and optionally written
        Event,          // a typed EventBus event
    };

    enum : size_t
    {
        SCRIPT_LIBRARY_NAME_MAX    = 32,    // including the NUL; a Lua global
        SCRIPT_LIBRARY_MEMBERS_MAX = 48,
        SCRIPT_TOOLTIP_MAX         = 128,   // including the NUL; truncated, it is only help text
    };

    /* One member, as plain data. Built by ScriptLibraryBinder below. */
    struct ScriptLibraryMember
    {
        char                 Name[SCRIPT_BINDING_NAME_MAX] = {};
        EScriptLibraryMember Kind = EScriptLibraryMember::Function;

        ScriptFunction       Function  = nullptr;                  // Function, Pure
        EScriptFieldType     FieldType = EScriptFieldType::Float;  // Variable
        ScriptStructID       FieldStruct = SCRIPT_STRUCT_INVALID;  // Variable of a script struct
        bool                 bReadOnly = false;                    // Variable
        void*                Storage   = nullptr;                  // Variable: the C++ object read and written
        uint32_t             Size      = 0;                        // Variable

        /* Function, Pure: parameters and results. Event: parameters only. */
        char                 Signature[SCRIPT_SIGNATURE_MAX] = {};
        char                 Tooltip[SCRIPT_TOOLTIP_MAX]     = {};
    };

    /* A registered library, as tools read it. */
    struct ScriptLibraryInfo
    {
        const char*                Name    = "";
        const ScriptLibraryMember* Members = nullptr;
        size_t                     Count   = 0;
    };

    using ScriptLibraryHandle = uint32_t;
    constexpr ScriptLibraryHandle SCRIPT_LIBRARY_INVALID = 0;

    /*
     * Registers a library; called once per library by the macro. InName must
     * be an identifier and not a global the engine or Lua already uses (World,
     * Events, TickPhase, math, string, ...). Members with a malformed or
     * repeated name are dropped and, with InRejected, logged as errors.
     * SCRIPT_LIBRARY_INVALID if the library is refused, including a second
     * library of the same name while the first is registered.
     */
    GAMECORE_API ScriptLibraryHandle RegisterScriptLibrary(const char* InName, const ScriptLibraryMember* InMembers,
                                                           size_t InCount, size_t InRejected);

    /* A no-op for SCRIPT_LIBRARY_INVALID or a handle already unregistered. */
    GAMECORE_API void UnregisterScriptLibrary(ScriptLibraryHandle InHandle);

    /* The registered libraries, in registration order. A ScriptLibraryInfo's
     * pointers are valid until the next register or unregister. */
    GAMECORE_API size_t GetScriptLibraryCount();
    GAMECORE_API bool   GetScriptLibrary(size_t InIndex, ScriptLibraryInfo& OutInfo);

    /*
     * What `Lib` is inside GAMECORE_DEFINE_SCRIPT_LIBRARY. Header-only; it
     * records plain data, and registration happens at Commit().
     */
    class ScriptLibraryBinder
    {
    public:
        /* Returned by each declaration, to add help text. */
        class MemberRef
        {
        public:
            explicit MemberRef(ScriptLibraryMember* InMember) : Member(InMember) {}

            /* Shown in the graph editor's palette and on the node. */
            MemberRef& Tooltip(const char* InText)
            {
                if(Member) { CopyText(Member->Tooltip, SCRIPT_TOOLTIP_MAX, InText, true); }
                return *this;
            }

        private:
            ScriptLibraryMember* Member;
        };

        /* A call with effects: a statement node. */
        MemberRef Function(const char* InName, const ScriptFunction InFn, const char* InSignature = nullptr)
        {
            return AddFunction(InName, InFn, InSignature, EScriptLibraryMember::Function);
        }

        /* A call without effects: a value node, evaluated each time a
         * statement needs it. */
        MemberRef Pure(const char* InName, const ScriptFunction InFn, const char* InSignature = nullptr)
        {
            return AddFunction(InName, InFn, InSignature, EScriptLibraryMember::Pure);
        }

        /*
         * An ordinary C++ function, called as it is: the ScriptCall glue and
         * the signature's types come from its parameters and return type
         * (ComponentScript.h, ScriptNative). InNames names them for the
         * graph's pins -- "Position, Value -> Pickup" -- or is omitted for
         * Arg1, Arg2, ... and Result. A World& or ScriptCall& parameter is
         * filled in, not read from the script.
         *
         *     EntityID SpawnPickup(World& InWorld, const Vector3& InAt, int32_t InValue);
         *     Lib.Native<&SpawnPickup>("SpawnPickup", "Position, Value -> Pickup");
         */
        template<auto TFn>
        MemberRef Native(const char* InName, const char* InNames = nullptr)
        {
            return AddNative<TFn>(InName, InNames, EScriptLibraryMember::Function);
        }

        /* The same, for a function without side effects: a value node. */
        template<auto TFn>
        MemberRef NativePure(const char* InName, const char* InNames = nullptr)
        {
            return AddNative<TFn>(InName, InNames, EScriptLibraryMember::Pure);
        }

        /* Get<Name> and Set<Name> on a C++ object that outlives the library --
         * a global or a static. Any type a component field may have. */
        template<typename TValue>
        MemberRef Variable(const char* InName, TValue* InStorage)
        {
            return AddVariable(InName, InStorage, sizeof(TValue), ScriptFieldDescOf<TValue>(), false);
        }

        /* Get<Name> only. */
        template<typename TValue>
        MemberRef ReadOnly(const char* InName, TValue* InStorage)
        {
            return AddVariable(InName, InStorage, sizeof(TValue), ScriptFieldDescOf<TValue>(), true);
        }

        /* A uint8_t 0/1 flag exposed as a boolean. */
        MemberRef Flag(const char* InName, uint8_t* InStorage, const bool bInReadOnly = false)
        {
            return AddVariable(InName, InStorage, sizeof(uint8_t), ScriptFieldDesc{ EScriptFieldType::Flag, SCRIPT_STRUCT_INVALID }, bInReadOnly);
        }

        /* A typed event, InSignature its parameters: "Pickup:Entity, Value:integer". */
        MemberRef Event(const char* InName, const char* InSignature = nullptr)
        {
            ScriptLibraryMember* Member = NewMember(InName, EScriptLibraryMember::Event);
            if(Member && !CopySignature(*Member, InSignature)) { Member = nullptr; }
            return MemberRef(Member);
        }

        ScriptLibraryHandle Commit(const char* InLibraryName)
        {
            return RegisterScriptLibrary(InLibraryName, Members, Count, Rejected);
        }

    private:
        template<auto TFn>
        static void NativeThunk(ScriptCall& InCall)
        {
            using FT = ScriptNative::FnTraits<decltype(TFn)>;
            ScriptNative::Call<typename FT::Return, typename FT::Args>::Run(InCall, TFn);
        }

        template<auto TFn>
        MemberRef AddNative(const char* InName, const char* InNames, const EScriptLibraryMember InKind)
        {
            using FT = ScriptNative::FnTraits<decltype(TFn)>;
            static_assert(FT::bKnown && !FT::bMember, "Lib.Native takes a free or static function: &Function");

            char Signature[SCRIPT_SIGNATURE_MAX];
            ScriptNative::Call<typename FT::Return, typename FT::Args>::BuildSignature(InNames, Signature, sizeof(Signature));
            return AddFunction(InName, &NativeThunk<TFn>, Signature, InKind);
        }

        MemberRef AddFunction(const char* InName, const ScriptFunction InFn, const char* InSignature,
                              const EScriptLibraryMember InKind)
        {
            ScriptLibraryMember* Member = InFn ? NewMember(InName, InKind) : nullptr;
            if(!InFn) { ++Rejected; }
            if(Member)
            {
                Member->Function = InFn;
                if(!CopySignature(*Member, InSignature)) { Member = nullptr; }
            }
            return MemberRef(Member);
        }

        MemberRef AddVariable(const char* InName, void* InStorage, const size_t InSize, const ScriptFieldDesc InType,
                              const bool bInReadOnly)
        {
            ScriptLibraryMember* Member = InStorage ? NewMember(InName, EScriptLibraryMember::Variable) : nullptr;
            if(!InStorage) { ++Rejected; }
            if(Member)
            {
                Member->Storage   = InStorage;
                Member->Size      = uint32_t(InSize);
                Member->FieldType   = InType.Type;
                Member->FieldStruct = InType.Struct;
                Member->bReadOnly = bInReadOnly;
            }
            return MemberRef(Member);
        }

        /* Rejects rather than truncates, as ComponentScriptBinder does: a
         * shortened name or signature is a different API. */
        ScriptLibraryMember* NewMember(const char* InName, const EScriptLibraryMember InKind)
        {
            ScriptLibraryMember Candidate;
            if(Count >= SCRIPT_LIBRARY_MEMBERS_MAX || !CopyText(Candidate.Name, SCRIPT_BINDING_NAME_MAX, InName, false)
               || Candidate.Name[0] == '\0')
            {
                ++Rejected;
                return nullptr;
            }
            Candidate.Kind = InKind;
            Members[Count] = Candidate;
            return &Members[Count++];
        }

        /* A rejected signature drops the member it belongs to. */
        bool CopySignature(ScriptLibraryMember& InOutMember, const char* InSignature)
        {
            if(CopyText(InOutMember.Signature, SCRIPT_SIGNATURE_MAX, InSignature, false)) { return true; }
            --Count;
            ++Rejected;
            return false;
        }

        /* False if InText does not fit and bInTruncate is not set. */
        static bool CopyText(char* OutBuffer, const size_t InCapacity, const char* InText, const bool bInTruncate)
        {
            size_t Length = 0;
            while(InText != nullptr && InText[Length] != '\0') { ++Length; }
            if(Length >= InCapacity)
            {
                if(!bInTruncate) { return false; }
                Length = InCapacity - 1;
            }
            for(size_t Index = 0; Index < Length; ++Index) { OutBuffer[Index] = InText[Index]; }
            OutBuffer[Length] = '\0';
            return true;
        }

        ScriptLibraryMember Members[SCRIPT_LIBRARY_MEMBERS_MAX];
        size_t              Count    = 0;
        size_t              Rejected = 0;
    };
}

/*
 * One per library, at file scope, in exactly one .cpp. LibraryName is the Lua
 * global and the palette category. The body that follows has `Lib` (a
 * ScriptLibraryBinder&) in scope.
 */
#define GAMECORE_DEFINE_SCRIPT_LIBRARY(LibraryName)                                                  \
    static void LibraryName##_ScriptLibraryBody(::GameCore::ScriptLibraryBinder& Lib);              \
    namespace                                                                                         \
    {                                                                                                 \
        struct LibraryName##_ScriptLibraryRegistrar                                                   \
        {                                                                                             \
            LibraryName##_ScriptLibraryRegistrar()                                                    \
            {                                                                                         \
                ::GameCore::ScriptLibraryBinder Lib;                                                  \
                LibraryName##_ScriptLibraryBody(Lib);                                                 \
                Handle = Lib.Commit(#LibraryName);                                                    \
            }                                                                                         \
            ~LibraryName##_ScriptLibraryRegistrar() { ::GameCore::UnregisterScriptLibrary(Handle); }  \
            ::GameCore::ScriptLibraryHandle Handle = ::GameCore::SCRIPT_LIBRARY_INVALID;              \
        };                                                                                            \
        LibraryName##_ScriptLibraryRegistrar G##LibraryName##_ScriptLibraryRegistrar;                 \
    }                                                                                                 \
    static void LibraryName##_ScriptLibraryBody(::GameCore::ScriptLibraryBinder& Lib)

#endif //GAMECORE_SCRIPTLIBRARY_H
