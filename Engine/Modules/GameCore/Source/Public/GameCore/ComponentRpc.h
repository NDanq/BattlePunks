#ifndef GAMECORE_COMPONENTRPC_H
#define GAMECORE_COMPONENTRPC_H

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/ScriptStruct.h"
#include "Public/Math/Vector.h"

/*
 * Remote procedure calls: member functions of a component that one process
 * asks another to run on the same entity.
 *
 *     struct ShipComponent
 *     {
 *         GAMECORE_COMPONENT_BODY()
 *         void ServerFire(const EngineCore::Vector3& InAim);                      // runs on the server
 *         void ClientHit(const GameCore::RpcContext& InCtx, int32_t InDamage);    // context is optional
 *     };
 *
 *     GAMECORE_DEFINE_COMPONENT_RPCS(ShipComponent)                               // in one .cpp
 *     {
 *         Rpc.Server<&ShipComponent::ServerFire>("ServerFire");
 *         Rpc.Client<&ShipComponent::ClientHit>("ClientHit");
 *     }
 *
 *     GAMECORE_RPC(ShipComponent, ServerFire)(Ship, Aim);                         // the call
 *
 * RATE LIMITS. A server takes only so many Server RPCs from each client
 * ([Net] RpcPerSecond, all of them together) and drops the rest; a client
 * that keeps flooding is disconnected (NetCore/README.md, "RPC limits"). One
 * RPC can be held to less, so a client cannot fire faster than its gun:
 *
 *         Rpc.Server<&ShipComponent::ServerFire>("ServerFire").RateLimit(8);   // calls a second, at most
 *
 * Targets:
 *     Server      a client asks the server; on the server it simply runs.
 *     Client      the server asks the entity's owning client (NetComponent).
 *     Multicast   the server runs it and asks every client.
 *
 * Arguments are values, encoded one at a time (never raw bytes): float,
 * double, int32_t, uint32_t, uint8_t, bool, Vector3, Color, EntityID (sent as
 * the entity's NetId), RpcString<N> and RpcFloats<N>. By value or const&.
 *
 * WHERE A CALL GOES is the installed IRpcSink's decision. GameCore's own sink
 * runs every call here and now, so a game with NetCore compiled out -- or
 * simply not connected -- behaves as its own server and nothing is dropped.
 * NetCore installs a sink that routes by role (NetSession.h).
 *
 * Unlike the replication table, this one is always compiled: the local sink
 * needs it. Game thread only.
 */

namespace GameCore
{
    /* A client's id on its server. The server itself, and every process that
     * is not networked, is NET_CLIENT_NONE. */
    using NetClientID = int32_t;
    constexpr NetClientID NET_CLIENT_NONE = -1;

    enum class ERpcTarget : uint8_t
    {
        Server,
        Client,      // the owning client
        Multicast,
    };

    /* What a handler may ask about the call: taken as an optional first
     * parameter, `const GameCore::RpcContext&`. */
    struct RpcContext
    {
        World*      OwningWorld  = nullptr;
        EntityID    Entity;
        NetClientID SenderClient = NET_CLIENT_NONE;   // who asked; NONE for the server or a local call
        bool        bRemote      = false;             // arrived over the network
        /* A client's call, on the server: the server time (NetFacade's
         * GetNetTimeMs) of what that client was showing as it called -- how
         * far back to look for what it aimed at (LagCompensation.h). 0 for a
         * local call: now. */
        uint32_t    ViewTimeMs   = 0;
    };

    /* A NUL-terminated text argument of at most N - 1 bytes. */
    template<size_t N>
    struct RpcString
    {
        static_assert(N > 1, "RpcString<N>: N includes the NUL");
        char Text[N] = {};

        RpcString() = default;
        RpcString(const char* InText) { Set(InText); }   // implicit, so a literal can be passed
        void Set(const char* InText)
        {
            size_t Length = 0;
            while(InText != nullptr && InText[Length] != '\0' && Length + 1 < N) { Text[Length] = InText[Length]; ++Length; }
            Text[Length] = '\0';
        }
        const char* c_str() const { return Text; }
    };

    /* Up to N floats, of which Count are sent: an axis set, a short curve. */
    template<size_t N>
    struct RpcFloats
    {
        static_assert(N > 0 && N <= 255, "RpcFloats<N>: 1..255 values");
        uint8_t Count = 0;
        float   Values[N] = {};
    };

    enum : size_t
    {
        RPCS_PER_TYPE_MAX     = 32,
        RPC_ARG_SIGNATURE_MAX = 16,   // one letter per argument, including the NUL
    };

    /* Decodes the arguments from InArgs and calls the member. False if they
     * did not decode (the call is skipped). */
    using RpcInvoker = bool (*)(void* InComponent, EngineCore::Archive& InArgs, const RpcContext& InContext);

    struct RpcEntry
    {
        char       Name[SCRIPT_BINDING_NAME_MAX] = {};
        ERpcTarget Target = ERpcTarget::Server;
        /* Its position in the type's table, which is sorted by name, so the
         * same on every process with the same code: the wire's RPC id. */
        uint8_t    Index  = 0;
        /* The argument types, one letter each (RpcArgs::Traits::Code): checked
         * at every call, so a caller compiled against another signature is
         * refused rather than misread. */
        char       ArgSignature[RPC_ARG_SIGNATURE_MAX] = {};
        RpcInvoker Invoker = nullptr;
        /* The most a second one client may send of it (a Server RPC, on the
         * server); 0: only the client's overall budget. Not in the schema:
         * it is the server's own rule. */
        float      MaxPerSecond = 0.0f;
    };

    /* What Rpc.Server/Client/Multicast return: options for the RPC just
     * declared. Inert if the declaration was refused. */
    class RpcDeclaration
    {
    public:
        explicit RpcDeclaration(RpcEntry* InEntry) : Entry(InEntry) {}

        /* At most InPerSecond calls a second from one client; past it the
         * server drops them. Bursts of up to a second's worth pass. */
        RpcDeclaration& RateLimit(const float InPerSecond)
        {
            if(Entry) { Entry->MaxPerSecond = InPerSecond > 0.0f ? InPerSecond : 0.0f; }
            return *this;
        }

    private:
        RpcEntry* Entry;
    };

    /* Called by the macro; sorts InEntries by name and numbers them. False on
     * a duplicate (the first is kept). InRejected is logged. */
    GAMECORE_API bool RegisterComponentRpcs(ComponentTypeID InType, const RpcEntry* InEntries, size_t InCount,
                                            size_t InRejected);
    GAMECORE_API void UnregisterComponentRpcs(ComponentTypeID InType);

    /* nullptr/0 if InType has none. Sorted by name; Entries[I].Index == I. */
    GAMECORE_API const RpcEntry* FindComponentRpcs(ComponentTypeID InType, size_t& OutCount);
    GAMECORE_API const RpcEntry* FindComponentRpc(ComponentTypeID InType, const char* InName);

    /*
     * Where calls go. One per process. Route decides; WriteEntity/ReadEntity
     * are how an EntityID argument is put on and taken off the wire (an id
     * means nothing in another process).
     */
    class GAMECORE_API IRpcSink
    {
    public:
        virtual ~IRpcSink() = default;

        /* InArgs are the encoded arguments. */
        virtual void Route(World& InWorld, EntityID InEntity, ComponentTypeID InType, const RpcEntry& InRpc,
                           const std::vector<uint8_t>& InArgs) = 0;

        virtual void     WriteEntity(World& InWorld, EntityID InEntity, EngineCore::Archive& Ar);
        virtual EntityID ReadEntity(World& InWorld, EngineCore::Archive& Ar);
    };

    /* nullptr restores GameCore's local sink. The sink must outlive its
     * installation. */
    GAMECORE_API void      SetRpcSink(IRpcSink* InSink);
    GAMECORE_API IRpcSink& GetRpcSink();

    /* Runs InRpc on InEntity's InType component with InArgs. False (warned)
     * if the entity or component is gone, or the arguments did not decode.
     * InViewTimeMs is RpcContext::ViewTimeMs. */
    GAMECORE_API bool InvokeRpc(World& InWorld, EntityID InEntity, ComponentTypeID InType, const RpcEntry& InRpc,
                                const std::vector<uint8_t>& InArgs, NetClientID InSender, bool bInRemote,
                                uint32_t InViewTimeMs = 0);

    /* Used by CallRpc: logs why a call was refused. */
    GAMECORE_API void ReportRpcCallError(ComponentTypeID InType, const char* InName, const char* InReason);

    class ScriptCall;

    /*
     * The script side of CallRpc: InCall's arguments, from InFirstArg, read as
     * InRpc's argument types (its ArgSignature) and handed to the sink -- so a
     * script's call is routed exactly as C++'s is. A Vector3 is three values,
     * a Color four (alpha optional when it is last), an entity an Entity, a
     * string a string. False, with InCall's error set, if the entity is not
     * alive or the arguments do not fit.
     *
     * Every component proxy in Lua answers its type's RPCs by name --
     * ship:ServerFire(x, y, z) -- and the Net library's Net.Call reaches any.
     */
    GAMECORE_API bool CallRpcFromScript(ScriptCall& InCall, int InFirstArg, World& InWorld, EntityID InEntity,
                                        ComponentTypeID InType, const RpcEntry& InRpc);

    // === Argument encoding ===================================================

    namespace RpcArgs
    {
        template<typename T> using Bare = std::remove_cv_t<std::remove_reference_t<T>>;

        template<typename T, typename = void> struct Traits { static constexpr bool bSupported = false; };

        template<typename T, char TCode>
        struct ScalarTraits
        {
            static constexpr bool bSupported = true;
            static constexpr char Code = TCode;
            static void Write(EngineCore::Archive& Ar, World&, const T& InValue) { T Copy = InValue; Ar << Copy; }
            static T    Read(EngineCore::Archive& Ar, World&)                    { T Value{}; Ar << Value; return Value; }
        };

        template<> struct Traits<float>    : ScalarTraits<float,    'f'> {};
        template<> struct Traits<double>   : ScalarTraits<double,   'd'> {};
        template<> struct Traits<int32_t>  : ScalarTraits<int32_t,  'i'> {};
        template<> struct Traits<uint32_t> : ScalarTraits<uint32_t, 'u'> {};
        template<> struct Traits<uint8_t>  : ScalarTraits<uint8_t,  'b'> {};
        template<> struct Traits<bool>     : ScalarTraits<bool,     'z'> {};

        /* Three floats through the accessors, as BinaryComponentArchive does. */
        template<> struct Traits<EngineCore::Vector3>
        {
            static constexpr bool bSupported = true;
            static constexpr char Code = 'v';
            static void Write(EngineCore::Archive& Ar, World&, const EngineCore::Vector3& InValue)
            {
                float X = float(InValue.GetX()), Y = float(InValue.GetY()), Z = float(InValue.GetZ());
                Ar << X << Y << Z;
            }
            static EngineCore::Vector3 Read(EngineCore::Archive& Ar, World&)
            {
                float X = 0.0f, Y = 0.0f, Z = 0.0f;
                Ar << X << Y << Z;
                EngineCore::Vector3 Value;
                Value.SetX(X); Value.SetY(Y); Value.SetZ(Z);
                return Value;
            }
        };

        template<> struct Traits<EngineCore::Color>
        {
            static constexpr bool bSupported = true;
            static constexpr char Code = 'c';
            static void Write(EngineCore::Archive& Ar, World&, const EngineCore::Color& InValue)
            {
                EngineCore::Color Copy = InValue;
                Ar << Copy.R << Copy.G << Copy.B << Copy.A;
            }
            static EngineCore::Color Read(EngineCore::Archive& Ar, World&)
            {
                EngineCore::Color Value;
                Ar << Value.R << Value.G << Value.B << Value.A;
                return Value;
            }
        };

        template<> struct Traits<EntityID>
        {
            static constexpr bool bSupported = true;
            static constexpr char Code = 'e';
            static void     Write(EngineCore::Archive& Ar, World& InWorld, const EntityID& InValue) { GetRpcSink().WriteEntity(InWorld, InValue, Ar); }
            static EntityID Read(EngineCore::Archive& Ar, World& InWorld)                           { return GetRpcSink().ReadEntity(InWorld, Ar); }
        };

        /* An Entity handle is passed as its id. */
        template<> struct Traits<Entity>
        {
            static constexpr bool bSupported = true;
            static constexpr char Code = 'e';
            static void   Write(EngineCore::Archive& Ar, World& InWorld, const Entity& InValue) { GetRpcSink().WriteEntity(InWorld, InValue.GetID(), Ar); }
            static Entity Read(EngineCore::Archive& Ar, World& InWorld)                         { return Entity(&InWorld, GetRpcSink().ReadEntity(InWorld, Ar)); }
        };

        template<size_t N> struct Traits<RpcString<N>>
        {
            static constexpr bool bSupported = true;
            static constexpr char Code = 's';
            static void Write(EngineCore::Archive& Ar, World&, const RpcString<N>& InValue)
            {
                RpcString<N> Copy = InValue;
                Ar.SerializeFixedString(Copy.Text, N);
            }
            static RpcString<N> Read(EngineCore::Archive& Ar, World&)
            {
                RpcString<N> Value;
                Ar.SerializeFixedString(Value.Text, N);
                return Value;
            }
        };

        /* Its count, then that many floats; a count past N does not decode. */
        template<size_t N> struct Traits<RpcFloats<N>>
        {
            static constexpr bool bSupported = true;
            static constexpr char Code = 'F';
            static void Write(EngineCore::Archive& Ar, World&, const RpcFloats<N>& InValue)
            {
                uint8_t Count = InValue.Count <= N ? InValue.Count : uint8_t(N);
                Ar << Count;
                for(uint8_t Index = 0; Index < Count; ++Index) { float Value = InValue.Values[Index]; Ar << Value; }
            }
            static RpcFloats<N> Read(EngineCore::Archive& Ar, World&)
            {
                RpcFloats<N> Value;
                Ar << Value.Count;
                if(Value.Count > N) { Ar.SetError("RpcFloats: more values than the parameter holds"); Value.Count = 0; return Value; }
                for(uint8_t Index = 0; Index < Value.Count; ++Index) { Ar << Value.Values[Index]; }
                return Value;
            }
        };

        /* A handler's parameters, without a leading `const RpcContext&`. */
        template<typename TTuple> struct StripContext
        {
            using Params = TTuple;
            static constexpr bool bHasContext = false;
        };
        template<typename... A> struct StripContext<std::tuple<const RpcContext&, A...>>
        {
            using Params = std::tuple<A...>;
            static constexpr bool bHasContext = true;
        };

        template<typename TFn> struct FnTraits { static constexpr bool bKnown = false; };
        template<typename TClass, typename R, typename... A> struct FnTraits<R (TClass::*)(A...)>
        {
            static constexpr bool bKnown = true;
            using Class = TClass; using Return = R; using Args = std::tuple<A...>;
        };
        template<typename TClass, typename R, typename... A> struct FnTraits<R (TClass::*)(A...) noexcept>
            : FnTraits<R (TClass::*)(A...)> {};

        template<typename TParams> struct Codec;
        template<typename... P> struct Codec<std::tuple<P...>>
        {
            static_assert(sizeof...(P) + 1 <= RPC_ARG_SIGNATURE_MAX, "an RPC takes at most 15 arguments");
            static_assert((Traits<Bare<P>>::bSupported && ...),
                          "RPC: an argument type cannot be sent. Supported: float, double, int32_t, uint32_t, uint8_t, "
                          "bool, Vector3, Color, EntityID, Entity, RpcString<N> and RpcFloats<N>, by value or const&");
            static_assert(((!std::is_lvalue_reference<P>::value || std::is_const<std::remove_reference_t<P>>::value) && ...),
                          "RPC: arguments are values; take them by value or const&");

            static void Signature(char (&OutSignature)[RPC_ARG_SIGNATURE_MAX])
            {
                const char Codes[] = { Traits<Bare<P>>::Code..., '\0' };
                for(size_t Index = 0; Index < sizeof(Codes); ++Index) { OutSignature[Index] = Codes[Index]; }
            }

            /* Each argument converted to its parameter's type first, so a
             * literal 3 for a float parameter is sent as a float. */
            template<typename... A>
            static void Write(EngineCore::Archive& Ar, World& InWorld, A&&... InArgs)
            {
                static_assert(sizeof...(A) == sizeof...(P), "RPC: wrong number of arguments");
                (Traits<Bare<P>>::Write(Ar, InWorld, Bare<P>(std::forward<A>(InArgs))), ...);
            }

            /* Left to right: a braced list. */
            static std::tuple<Bare<P>...> Read(EngineCore::Archive& Ar, World& InWorld)
            {
                return std::tuple<Bare<P>...>{ Traits<Bare<P>>::Read(Ar, InWorld)... };
            }
        };

        inline bool SameSignature(const char* InA, const char* InB)
        {
            size_t Index = 0;
            for(; InA[Index] != '\0' && InB[Index] != '\0'; ++Index)
            {
                if(InA[Index] != InB[Index]) { return false; }
            }
            return InA[Index] == InB[Index];
        }
    }

    /* What `Rpc` is inside GAMECORE_DEFINE_COMPONENT_RPCS. Plain data;
     * registration happens at Commit(). */
    template<typename T>
    class ComponentRpcBinder
    {
    public:
        template<auto TFn> RpcDeclaration Server(const char* InName)    { return RpcDeclaration(Add<TFn>(InName, ERpcTarget::Server)); }
        template<auto TFn> RpcDeclaration Client(const char* InName)    { return RpcDeclaration(Add<TFn>(InName, ERpcTarget::Client)); }
        template<auto TFn> RpcDeclaration Multicast(const char* InName) { return RpcDeclaration(Add<TFn>(InName, ERpcTarget::Multicast)); }

        bool Commit() { return RegisterComponentRpcs(T::StaticType().ID, Entries, Count, Rejected); }

    private:
        template<auto TFn>
        RpcEntry* Add(const char* InName, const ERpcTarget InTarget)
        {
            using FT = RpcArgs::FnTraits<decltype(TFn)>;
            static_assert(FT::bKnown, "Rpc.Server/Client/Multicast take a non-const member function: &Component::Function");
            static_assert(std::is_base_of<typename FT::Class, T>::value, "RPC: the function is not a member of this component");
            static_assert(std::is_void<typename FT::Return>::value, "RPC: a handler returns void -- answer with another RPC");
            using Params = typename RpcArgs::StripContext<typename FT::Args>::Params;

            size_t Length = 0;
            while(InName != nullptr && InName[Length] != '\0') { ++Length; }
            if(Count >= RPCS_PER_TYPE_MAX || Length == 0 || Length >= SCRIPT_BINDING_NAME_MAX)
            {
                ++Rejected;
                return nullptr;
            }

            RpcEntry& Entry = Entries[Count++];
            for(size_t Index = 0; Index < Length; ++Index) { Entry.Name[Index] = InName[Index]; }
            Entry.Target  = InTarget;
            Entry.Invoker = &Thunk<TFn>;
            RpcArgs::Codec<Params>::Signature(Entry.ArgSignature);
            return &Entry;
        }

        template<auto TFn>
        static bool Thunk(void* InComponent, EngineCore::Archive& Ar, const RpcContext& InContext)
        {
            using FT    = RpcArgs::FnTraits<decltype(TFn)>;
            using Strip = RpcArgs::StripContext<typename FT::Args>;
            auto Values = RpcArgs::Codec<typename Strip::Params>::Read(Ar, *InContext.OwningWorld);
            if(Ar.HasError()) { return false; }

            T& Component = *static_cast<T*>(InComponent);
            std::apply([&](auto&... InValues)
            {
                if constexpr(Strip::bHasContext) { (Component.*TFn)(InContext, InValues...); }
                else                             { (Component.*TFn)(InValues...); }
            }, Values);
            return true;
        }

        RpcEntry Entries[RPCS_PER_TYPE_MAX];
        size_t   Count    = 0;
        size_t   Rejected = 0;
    };

    /*
     * Encodes the arguments and hands the call to the sink. InName is the
     * name the RPC was registered under (GAMECORE_RPC passes the member's
     * own). False, and logged, if InEntity has no live world, the type has
     * no such RPC, or it was registered with other argument types.
     */
    template<auto TFn, typename... A>
    bool CallRpc(const Entity& InEntity, const char* InName, A&&... InArgs)
    {
        using FT    = RpcArgs::FnTraits<decltype(TFn)>;
        static_assert(FT::bKnown, "CallRpc takes a non-const member function: &Component::Function");
        using Class = typename FT::Class;
        using Coder = RpcArgs::Codec<typename RpcArgs::StripContext<typename FT::Args>::Params>;

        const ComponentTypeID Type = Class::StaticType().ID;
        World* const OwningWorld = InEntity.GetWorld();
        if(!OwningWorld || !InEntity.IsAlive())
        {
            ReportRpcCallError(Type, InName, "the entity is not alive");
            return false;
        }

        const RpcEntry* Rpc = FindComponentRpc(Type, InName);
        if(!Rpc)
        {
            ReportRpcCallError(Type, InName, "no RPC by that name is registered (GAMECORE_DEFINE_COMPONENT_RPCS)");
            return false;
        }

        char Signature[RPC_ARG_SIGNATURE_MAX] = {};
        Coder::Signature(Signature);
        if(!RpcArgs::SameSignature(Signature, Rpc->ArgSignature))
        {
            ReportRpcCallError(Type, InName, "it was registered with other argument types");
            return false;
        }

        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        Coder::Write(Ar, *OwningWorld, std::forward<A>(InArgs)...);
        GetRpcSink().Route(*OwningWorld, InEntity.GetID(), Type, *Rpc, Ar.GetBytes());
        return true;
    }

    /* What GAMECORE_RPC makes: CallRpc with the name bound. */
    template<auto TFn>
    struct RpcCaller
    {
        const char* Name;

        template<typename... A>
        bool operator()(const Entity& InEntity, A&&... InArgs) const
        {
            return CallRpc<TFn>(InEntity, Name, std::forward<A>(InArgs)...);
        }
    };
}

/* GAMECORE_RPC(ShipComponent, ServerFire)(Ship, Aim): the RPC registered
 * under the member's own name. */
#define GAMECORE_RPC(TypeName, FunctionName) \
    ::GameCore::RpcCaller<&TypeName::FunctionName>{ #FunctionName }

/*
 * One per component type, at file scope, in exactly one .cpp. The body that
 * follows has `Rpc` (a ComponentRpcBinder<TypeName>&) in scope. Works the
 * same in a game module: the registrar's destructor unregisters when the
 * module unloads.
 */
#define GAMECORE_DEFINE_COMPONENT_RPCS(TypeName)                                                   \
    static void TypeName##_RpcBody(::GameCore::ComponentRpcBinder<TypeName>& Rpc);                \
    namespace                                                                                       \
    {                                                                                               \
        struct TypeName##_RpcRegistrar                                                              \
        {                                                                                           \
            TypeName##_RpcRegistrar()                                                               \
            {                                                                                       \
                ::GameCore::ComponentRpcBinder<TypeName> Rpc;                                       \
                TypeName##_RpcBody(Rpc);                                                            \
                bRegistered = Rpc.Commit();                                                         \
            }                                                                                       \
            /* Only its own registration: a refused duplicate leaves the first. */                  \
            ~TypeName##_RpcRegistrar()                                                              \
            {                                                                                       \
                if(bRegistered) { ::GameCore::UnregisterComponentRpcs(TypeName::StaticType().ID); } \
            }                                                                                       \
            bool bRegistered = false;                                                               \
        };                                                                                          \
        TypeName##_RpcRegistrar G##TypeName##_RpcRegistrar;                                         \
    }                                                                                               \
    static void TypeName##_RpcBody(::GameCore::ComponentRpcBinder<TypeName>& Rpc)

#endif //GAMECORE_COMPONENTRPC_H
