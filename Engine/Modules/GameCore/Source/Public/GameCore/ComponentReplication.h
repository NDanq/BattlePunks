#ifndef GAMECORE_COMPONENTREPLICATION_H
#define GAMECORE_COMPONENTREPLICATION_H

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
        /* One bit each in a uint32_t change mask. */
        REPLICATED_FIELDS_PER_TYPE_MAX = 32,
    };

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
    };

    struct ReplicatedFieldEntry
    {
        char             Name[SCRIPT_BINDING_NAME_MAX] = {};
        EReplicatedKind  Kind      = EReplicatedKind::Value;
        EScriptFieldType FieldType = EScriptFieldType::Float;
        uint8_t          Flags     = RFF_None;
        uint32_t         Offset    = 0;   // from the start of the component (Owner included)
        uint32_t         Size      = 0;
    };

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
        void Field(const char* InName, TField T::* InMember, const uint8_t InFlags = RFF_None)
        {
            static_assert(!IsScriptStruct<TField>::value, "Rep.Field: script structs do not replicate yet; replicate their fields' owner members");
            AddField(InName, InMember, EReplicatedKind::Value, ScriptFieldTypeOf<TField>::Value, InFlags);
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
        void AddField(const char* InName, TField T::* InMember, const EReplicatedKind InKind,
                      const EScriptFieldType InType, const uint8_t InFlags)
        {
            size_t Length = 0;
            while(InName != nullptr && InName[Length] != '\0') { ++Length; }
            if(Count >= REPLICATED_FIELDS_PER_TYPE_MAX || Length == 0 || Length >= SCRIPT_BINDING_NAME_MAX)
            {
                ++Rejected;
                return;
            }

            const T Probe{};
            ReplicatedFieldEntry& Out = Entries[Count++];
            for(size_t Index = 0; Index < Length; ++Index) { Out.Name[Index] = InName[Index]; }
            Out.Kind      = InKind;
            Out.FieldType = InType;
            Out.Flags     = InFlags;
            Out.Offset    = uint32_t(reinterpret_cast<const char*>(&(Probe.*InMember)) - reinterpret_cast<const char*>(&Probe));
            Out.Size      = uint32_t(sizeof(TField));
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
        void Field(const char*, TField T::*, const uint8_t = RFF_None)
        {
            static_assert(!IsScriptStruct<TField>::value, "Rep.Field: script structs do not replicate yet; replicate their fields' owner members");
            (void)ScriptFieldTypeOf<TField>::Value;
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
