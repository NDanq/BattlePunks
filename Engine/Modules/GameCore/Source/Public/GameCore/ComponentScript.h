#ifndef GAMECORE_COMPONENTSCRIPT_H
#define GAMECORE_COMPONENTSCRIPT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/ScriptStruct.h"
#include "Public/Math/Vector.h"

/* Lua's own type, forward-declared at global scope -- lua.h never appears in a
 * public header (see ScriptEngine.h). */
struct lua_State;

/*
 * Selectively exposing a component to Lua. A component lists what scripts may
 * touch; anything not listed is invisible. See DESIGN.md.
 *
 *     GAMECORE_DEFINE_COMPONENT_SCRIPT(RenderComponent)
 *     {
 *         Bind.Field("Width", &RenderComponent::Width);    // GetWidth / SetWidth
 *         Bind.Flag("Visible", &RenderComponent::bVisible); // uint8_t as a Lua boolean
 *         Bind.ReadOnly("World", &BoundsComponent::World);     // GetWorld only
 *         Bind.Method("Translate", [](TransformComponent& C, ScriptCall& Call) { ... });
 *         Bind.Native<&PickupComponent::Collect>("Collect", "-> Value");  // a C++ member, as it is
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
     * One call of a custom method or a library function (ScriptLibrary.h), as
     * typed values. Arguments are numbered from 1, not counting the component
     * itself: t:Translate(dx, dy, dz) and Sandbox.Spawn(dx, dy, dz) both have
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
        EngineCore::Color   GetColor  (int InFirstArg);   // three or four integers 0..255; alpha defaults to 255
        EntityID            GetEntity (int InArg);        // an Entity value; nil is an unset id, not an error

        /* A script struct (ScriptStruct.h) from its fields' values, starting
         * at InFirstArg, into OutValue (the struct's size). False, with an
         * error, if a value is bad or the struct is not registered. */
        bool GetStruct(int InFirstArg, ScriptStructID InStruct, void* OutValue);

        void PushNumber (double InValue);
        void PushInteger(int64_t InValue);   // a Lua integer: prints as 3, not 3.0
        void PushBool   (bool InValue);
        void PushString (const char* InValue);
        void PushVector3(const EngineCore::Vector3& InValue);   // three numbers
        void PushColor  (const EngineCore::Color& InValue);     // four integers
        void PushEntity (EntityID InEntity);
        void PushStruct (ScriptStructID InStruct, const void* InValue);   // its fields' values, in order
        void PushNil    ();

        /* Fails the call with InMessage (the first error wins). */
        void Error(const char* InMessage);
        bool HasError() const { return ErrorText[0] != '\0'; }

        /* The component's entity (unset in a library function), and the World
         * the script runs in. */
        EntityID GetOwner() const { return Owner; }
        World&   GetWorld() const { return *OwnerWorld; }

        // --- engine side ------------------------------------------------------
        // Constructed and read only by GameCore's dispatchers.

        /* InArgBase: Lua stack slots before argument 1 -- 1 for a method (the
         * proxy), 0 for a library function. */
        ScriptCall(lua_State* InState, const char* InMethodName, EntityID InOwner, World* InWorld, int InArgBase = 1);
        int         GetResultCount() const { return Results; }
        const char* GetErrorText()   const { return ErrorText; }

    private:
        void ArgError(int InArg, const char* InExpected);

        lua_State*        State      = nullptr;
        const char*       MethodName = "";
        EntityID          Owner;
        World*            OwnerWorld = nullptr;
        int               Results    = 0;
        int               ArgBase    = 1;
        char              ErrorText[160] = {};
    };

    // === Binding descriptions ================================================
    // Built by ComponentScriptBinder below; nothing here is written by hand.

    /* Calls a typed method through its type-erased pointer. */
    using ScriptMethodInvoker = void (*)(void (*InMethod)(), void* InComponent, ScriptCall& InCall);

    struct ScriptBindingEntry
    {
        char                Name[SCRIPT_BINDING_NAME_MAX] = {};   // "Width" -> GetWidth/SetWidth
        bool                bMethod   = false;
        bool                bReadOnly = false;
        bool                bPure     = false;   // a method without effects: a value node in a graph
        EScriptFieldType    FieldType = EScriptFieldType::Float;
        uint32_t            Offset    = 0;
        uint32_t            Size      = 0;
        ScriptStructID      FieldStruct = SCRIPT_STRUCT_INVALID;   // a Struct field's struct
        ScriptMethodInvoker Invoker   = nullptr;
        void              (*Method)() = nullptr;

        /* For tools only: "Name:type, ..." optionally " -> Name:type, ...",
         * types number, integer, bool, string, Vector3, Color, Entity. Copied,
         * so it survives a hot reload of the registering module. */
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

    /*
     * Withdraws InType's bindings; the macro's registrar calls it when its image
     * unloads, so a game module's methods never outlive its code and the next
     * load can register again. A method called through a metatable built
     * before this fails with a Lua error instead of jumping into the unloaded
     * image. A no-op for a type without bindings.
     */
    GAMECORE_API void UnregisterComponentScript(ComponentTypeID InType);

    /* nullptr/0 if InType has no bindings -- a normal answer. */
    GAMECORE_API const ScriptBindingEntry* FindComponentScript(ComponentTypeID InType, size_t& OutCount);

    // === Native calls ========================================================
    // What Bind.Native / Lib.Native generate: the ScriptCall glue for an
    // ordinary C++ function, from its parameter and return types.

    namespace ScriptNative
    {
        template<typename T> using Bare = std::remove_cv_t<std::remove_reference_t<T>>;

        /*
         * One supported C++ type: how many Lua values it takes (Lanes), its
         * signature type ("number"; nullptr if it is not a script value), how
         * to read it from a ScriptCall and how to push it back. A type without
         * a specialisation fails to compile at the Native call that uses it.
         */
        template<typename T, typename = void> struct Traits
        {
            /* Enough for the static_asserts below to report it, and no more. */
            static constexpr bool        bSupported = false;
            static constexpr int         Lanes      = 0;
            static constexpr const char* Type       = nullptr;
        };

        template<typename TValue, int TLanes>
        struct TraitsBase
        {
            static constexpr bool bSupported = true;
            static constexpr int  Lanes      = TLanes;
            using Stored = TValue;
            static TValue& Use(TValue& InStored) { return InStored; }
        };

        template<> struct Traits<float> : TraitsBase<float, 1>
        {
            static constexpr const char* Type = "number";
            static float Get(ScriptCall& C, const int A)       { return C.GetFloat(A); }
            static void  Push(ScriptCall& C, const float InV)  { C.PushNumber(InV); }
        };
        template<> struct Traits<double> : TraitsBase<double, 1>
        {
            static constexpr const char* Type = "number";
            static double Get(ScriptCall& C, const int A)      { return C.GetNumber(A); }
            static void   Push(ScriptCall& C, const double InV) { C.PushNumber(InV); }
        };
        template<> struct Traits<int32_t> : TraitsBase<int32_t, 1>
        {
            static constexpr const char* Type = "integer";
            static int32_t Get(ScriptCall& C, const int A)       { return C.GetInt(A); }
            static void    Push(ScriptCall& C, const int32_t InV) { C.PushInteger(InV); }
        };
        template<> struct Traits<bool> : TraitsBase<bool, 1>
        {
            static constexpr const char* Type = "bool";
            static bool Get(ScriptCall& C, const int A)      { return C.GetBool(A); }
            static void Push(ScriptCall& C, const bool InV)  { C.PushBool(InV); }
        };
        /* Valid for the duration of the call, which is all a parameter needs. */
        template<> struct Traits<const char*> : TraitsBase<const char*, 1>
        {
            static constexpr const char* Type = "string";
            static const char* Get(ScriptCall& C, const int A)         { return C.GetString(A); }
            static void        Push(ScriptCall& C, const char* InV)    { C.PushString(InV); }
        };
        template<> struct Traits<std::string> : TraitsBase<std::string, 1>
        {
            static constexpr const char* Type = "string";
            static std::string Get(ScriptCall& C, const int A)            { return std::string(C.GetString(A)); }
            static void        Push(ScriptCall& C, const std::string& InV) { C.PushString(InV.c_str()); }
        };
        template<> struct Traits<EngineCore::Vector3> : TraitsBase<EngineCore::Vector3, 3>
        {
            static constexpr const char* Type = "Vector3";
            static EngineCore::Vector3 Get(ScriptCall& C, const int A)                    { return C.GetVector3(A); }
            static void                Push(ScriptCall& C, const EngineCore::Vector3& InV) { C.PushVector3(InV); }
        };
        /* Four values, alpha included, so the arguments after it stay in place. */
        template<> struct Traits<EngineCore::Color> : TraitsBase<EngineCore::Color, 4>
        {
            static constexpr const char* Type = "Color";
            static EngineCore::Color Get(ScriptCall& C, const int A)                  { return C.GetColor(A); }
            static void              Push(ScriptCall& C, const EngineCore::Color& InV) { C.PushColor(InV); }
        };
        template<> struct Traits<EntityID> : TraitsBase<EntityID, 1>
        {
            static constexpr const char* Type = "Entity";
            static EntityID Get(ScriptCall& C, const int A)        { return C.GetEntity(A); }
            static void     Push(ScriptCall& C, const EntityID InV) { C.PushEntity(InV); }
        };

        /* Context, not script values: no Lua argument, not in the signature. A
         * function that needs the World, or the call itself (to Error()),
         * takes it as a parameter anywhere in its list. */
        template<> struct Traits<World>
        {
            static constexpr bool        bSupported = true;
            static constexpr int         Lanes      = 0;
            static constexpr const char* Type       = nullptr;
            using Stored = World*;
            static World* Get(ScriptCall& C, int)  { return &C.GetWorld(); }
            static World& Use(World* InStored)     { return *InStored; }
        };
        template<> struct Traits<ScriptCall>
        {
            static constexpr bool        bSupported = true;
            static constexpr int         Lanes      = 0;
            static constexpr const char* Type       = nullptr;
            using Stored = ScriptCall*;
            static ScriptCall* Get(ScriptCall& C, int)     { return &C; }
            static ScriptCall& Use(ScriptCall* InStored)   { return *InStored; }
        };

        /* Any script struct (ScriptStruct.h): its fields' values, in order.
         * Its width and name are the registered struct's, so they are read
         * at run time (Lanes -1; LaneCountOf, TypeNameOf). */
        template<typename T> struct Traits<T, std::enable_if_t<IsScriptStruct<T>::value>>
        {
            static constexpr bool        bSupported = true;
            static constexpr int         Lanes      = -1;
            static constexpr const char* Type       = "";
            using Stored = T;
            static T&   Use(T& InStored) { return InStored; }
            static T    Get(ScriptCall& C, const int A)   { T Value{}; C.GetStruct(A, T::StaticScriptStruct(), &Value); return Value; }
            static void Push(ScriptCall& C, const T& InV) { C.PushStruct(T::StaticScriptStruct(), &InV); }
        };

        template<typename B>
        int LaneCountOf()
        {
            if constexpr(IsScriptStruct<B>::value) { return GetScriptFieldLanes(EScriptFieldType::Struct, B::StaticScriptStruct()); }
            else                                   { return Traits<B>::Lanes; }
        }

        /* nullptr for a context parameter (World&, ScriptCall&). */
        template<typename B>
        const char* TypeNameOf()
        {
            if constexpr(IsScriptStruct<B>::value) { return GetScriptStructName(B::StaticScriptStruct()); }
            else                                   { return Traits<B>::Type; }
        }

        /* A parameter is read from Lua, so it may be a value or a const
         * reference; World& and ScriptCall& are the only mutable references. */
        template<typename TParam>
        constexpr bool IsSupportedParameter()
        {
            using B = Bare<TParam>;
            if constexpr(!Traits<B>::bSupported) { return false; }
            else
            {
                constexpr bool bMutableRef = std::is_lvalue_reference<TParam>::value
                                          && !std::is_const<std::remove_reference_t<TParam>>::value;
                return !bMutableRef || Traits<B>::Lanes == 0;
            }
        }

        /* void, or a script value (not World or ScriptCall). */
        template<typename TReturn>
        constexpr bool IsSupportedReturn()
        {
            if constexpr(std::is_void<TReturn>::value) { return true; }
            else if constexpr(!Traits<Bare<TReturn>>::bSupported) { return false; }
            else { return Traits<Bare<TReturn>>::Lanes != 0; }
        }

        template<typename TFn> struct FnTraits { static constexpr bool bKnown = false; };
        template<typename TClass, typename R, typename... A> struct FnTraits<R (TClass::*)(A...)>
        {
            static constexpr bool bKnown = true, bMember = true;
            using Class = TClass; using Return = R; using Args = std::tuple<A...>;
        };
        template<typename TClass, typename R, typename... A> struct FnTraits<R (TClass::*)(A...) const>
            : FnTraits<R (TClass::*)(A...)> {};
        template<typename TClass, typename R, typename... A> struct FnTraits<R (TClass::*)(A...) noexcept>
            : FnTraits<R (TClass::*)(A...)> {};
        template<typename TClass, typename R, typename... A> struct FnTraits<R (TClass::*)(A...) const noexcept>
            : FnTraits<R (TClass::*)(A...)> {};
        template<typename R, typename... A> struct FnTraits<R (*)(A...)>
        {
            static constexpr bool bKnown = true, bMember = false;
            using Class = void; using Return = R; using Args = std::tuple<A...>;
        };
        template<typename R, typename... A> struct FnTraits<R (*)(A...) noexcept> : FnTraits<R (*)(A...)> {};

        /* Appends to a fixed buffer; false (and the buffer emptied) on overflow. */
        inline bool Append(char* InOutBuffer, const size_t InCapacity, size_t& InOutLength, const char* InText)
        {
            for(const char* Ch = InText; *Ch != '\0'; ++Ch)
            {
                if(InOutLength + 1 >= InCapacity) { InOutBuffer[0] = '\0'; InOutLength = 0; return false; }
                InOutBuffer[InOutLength++] = *Ch;
            }
            InOutBuffer[InOutLength] = '\0';
            return true;
        }

        /* The InIndex-th name of a comma-separated list in [InBegin, InEnd),
         * trimmed, into OutName; false if the list is shorter. */
        inline bool NthName(const char* InBegin, const char* InEnd, int InIndex, char (&OutName)[SCRIPT_BINDING_NAME_MAX])
        {
            const char* Start = InBegin;
            for(const char* Ch = InBegin; Ch <= InEnd; ++Ch)
            {
                if(Ch != InEnd && *Ch != ',') { continue; }
                if(InIndex-- == 0)
                {
                    const char* First = Start;
                    const char* Last  = Ch;
                    while(First < Last && (*First == ' ' || *First == '\t')) { ++First; }
                    while(Last > First && (Last[-1] == ' ' || Last[-1] == '\t')) { --Last; }
                    const size_t Length = size_t(Last - First);
                    if(Length == 0 || Length >= SCRIPT_BINDING_NAME_MAX) { return false; }
                    for(size_t Index = 0; Index < Length; ++Index) { OutName[Index] = First[Index]; }
                    OutName[Length] = '\0';
                    return true;
                }
                Start = Ch + 1;
            }
            return false;
        }

        template<typename R, typename TArgs> struct Call;

        template<typename R, typename... A>
        struct Call<R, std::tuple<A...>>
        {
            static_assert((IsSupportedParameter<A>() && ...),
                          "Native: a parameter type has no script mapping. Supported: float, double, int32_t, bool, "
                          "const char*, std::string, Vector3, Color, EntityID, script structs (by value or const&), and World& or "
                          "ScriptCall& for context. Bind it with Method and a ScriptCall instead.");
            static_assert(IsSupportedReturn<R>(),
                          "Native: the return type has no script mapping (see the parameter list above). For several "
                          "results, bind with Method and push each one.");

            /* Reads every argument (left to right: a braced list), stops if
             * any was bad, then calls and pushes the result. Never raises, as
             * ScriptCall promises. Each parameter's first Lua argument is the
             * lanes before it plus one -- counted at run time, since a script
             * struct's width is known only once it is registered. */
            template<typename TFn, size_t... I>
            static void Run(ScriptCall& InCall, TFn& InFn, std::index_sequence<I...>)
            {
                const int Lanes[] = { LaneCountOf<Bare<A>>()..., 0 };
                int       First[sizeof...(A) + 1] = { 1 };
                for(size_t Index = 0; Index < sizeof...(A); ++Index) { First[Index + 1] = First[Index] + Lanes[Index]; }

                std::tuple<typename Traits<Bare<A>>::Stored...> Values{ Traits<Bare<A>>::Get(InCall, First[I])... };
                if(InCall.HasError()) { return; }
                if constexpr(std::is_void<R>::value)
                {
                    InFn(Traits<Bare<A>>::Use(std::get<I>(Values))...);
                }
                else
                {
                    Traits<Bare<R>>::Push(InCall, InFn(Traits<Bare<A>>::Use(std::get<I>(Values))...));
                }
            }

            template<typename TFn>
            static void Run(ScriptCall& InCall, TFn&& InFn)
            {
                Run(InCall, InFn, std::index_sequence_for<A...>{});
            }

            /*
             * The tools' signature from the C++ types. InNames is null, names
             * only ("Time, Height -> Offset"), or a full signature (it has a
             * ':') used as it is. Missing names are Arg1, Arg2, ... and Result.
             */
            static void BuildSignature(const char* InNames, char* OutBuffer, const size_t InCapacity)
            {
                OutBuffer[0] = '\0';
                size_t Length = 0;
                const char* Names = InNames ? InNames : "";
                for(const char* Ch = Names; *Ch != '\0'; ++Ch)
                {
                    if(*Ch == ':') { Append(OutBuffer, InCapacity, Length, Names); return; }
                }

                const char* Arrow = Names;
                while(*Arrow != '\0' && !(Arrow[0] == '-' && Arrow[1] == '>')) { ++Arrow; }
                const char* NamesEnd = Names;
                while(*NamesEnd != '\0') { ++NamesEnd; }

                const char* const Types[] = { TypeNameOf<Bare<A>>()..., nullptr };
                int Visible = 0;
                for(size_t Index = 0; Index + 1 < sizeof(Types) / sizeof(Types[0]); ++Index)
                {
                    if(!Types[Index]) { continue; }   // World&, ScriptCall&
                    char Name[SCRIPT_BINDING_NAME_MAX] = {};
                    if(!NthName(Names, Arrow, Visible, Name))
                    {
                        Name[0] = 'A'; Name[1] = 'r'; Name[2] = 'g';
                        Name[3] = char('1' + Visible % 10); Name[4] = '\0';
                    }
                    if(!(Append(OutBuffer, InCapacity, Length, Visible > 0 ? ", " : "")
                         && Append(OutBuffer, InCapacity, Length, Name)
                         && Append(OutBuffer, InCapacity, Length, ":")
                         && Append(OutBuffer, InCapacity, Length, Types[Index]))) { return; }
                    ++Visible;
                }

                if constexpr(!std::is_void<R>::value)
                {
                    char Name[SCRIPT_BINDING_NAME_MAX] = "Result";
                    if(*Arrow != '\0') { NthName(Arrow + 2, NamesEnd, 0, Name); }
                    if(!(Append(OutBuffer, InCapacity, Length, Visible > 0 ? " -> " : "-> ")
                         && Append(OutBuffer, InCapacity, Length, Name)
                         && Append(OutBuffer, InCapacity, Length, ":")
                         && Append(OutBuffer, InCapacity, Length, TypeNameOf<Bare<R>>()))) { return; }
                }
            }
        };
    }

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
        void Field(const char* InName, TField T::* InMember) { AddField(InName, InMember, ScriptFieldDescOf<TField>(), false); }

        /* Get<Name> only. */
        template<typename TField>
        void ReadOnly(const char* InName, TField T::* InMember) { AddField(InName, InMember, ScriptFieldDescOf<TField>(), true); }

        /* One element of an array member, as Get<Name> and Set<Name>. */
        template<typename TElement, size_t N>
        void Element(const char* InName, TElement (T::* InArray)[N], const size_t InIndex)
        {
            if(InIndex >= N)
            {
                ++Rejected;
                return;
            }
            ScriptBindingEntry* Entry = NewEntry(InName);
            if(!Entry) { return; }

            const T Probe{};
            const char* Base   = reinterpret_cast<const char*>(&Probe);
            const char* Member = reinterpret_cast<const char*>(&((Probe.*InArray)[InIndex]));

            Entry->FieldType = ScriptFieldTypeOf<TElement>::Value;
            Entry->bReadOnly = false;
            Entry->Offset    = uint32_t(Member - Base);
            Entry->Size      = uint32_t(sizeof(TElement));
        }

        /* A uint8_t 0/1 flag exposed as a Lua boolean. */
        void Flag(const char* InName, uint8_t T::* InMember, const bool bInReadOnly = false)
        {
            AddField(InName, InMember, ScriptFieldDesc{ EScriptFieldType::Flag, SCRIPT_STRUCT_INVALID }, bInReadOnly);
        }

        /* A capture-less lambda converts to MethodFn. InSignature: see
         * ScriptBindingEntry::Signature, e.g. "Factor:number -> Result:number". */
        void Method(const char* InName, const MethodFn InFn, const char* InSignature = nullptr)
        {
            AddMethod(InName, InFn, InSignature, false);
        }

        /* The same for a method without effects -- a query. A script graph
         * makes it a value node, evaluated where it is used, rather than a
         * statement in the execution chain. Lua calls it the same way. */
        void Pure(const char* InName, const MethodFn InFn, const char* InSignature = nullptr)
        {
            AddMethod(InName, InFn, InSignature, true);
        }

        /*
         * A member function of the component, called as it is: the Lua glue
         * and the graph signature come from its C++ types (ScriptNative).
         *
         *     Bind.Native<&PickupComponent::Collect>("Collect", "-> Value");
         *     Bind.NativePure<&PickupComponent::PositionAt>("PositionAt", "Time -> Position");
         *
         * InNames names the parameters and the result for the graph's pins;
         * omitted, they are Arg1, Arg2, ... and Result. A World& or ScriptCall&
         * parameter is filled in and is not a script argument. NativePure is
         * for a query (a const member, usually): a value node in a graph.
         */
        template<auto TFn>
        void Native(const char* InName, const char* InNames = nullptr)     { AddNative<TFn>(InName, InNames, false); }

        template<auto TFn>
        void NativePure(const char* InName, const char* InNames = nullptr) { AddNative<TFn>(InName, InNames, true); }

        bool Commit() { return RegisterComponentScript(T::StaticType().ID, Entries, Count, Rejected); }

    private:
        void AddMethod(const char* InName, const MethodFn InFn, const char* InSignature, const bool bInPure)
        {
            ScriptBindingEntry* Entry = NewEntry(InName);
            if(!Entry) { return; }
            Entry->bMethod = true;
            Entry->bPure   = bInPure;
            Entry->Method  = reinterpret_cast<void (*)()>(InFn);
            Entry->Invoker = &Invoke;

            size_t Length = 0;
            while(InSignature != nullptr && InSignature[Length] != '\0') { ++Length; }
            if(Length < SCRIPT_SIGNATURE_MAX)
            {
                for(size_t Index = 0; Index < Length; ++Index) { Entry->Signature[Index] = InSignature[Index]; }
            }
        }

        template<auto TFn>
        void AddNative(const char* InName, const char* InNames, const bool bInPure)
        {
            using FT = ScriptNative::FnTraits<decltype(TFn)>;
            static_assert(FT::bKnown && FT::bMember, "Bind.Native takes a member function: &Component::Function");
            static_assert(std::is_base_of<typename FT::Class, T>::value, "Bind.Native: the function is not a member of this component");
            using Call = ScriptNative::Call<typename FT::Return, typename FT::Args>;

            char Signature[SCRIPT_SIGNATURE_MAX];
            Call::BuildSignature(InNames, Signature, sizeof(Signature));
            AddMethod(InName, &NativeThunk<TFn>, Signature, bInPure);
        }

        template<auto TFn>
        static void NativeThunk(T& InComponent, ScriptCall& InCall)
        {
            using FT = ScriptNative::FnTraits<decltype(TFn)>;
            ScriptNative::Call<typename FT::Return, typename FT::Args>::Run(InCall, [&InComponent](auto&&... InArgs) -> decltype(auto)
            {
                return (InComponent.*TFn)(std::forward<decltype(InArgs)>(InArgs)...);
            });
        }

        /* Casts back to the exact type Method() erased, which is well-defined. */
        static void Invoke(void (*InMethod)(), void* InComponent, ScriptCall& InCall)
        {
            reinterpret_cast<MethodFn>(InMethod)(*static_cast<T*>(InComponent), InCall);
        }

        template<typename TField>
        void AddField(const char* InName, TField T::* InMember, const ScriptFieldDesc InType, const bool bInReadOnly)
        {
            ScriptBindingEntry* Entry = NewEntry(InName);
            if(!Entry) { return; }

            /* The member's offset, measured on a default-constructed instance
             * -- every component is default-constructible and trivially
             * copyable (GAMECORE_DEFINE_COMPONENT asserts the latter). */
            const T Probe{};
            const char* Base   = reinterpret_cast<const char*>(&Probe);
            const char* Member = reinterpret_cast<const char*>(&(Probe.*InMember));

            Entry->FieldType   = InType.Type;
            Entry->FieldStruct = InType.Struct;
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
 * follows has `Bind` (a ComponentScriptBinder<TypeName>&) in scope. Works the
 * same in a game module: the registrar's destructor unregisters when the
 * module unloads.
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
                bRegistered = Bind.Commit();                                                          \
            }                                                                                         \
            /* Only its own registration: a refused duplicate leaves the first. */                    \
            ~TypeName##_ScriptRegistrar()                                                             \
            {                                                                                         \
                if(bRegistered) { ::GameCore::UnregisterComponentScript(TypeName::StaticType().ID); } \
            }                                                                                         \
            bool bRegistered = false;                                                                 \
        };                                                                                            \
        TypeName##_ScriptRegistrar G##TypeName##_ScriptRegistrar;                                      \
    }                                                                                                 \
    static void TypeName##_ScriptBody(::GameCore::ComponentScriptBinder<TypeName>& Bind)

#endif //GAMECORE_COMPONENTSCRIPT_H
