#ifndef GAMECORE_COMPONENTSCRIPT_H
#define GAMECORE_COMPONENTSCRIPT_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/Math/Vector.h"

/* Lua's own type, forward-declared at global scope -- lua.h never appears in a
 * public header (see ScriptEngine.h). */
struct lua_State;

/*
 * Selectively exposing a component to Lua. A component lists what scripts may
 * touch; anything not listed is invisible. See DESIGN.md.
 *
 *     GAMECORE_DEFINE_COMPONENT_SCRIPT(PrimitiveComponent)
 *     {
 *         Bind.Field("Width", &PrimitiveComponent::Width);    // GetWidth / SetWidth
 *         Bind.Flag("Visible", &PrimitiveComponent::bVisible); // uint8_t as a Lua boolean
 *         Bind.ReadOnly("World", &BoundsComponent::World);     // GetWorld only
 *         Bind.Method("Translate", [](TransformComponent& C, ScriptCall& Call) { ... });
 *     }
 *
 * Every component, bound or not, also answers GetEntity().
 *
 * NO lua.h HERE, and binding code must never include it: Lua is a static
 * library inside GameCore.dll, and a second copy in another image operating on
 * GameCore's lua_State is not something Lua 5.4 detects. GameCore does all the
 * Lua work; a binding sees only typed values.
 *
 * Game thread only. Bindings are read when a type's first proxy is pushed in a
 * lua_State, so register them at static initialisation (the macro does).
 */

namespace GameCore
{
    class World;

    /*
     * One call of a custom method, as typed values. Arguments are numbered
     * from 1, not counting the component itself: t:Translate(dx, dy, dz) has
     * GetNumber(1..3).
     *
     * NEVER RAISES. A missing or mistyped argument records an error and the
     * getter returns a default; GameCore raises the Lua error after the method
     * returns. Lua errors are longjmps, which would skip the destructors of
     * anything alive in the method -- so they only happen outside it. A method
     * may check HasError() to stop early, but need not.
     */
    class GAMECORE_API ScriptCall
    {
    public:
        int  GetArgCount() const;
        bool HasArg(int InArg) const;   // present and not nil

        double              GetNumber (int InArg);
        float               GetFloat  (int InArg);
        int32_t             GetInt    (int InArg);
        bool                GetBool   (int InArg);   // Lua truthiness; never an error
        const char*         GetString (int InArg);   // valid until the method returns; "" on error
        EngineCore::Vector3 GetVector3(int InFirstArg);   // three numbers

        void PushNumber (double InValue);
        void PushBool   (bool InValue);
        void PushString (const char* InValue);
        void PushVector3(const EngineCore::Vector3& InValue);   // three numbers
        void PushEntity (EntityID InEntity);
        void PushNil    ();

        /* Fails the call with InMessage (the first error wins). */
        void Error(const char* InMessage);
        bool HasError() const { return ErrorText[0] != '\0'; }

        /* The component's entity, and the World it lives in. */
        EntityID GetOwner() const { return Owner; }
        World&   GetWorld() const { return *OwnerWorld; }

        // --- engine side ------------------------------------------------------
        // Constructed and read only by GameCore's dispatcher.

        ScriptCall(lua_State* InState, const char* InMethodName, EntityID InOwner, World* InWorld);
        int         GetResultCount() const { return Results; }
        const char* GetErrorText()   const { return ErrorText; }

    private:
        void ArgError(int InArg, const char* InExpected);

        lua_State*        State      = nullptr;
        const char*       MethodName = "";
        EntityID          Owner;
        World*            OwnerWorld = nullptr;
        int               Results    = 0;
        char              ErrorText[160] = {};
    };

    // === Binding descriptions ================================================
    // Built by ComponentScriptBinder below; nothing here is written by hand.

    enum class EScriptFieldType : uint8_t
    {
        Float, Double, Int32, UInt8, Bool, Flag, Vector3, AABB, Color, FixedString
    };

    template<typename TField> struct ScriptFieldTypeOf;   // unsupported types fail to compile
    template<> struct ScriptFieldTypeOf<float>               { static constexpr EScriptFieldType Value = EScriptFieldType::Float;   };
    template<> struct ScriptFieldTypeOf<double>              { static constexpr EScriptFieldType Value = EScriptFieldType::Double;  };
    template<> struct ScriptFieldTypeOf<int32_t>             { static constexpr EScriptFieldType Value = EScriptFieldType::Int32;   };
    template<> struct ScriptFieldTypeOf<uint8_t>             { static constexpr EScriptFieldType Value = EScriptFieldType::UInt8;   };
    template<> struct ScriptFieldTypeOf<bool>                { static constexpr EScriptFieldType Value = EScriptFieldType::Bool;    };
    template<> struct ScriptFieldTypeOf<EngineCore::Vector3> { static constexpr EScriptFieldType Value = EScriptFieldType::Vector3; };
    template<> struct ScriptFieldTypeOf<AABB>                { static constexpr EScriptFieldType Value = EScriptFieldType::AABB;    };
    template<> struct ScriptFieldTypeOf<EngineCore::Color>   { static constexpr EScriptFieldType Value = EScriptFieldType::Color;   };
    template<size_t N> struct ScriptFieldTypeOf<char[N]>     { static constexpr EScriptFieldType Value = EScriptFieldType::FixedString; };

    /* Calls a typed method through its type-erased pointer. */
    using ScriptMethodInvoker = void (*)(void (*InMethod)(), void* InComponent, ScriptCall& InCall);

    enum : size_t
    {
        SCRIPT_BINDING_NAME_MAX      = 40,   // including the NUL; accessors add Get/Set
        SCRIPT_BINDINGS_PER_TYPE_MAX = 32,
        SCRIPT_SIGNATURE_MAX         = 96,   // including the NUL
    };

    struct ScriptBindingEntry
    {
        char                Name[SCRIPT_BINDING_NAME_MAX] = {};   // "Width" -> GetWidth/SetWidth
        bool                bMethod   = false;
        bool                bReadOnly = false;
        EScriptFieldType    FieldType = EScriptFieldType::Float;
        uint32_t            Offset    = 0;
        uint32_t            Size      = 0;
        ScriptMethodInvoker Invoker   = nullptr;
        void              (*Method)() = nullptr;

        /* A method's parameters and results, for tools -- Lua never reads it.
         * "Name:type, ..." then optionally " -> Name:type, ...", the types
         * being number, integer, bool, string, Vector3, Color, Entity. "" when
         * the binding gave none (or one too long to store). Copied, not
         * pointed at, so it outlives a hot-reloaded module that registered it. */
        char                Signature[SCRIPT_SIGNATURE_MAX] = {};
    };

    /*
     * Registers a type's bindings; called once per type by the macro.
     * InRejected counts bindings the binder could not record (too many, or a
     * name too long to store without changing it) and is logged as an error.
     * False on a duplicate registration (the first is kept) or a full table.
     */
    GAMECORE_API bool RegisterComponentScript(ComponentTypeID InType, const ScriptBindingEntry* InEntries,
                                              size_t InCount, size_t InRejected);

    /* nullptr/0 if InType has no bindings -- a normal answer. */
    GAMECORE_API const ScriptBindingEntry* FindComponentScript(ComponentTypeID InType, size_t& OutCount);

    /*
     * What `Bind` is inside GAMECORE_DEFINE_COMPONENT_SCRIPT. Header-only, and
     * everything it records is plain data -- registration happens at Commit().
     */
    template<typename T>
    class ComponentScriptBinder
    {
    public:
        using MethodFn = void (*)(T&, ScriptCall&);

        /* Get<Name> and Set<Name>. */
        template<typename TField>
        void Field(const char* InName, TField T::* InMember) { AddField(InName, InMember, ScriptFieldTypeOf<TField>::Value, false); }

        /* Get<Name> only. */
        template<typename TField>
        void ReadOnly(const char* InName, TField T::* InMember) { AddField(InName, InMember, ScriptFieldTypeOf<TField>::Value, true); }

        /* A uint8_t 0/1 flag exposed as a Lua boolean. */
        void Flag(const char* InName, uint8_t T::* InMember, const bool bInReadOnly = false)
        {
            AddField(InName, InMember, EScriptFieldType::Flag, bInReadOnly);
        }

        /* A capture-less lambda converts to MethodFn. InSignature describes the
         * arguments for tools (see ScriptBindingEntry::Signature), e.g.
         * "Delta:Vector3" or "Factor:number -> Result:number". */
        void Method(const char* InName, const MethodFn InFn, const char* InSignature = nullptr)
        {
            ScriptBindingEntry* Entry = NewEntry(InName);
            if(!Entry) { return; }
            Entry->bMethod = true;
            Entry->Method  = reinterpret_cast<void (*)()>(InFn);
            Entry->Invoker = &Invoke;

            size_t Length = 0;
            while(InSignature != nullptr && InSignature[Length] != '\0') { ++Length; }
            if(Length < SCRIPT_SIGNATURE_MAX)
            {
                for(size_t Index = 0; Index < Length; ++Index) { Entry->Signature[Index] = InSignature[Index]; }
            }
        }

        void Commit() { RegisterComponentScript(T::StaticType().ID, Entries, Count, Rejected); }

    private:
        /* Casts back to the exact type Method() erased, which is well-defined. */
        static void Invoke(void (*InMethod)(), void* InComponent, ScriptCall& InCall)
        {
            reinterpret_cast<MethodFn>(InMethod)(*static_cast<T*>(InComponent), InCall);
        }

        template<typename TField>
        void AddField(const char* InName, TField T::* InMember, const EScriptFieldType InType, const bool bInReadOnly)
        {
            ScriptBindingEntry* Entry = NewEntry(InName);
            if(!Entry) { return; }

            /* The member's offset, measured on a default-constructed instance
             * -- every component is default-constructible and trivially
             * copyable (GAMECORE_DEFINE_COMPONENT asserts the latter). */
            const T Probe{};
            const char* Base   = reinterpret_cast<const char*>(&Probe);
            const char* Member = reinterpret_cast<const char*>(&(Probe.*InMember));

            Entry->FieldType = InType;
            Entry->bReadOnly = bInReadOnly;
            Entry->Offset    = uint32_t(Member - Base);
            Entry->Size      = uint32_t(sizeof(TField));
        }

        /* Rejects rather than truncates an over-long name: a truncated name is a
         * different Lua API. Rejections are reported at Commit(). */
        ScriptBindingEntry* NewEntry(const char* InName)
        {
            size_t Length = 0;
            while(InName != nullptr && InName[Length] != '\0') { ++Length; }

            if(Count >= SCRIPT_BINDINGS_PER_TYPE_MAX || Length == 0 || Length >= SCRIPT_BINDING_NAME_MAX)
            {
                ++Rejected;
                return nullptr;
            }

            ScriptBindingEntry& Entry = Entries[Count++];
            for(size_t Index = 0; Index < Length; ++Index) { Entry.Name[Index] = InName[Index]; }
            return &Entry;
        }

        ScriptBindingEntry Entries[SCRIPT_BINDINGS_PER_TYPE_MAX];
        size_t             Count    = 0;
        size_t             Rejected = 0;
    };
}

/*
 * One per component type, at file scope, in exactly one .cpp. The body that
 * follows has `Bind` (a ComponentScriptBinder<TypeName>&) in scope.
 */
#define GAMECORE_DEFINE_COMPONENT_SCRIPT(TypeName)                                                   \
    static void TypeName##_ScriptBody(::GameCore::ComponentScriptBinder<TypeName>& Bind);            \
    namespace                                                                                         \
    {                                                                                                 \
        struct TypeName##_ScriptRegistrar                                                             \
        {                                                                                             \
            TypeName##_ScriptRegistrar()                                                              \
            {                                                                                         \
                ::GameCore::ComponentScriptBinder<TypeName> Bind;                                     \
                TypeName##_ScriptBody(Bind);                                                          \
                Bind.Commit();                                                                        \
            }                                                                                         \
        };                                                                                            \
        TypeName##_ScriptRegistrar G##TypeName##_ScriptRegistrar;                                      \
    }                                                                                                 \
    static void TypeName##_ScriptBody(::GameCore::ComponentScriptBinder<TypeName>& Bind)

#endif //GAMECORE_COMPONENTSCRIPT_H
