#ifndef GAMECORE_COMPONENTREPLICATION_H
#define GAMECORE_COMPONENTREPLICATION_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/ScriptStruct.h"

/*
 * Which of a component's fields a server sends to its clients. A component
 * lists them; anything not listed stays local to each process.
 *
 *     GAMECORE_DEFINE_COMPONENT_REPLICATION(TransformComponent)
 *     {
 *         Rep.Field("Position", &TransformComponent::Position);
 *         Rep.Flag("Visible",   &RenderComponent::bVisible);   // a uint8_t 0/1
 *         Rep.Entity("Target",  &TurretComponent::Target);     // sent as a NetId
 *     }
 *
 * Plain data, like the serializer and script tables: GameCore records the
 * offsets, NetCore reads them, diffs each field against what it last sent and
 * encodes the changed ones by TYPE -- never as raw bytes, since Vector3's
 * layout depends on WITH_SIMD.
 *
 * COMPILED OUT WITH NETCORE. A WITH_NETCORE=0 build still compiles the body
 * (so a replicated member that is renamed still fails to build in both) but
 * against a binder that records nothing, and nothing is registered.
 *
 * Server-authoritative: a client never writes these back. Game thread only.
 */

#ifndef WITH_NETCORE
    #error "WITH_NETCORE is not defined -- configure through Engine/CMakeLists.txt, or define it to 0 or 1"
#endif

namespace GameCore
{
    enum : size_t
    {
        /* One bit each in a ReplicatedFieldMask. */
        REPLICATED_FIELDS_PER_TYPE_MAX = 64,
    };

    /* A set of a type's fields, bit N for its Nth: what a state entry carries. */
    using ReplicatedFieldMask = uint64_t;
    static_assert(sizeof(ReplicatedFieldMask) * 8 >= REPLICATED_FIELDS_PER_TYPE_MAX, "a change mask has a bit for every field");

    inline constexpr ReplicatedFieldMask ReplicatedFieldBit(const size_t InField) { return ReplicatedFieldMask(1) << InField; }

    /* How a field is encoded. Value covers every EScriptFieldType but Struct. */
    enum class EReplicatedKind : uint8_t
    {
        Value,    // by FieldType
        Entity,   // an EntityID, sent as the entity's NetId
    };

    enum EReplicatedFieldFlags : uint8_t
    {
        RFF_None        = 0,
        RFF_InitialOnly = 1 << 0,   // sent when the entity spawns on a client, never after
        RFF_Smooth      = 1 << 1,   // a client blends between updates, as it does a Transform (ReplicatedFieldOptions::Smooth)
    };

    /*
     * QUANTIZATION: a Float or Vector3 field sent in fewer bytes, by its
     * options in the table (ReplicatedFieldOptions):
     *
     *     Rep.Field("Position", &TransformComponent::Position).Precision(0.01f);   // whole 0.01s
     *     Rep.Field("Rotation", &TransformComponent::Rotation).Angles();           // degrees, 16 bits
     *
     * Precision(Step): the value as a whole number of Steps, a variable-length
     * integer -- smaller the nearer zero. Angles(): degrees in 65536ths of a
     * turn, two bytes, read back in [-180, 180). A client reads the value
     * rounded so; the server keeps its own, unrounded -- except an avatar's,
     * which its simulation snaps after every step on both ends
     * (QuantizeEntityReplicatedFields), so a predicting client and the server
     * step on from the same values and prediction stays exact.
     */
    enum EReplicatedQuantize : uint8_t
    {
        RQ_None  = 0,
        RQ_Step  = 1,   // whole Steps
        RQ_Angle = 2,   // degrees, 16 bits
    };

    /* The quantization arithmetic, one place for the wire and the snap. */
    namespace ReplicationQuantize
    {
        inline int64_t  ToSteps(const float InValue, const float InStep)   { return int64_t(std::llround(double(InValue) / double(InStep))); }
        inline float    FromSteps(const int64_t InSteps, const float InStep) { return float(double(InSteps) * double(InStep)); }
        inline uint16_t ToAngle(const float InDegrees)
        {
            double Turn = std::fmod(double(InDegrees), 360.0);
            if(Turn < 0.0) { Turn += 360.0; }
            return uint16_t(uint32_t(std::llround(Turn * (65536.0 / 360.0))) & 0xFFFFu);
        }
        inline float FromAngle(const uint16_t InAngle)
        {
            double Degrees = double(InAngle) * (360.0 / 65536.0);
            if(Degrees >= 180.0) { Degrees -= 360.0; }
            return float(Degrees);
        }
        inline float SnapStep(const float InValue, const float InStep) { return FromSteps(ToSteps(InValue, InStep), InStep); }
        inline float SnapAngle(const float InDegrees)                 { return FromAngle(ToAngle(InDegrees)); }
    }

    struct ReplicatedFieldEntry
    {
        char             Name[SCRIPT_BINDING_NAME_MAX] = {};
        EReplicatedKind  Kind      = EReplicatedKind::Value;
        EScriptFieldType FieldType = EScriptFieldType::Float;
        uint8_t          Flags     = RFF_None;
        uint32_t         Offset    = 0;   // from the start of the component (Owner included)
        uint32_t         Size      = 0;
        uint8_t          Quantize  = RQ_None;   // EReplicatedQuantize
        float            Step      = 0.0f;      // RQ_Step's
    };

    /* What Rep.Field returns: how the field just declared travels. Inert if
     * the declaration was refused; a Float or Vector3 only (others ignore it). */
    class ReplicatedFieldOptions
    {
    public:
        explicit ReplicatedFieldOptions(ReplicatedFieldEntry* InEntry) : Entry(InEntry) {}

        /* Whole InStep units (InStep > 0): a variable-length integer. */
        ReplicatedFieldOptions& Precision(const float InStep)
        {
            if(Accepts() && InStep > 0.0f) { Entry->Quantize = RQ_Step; Entry->Step = InStep; }
            return *this;
        }

        /* Degrees, 65536 to the turn: two bytes a value, read back in [-180, 180). */
        ReplicatedFieldOptions& Angles()
        {
            if(Accepts()) { Entry->Quantize = RQ_Angle; Entry->Step = 0.0f; }
            return *this;
        }

        /* A client shows it [Net] InterpolationDelayMs in the past, blended
         * between the server's updates, as it shows a Transform -- the long
         * way round never: an Angles() field turns the short way. Not on an
         * entity the client predicts, nor with a delay of 0: then it is
         * written as it arrives, like any other field. */
        ReplicatedFieldOptions& Smooth()
        {
            if(Accepts()) { Entry->Flags |= RFF_Smooth; }
            return *this;
        }

    private:
        bool Accepts() const
        {
            return Entry && Entry->Kind == EReplicatedKind::Value
                && (Entry->FieldType == EScriptFieldType::Float || Entry->FieldType == EScriptFieldType::Vector3);
        }
        ReplicatedFieldEntry* Entry;
    };

    /* Snaps InType's quantized fields on InComponent to what they read back
     * as. False (nothing done) if it has none. */
    GAMECORE_API bool QuantizeReplicatedFields(ComponentTypeID InType, void* InComponent);

    /* Whether InType has a quantized field: cheap, for a check every tick. */
    GAMECORE_API bool HasQuantizedFields(ComponentTypeID InType);

    /* Every quantized field of every component InEntity has: what an avatar
     * simulation does after each step, on the server and the predicting
     * client alike, so they step on from the same values. */
    GAMECORE_API void QuantizeEntityReplicatedFields(World& InWorld, EntityID InEntity);

    /* Called by the macro. InRejected counts fields the binder could not
     * record (too many, a name too long) and is logged. False on a duplicate
     * (the first is kept). */
    GAMECORE_API bool RegisterComponentReplication(ComponentTypeID InType, const ReplicatedFieldEntry* InEntries,
                                                   size_t InCount, size_t InRejected);

    /* The registrar calls it when its image unloads. */
    GAMECORE_API void UnregisterComponentReplication(ComponentTypeID InType);

    /* nullptr/0 if InType replicates nothing -- a normal answer. */
    GAMECORE_API const ReplicatedFieldEntry* FindComponentReplication(ComponentTypeID InType, size_t& OutCount);

    /* Every type with a table, in ComponentTypeID order. */
    GAMECORE_API void ForEachReplicatedType(void (*InFn)(ComponentTypeID InType, void* InUserData), void* InUserData);

    /* Bumped by every register and unregister of a replication OR an RPC
     * table, so NetCore can rebuild what it derived from them (its schema)
     * after a game module loads or hot reloads. */
    GAMECORE_API uint32_t GetNetTablesGeneration();

    /* What `Rep` is inside GAMECORE_DEFINE_COMPONENT_REPLICATION. Header-only
     * plain data; registration happens at Commit(). */
    template<typename T>
    class ComponentReplicationBinder
    {
    public:
        template<typename TField>
        ReplicatedFieldOptions Field(const char* InName, TField T::* InMember, const uint8_t InFlags = RFF_None)
        {
            static_assert(!IsScriptStruct<TField>::value, "Rep.Field: script structs do not replicate yet; replicate their fields' owner members");
            return ReplicatedFieldOptions(AddField(InName, InMember, EReplicatedKind::Value, ScriptFieldTypeOf<TField>::Value, InFlags));
        }

        /* A uint8_t 0/1 flag. */
        void Flag(const char* InName, uint8_t T::* InMember, const uint8_t InFlags = RFF_None)
        {
            AddField(InName, InMember, EReplicatedKind::Value, EScriptFieldType::Flag, InFlags);
        }

        /* An entity reference. A client sees the entity it knows by the same
         * NetId, or an unset id if it has none. */
        void Entity(const char* InName, EntityID T::* InMember, const uint8_t InFlags = RFF_None)
        {
            AddField(InName, InMember, EReplicatedKind::Entity, EScriptFieldType::Int32, InFlags);
        }

        bool Commit() { return RegisterComponentReplication(T::StaticType().ID, Entries, Count, Rejected); }

    private:
        /* Offsets measured on a default-constructed instance, as
         * ComponentScriptBinder does. Over-long names are rejected, not cut. */
        template<typename TField>
        ReplicatedFieldEntry* AddField(const char* InName, TField T::* InMember, const EReplicatedKind InKind,
                                       const EScriptFieldType InType, const uint8_t InFlags)
        {
            size_t Length = 0;
            while(InName != nullptr && InName[Length] != '\0') { ++Length; }
            if(Count >= REPLICATED_FIELDS_PER_TYPE_MAX || Length == 0 || Length >= SCRIPT_BINDING_NAME_MAX)
            {
                ++Rejected;
                return nullptr;
            }

            const T Probe{};
            ReplicatedFieldEntry& Out = Entries[Count++];
            for(size_t Index = 0; Index < Length; ++Index) { Out.Name[Index] = InName[Index]; }
            Out.Kind      = InKind;
            Out.FieldType = InType;
            Out.Flags     = InFlags;
            Out.Offset    = uint32_t(reinterpret_cast<const char*>(&(Probe.*InMember)) - reinterpret_cast<const char*>(&Probe));
            Out.Size      = uint32_t(sizeof(TField));
            return &Out;
        }

        ReplicatedFieldEntry Entries[REPLICATED_FIELDS_PER_TYPE_MAX];
        size_t               Count    = 0;
        size_t               Rejected = 0;
    };

    /* The binder a WITH_NETCORE=0 build compiles the body against: the same
     * calls, checked the same way, recording nothing. */
    template<typename T>
    class NullReplicationBinder
    {
    public:
        template<typename TField>
        ReplicatedFieldOptions Field(const char*, TField T::*, const uint8_t = RFF_None)
        {
            static_assert(!IsScriptStruct<TField>::value, "Rep.Field: script structs do not replicate yet; replicate their fields' owner members");
            (void)ScriptFieldTypeOf<TField>::Value;
            return ReplicatedFieldOptions(nullptr);
        }
        void Flag(const char*, uint8_t T::*, const uint8_t = RFF_None) {}
        void Entity(const char*, EntityID T::*, const uint8_t = RFF_None) {}
    };
}

/*
 * One per component type, at file scope, in exactly one .cpp. The body that
 * follows has `Rep` in scope. Works the same in a game module: the
 * registrar's destructor unregisters when the module unloads.
 */
#if WITH_NETCORE
#define GAMECORE_DEFINE_COMPONENT_REPLICATION(TypeName)                                                   \
    static void TypeName##_ReplicationBody(::GameCore::ComponentReplicationBinder<TypeName>& Rep);        \
    namespace                                                                                              \
    {                                                                                                      \
        struct TypeName##_ReplicationRegistrar                                                             \
        {                                                                                                  \
            TypeName##_ReplicationRegistrar()                                                              \
            {                                                                                              \
                ::GameCore::ComponentReplicationBinder<TypeName> Rep;                                      \
                TypeName##_ReplicationBody(Rep);                                                           \
                bRegistered = Rep.Commit();                                                                \
            }                                                                                              \
            /* Only its own registration: a refused duplicate leaves the first. */                         \
            ~TypeName##_ReplicationRegistrar()                                                             \
            {                                                                                              \
                if(bRegistered) { ::GameCore::UnregisterComponentReplication(TypeName::StaticType().ID); } \
            }                                                                                              \
            bool bRegistered = false;                                                                      \
        };                                                                                                 \
        TypeName##_ReplicationRegistrar G##TypeName##_ReplicationRegistrar;                                \
    }                                                                                                      \
    static void TypeName##_ReplicationBody(::GameCore::ComponentReplicationBinder<TypeName>& Rep)
#else
#define GAMECORE_DEFINE_COMPONENT_REPLICATION(TypeName)                                                   \
    [[maybe_unused]] static void TypeName##_ReplicationBody(::GameCore::NullReplicationBinder<TypeName>& Rep)
#endif

#endif //GAMECORE_COMPONENTREPLICATION_H
