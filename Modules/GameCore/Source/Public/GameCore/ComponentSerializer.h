#ifndef GAMECORE_COMPONENTSERIALIZER_H
#define GAMECORE_COMPONENTSERIALIZER_H

#include <cstdint>
#include <cstring>     // std::memcpy, for the defaults function the macro generates

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreTypes.h"      // EngineCore::Color, for SerializeColor
#include "Public/GameCore/AABB.h"       // complete, for SerializeAABB's by-reference parameter
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"

/*
 * Per-type serializers, opt-in: a component names its fields explicitly and
 * skips per-run state (Owner, ProxyID, InstanceRef). A raw SizeBytes copy is
 * wrong, not just fragile -- Vector3's layout depends on WITH_SIMD. A type with
 * no serializer is skipped with a warning, never a failure. One registry per
 * process, like RenderCore::RegisterProxyType. Game thread only. See DESIGN.md.
 */

namespace GameCore
{
    /* How an EntityID field crosses the archive: saved as a save-local index,
     * resolved to the newly-created entity on load. A saved {Index, Generation}
     * means nothing in a fresh World. Implemented by WorldSerializer.cpp. */
    class GAMECORE_API IEntityRemapper
    {
    public:
        virtual ~IEntityRemapper() = default;

        /* Unset round-trips as unset; an unresolvable index loads as
         * InvalidEntityID() with a warning rather than failing the load. */
        virtual void RemapEntity(EngineCore::Archive& Ar, EntityID& InOut) = 0;
    };

    /* InComponent points into the pool, so a loading serializer writes live
     * storage directly. Raw function pointer, for SystemFn's reasons. */
    using ComponentSerializeFn = void (*)(EngineCore::Archive& Ar, void* InComponent,
                                          IEntityRemapper& InRemap);

    /* Writes a default-constructed instance into OutBytes (SizeBytes long).
     * What the loader starts each component from -- a zeroed buffer is not a
     * default-constructed component; see DESIGN.md for the ProxyID hang. */
    using ComponentDefaultsFn = void (*)(void* OutBytes);

    struct ComponentSerializerInfo
    {
        ComponentTypeID      Type       = COMPONENT_TYPE_INVALID;
        uint16_t             Version    = 0;
        ComponentSerializeFn Fn         = nullptr;
        ComponentDefaultsFn  DefaultsFn = nullptr;
    };

    /* Called once per type by GAMECORE_DEFINE_COMPONENT_SERIALIZER. InVersion
     * is each instance's DATA chunk version, which the serializer reads back
     * with Ar.GetChunkVersion(). False on a duplicate -- the first
     * registration is kept, as RegisterComponentType does. */
    GAMECORE_API bool RegisterComponentSerializer(ComponentTypeID InType, uint16_t InVersion,
                                                  ComponentSerializeFn InFn,
                                                  ComponentDefaultsFn InDefaultsFn);

    /* nullptr if InType has no serializer -- a normal answer, not an error. */
    GAMECORE_API const ComponentSerializerInfo* FindComponentSerializer(ComponentTypeID InType);

    GAMECORE_API size_t GetComponentSerializerCount();

    /* Three floats through GetX/SetX, never Data[], so SIMD and scalar builds
     * read and write the same twelve bytes. Float because that is what the SIMD
     * path stores. Every Vector3 field goes through this. */
    GAMECORE_API void SerializeVector3(EngineCore::Archive& Ar, EngineCore::Vector3& InOutValue);

    /* Min then Max, each through SerializeVector3. */
    GAMECORE_API void SerializeAABB(EngineCore::Archive& Ar, AABB& InOutValue);

    /* The four channel bytes, so a round trip is exact. */
    GAMECORE_API void SerializeColor(EngineCore::Archive& Ar, EngineCore::Color& InOutValue);
}

/*
 * One per component type, at file scope, in exactly one .cpp -- a header
 * definition registers once per including TU. The body that follows has Ar,
 * C (a TypeName&) and Remap in scope:
 *
 *     GAMECORE_DEFINE_COMPONENT_SERIALIZER(TransformComponent, 1)
 *     {
 *         SerializeVector3(Ar, C.Position);
 *         SerializeVector3(Ar, C.Rotation);
 *         SerializeVector3(Ar, C.Scale);
 *     }
 *
 * To add a field, bump Version and append it behind a check -- the body runs
 * inside the instance's DATA chunk, so Ar.GetChunkVersion() is the version
 * being written on save and the file's on load:
 *
 *     if(Ar.GetChunkVersion() >= 2) { Ar << C.NewField; }
 *
 * An older save then leaves NewField at its default.
 *
 * Never serialize C.Owner: AddComponentRaw writes it, and it holds a live
 * World*. Registration runs from a file-scope object's constructor into a
 * constant-initialised table, the ordering CoreConfig.h's GConfigHead relies on.
 */
#define GAMECORE_DEFINE_COMPONENT_SERIALIZER(TypeName, Version)                                   \
    /* Declared first so the void* trampoline below can forward to it. */                          \
    static void SerializeBody_##TypeName(::EngineCore::Archive& Ar, TypeName& C,                   \
                                         ::GameCore::IEntityRemapper& Remap);                      \
    static void TypeName##_Serialize(::EngineCore::Archive& Ar, void* InComponent,                 \
                                     ::GameCore::IEntityRemapper& Remap)                           \
    {                                                                                              \
        SerializeBody_##TypeName(Ar, *static_cast<TypeName*>(InComponent), Remap);                  \
    }                                                                                              \
    /* memcpy is sound: GAMECORE_DEFINE_COMPONENT asserts trivial copyability. */                 \
    static void TypeName##_Defaults(void* OutBytes)                                                \
    {                                                                                              \
        const TypeName Fresh{};                                                                     \
        ::std::memcpy(OutBytes, &Fresh, sizeof(TypeName));                                          \
    }                                                                                              \
    namespace                                                                                      \
    {                                                                                              \
        struct TypeName##_SerializerRegistrar                                                      \
        {                                                                                          \
            TypeName##_SerializerRegistrar()                                                       \
            {                                                                                      \
                ::GameCore::RegisterComponentSerializer(TypeName::StaticType().ID, (Version),      \
                                                        &TypeName##_Serialize,                     \
                                                        &TypeName##_Defaults);                     \
            }                                                                                      \
        };                                                                                         \
        TypeName##_SerializerRegistrar G##TypeName##_SerializerRegistrar;                           \
    }                                                                                              \
    static void SerializeBody_##TypeName(::EngineCore::Archive& Ar, TypeName& C,                   \
                                         ::GameCore::IEntityRemapper& Remap)

#endif //GAMECORE_COMPONENTSERIALIZER_H
