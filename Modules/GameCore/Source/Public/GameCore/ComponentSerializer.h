#ifndef GAMECORE_COMPONENTSERIALIZER_H
#define GAMECORE_COMPONENTSERIALIZER_H

#include <cstddef>
#include <cstdint>
#include <cstring>     // std::memcpy, for the defaults function the macro generates

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreTypes.h"      // EngineCore::Color
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/Math/Vector.h"

/*
 * Per-type serializers, opt-in: a component names its fields explicitly and
 * skips per-run state (Owner, ProxyID, a render proxy). A raw SizeBytes copy is
 * wrong, not just fragile -- Vector3's layout depends on WITH_SIMD. A type with
 * no serializer is skipped with a warning, never a failure. One registry per
 * process, like RenderCore::RegisterProxyType. Game thread only. See DESIGN.md.
 *
 * Every field is NAMED, because the same serializer writes two formats:
 *
 *   binary   the cooked world and save games (WorldSerializer.h). Names are
 *            ignored; fields are written in call order, so the bytes are
 *            exactly what positional `Ar << x` produced.
 *   text     the editor's .nmworld/.nmentity XML (WorldPackage.h, editor
 *            builds only). One element per field, matched by name on load: a
 *            missing field keeps its default and an unknown one is skipped.
 */

namespace GameCore
{
    /*
     * What a serializer body writes through (`Ar` inside the macro). Saving and
     * loading are the same calls, reading or writing the reference given.
     *
     * Names must be unique within one component and valid XML element names
     * (letters, digits, underscore; not starting with a digit).
     */
    class GAMECORE_API ComponentArchive
    {
    public:
        virtual ~ComponentArchive() = default;

        virtual bool IsLoading() const = 0;
        bool IsSaving() const { return !IsLoading(); }

        /* The version being written on save; the file's on load. A binary
         * field added later needs `if(Ar.GetVersion() >= N)`; text does not
         * (a missing element keeps its default), but the guard is harmless. */
        virtual uint16_t GetVersion() const = 0;

        virtual void Field(const char* InName, float& InOut)    = 0;
        virtual void Field(const char* InName, double& InOut)   = 0;
        virtual void Field(const char* InName, int32_t& InOut)  = 0;
        virtual void Field(const char* InName, uint32_t& InOut) = 0;
        /* A number 0..255. For a 0/1 flag use Flag(), which reads as a
         * boolean in text and is the same byte in binary. */
        virtual void Field(const char* InName, uint8_t& InOut)  = 0;
        virtual void Field(const char* InName, bool& InOut)     = 0;
        /* Three floats through GetX/SetX, never Data[], so SIMD and scalar
         * builds agree. */
        virtual void Field(const char* InName, EngineCore::Vector3& InOut) = 0;
        virtual void Field(const char* InName, AABB& InOut) = 0;
        /* The four channel bytes, so a round trip is exact. */
        virtual void Field(const char* InName, EngineCore::Color& InOut) = 0;
        /* A NUL-terminated char buffer of InCapacity bytes, re-clamped on load. */
        virtual void FixedString(const char* InName, char* InOutBuffer, size_t InCapacity) = 0;

        template<size_t N>
        void Field(const char* InName, char (&InOutBuffer)[N]) { FixedString(InName, InOutBuffer, N); }

        virtual void Flag(const char* InName, uint8_t& InOut) = 0;

        /*
         * An EntityID field. Crosses as something that survives the process --
         * a save-local index in binary, the target's EntityGuid in text -- and
         * is resolved back to the loaded entity. Unset round-trips as unset;
         * an unresolvable reference loads as InvalidEntityID() with a warning
         * rather than failing the load.
         */
        virtual void Entity(const char* InName, EntityID& InOut) = 0;
    };

    /* InComponent points into the pool, so a loading serializer writes live
     * storage directly. Raw function pointer, for SystemFn's reasons. */
    using ComponentSerializeFn = void (*)(ComponentArchive& Ar, void* InComponent);

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
     * is what Ar.GetVersion() reports while saving, and what each saved
     * instance records. False on a duplicate -- the first registration is
     * kept, as RegisterComponentType does. */
    GAMECORE_API bool RegisterComponentSerializer(ComponentTypeID InType, uint16_t InVersion,
                                                  ComponentSerializeFn InFn,
                                                  ComponentDefaultsFn InDefaultsFn);

    /* nullptr if InType has no serializer -- a normal answer, not an error. */
    GAMECORE_API const ComponentSerializerInfo* FindComponentSerializer(ComponentTypeID InType);

    GAMECORE_API size_t GetComponentSerializerCount();

    // === The binary backend =================================================

    /* How a binary EntityID field crosses the archive: saved as a save-local
     * index, resolved to the newly-created entity on load. Implemented by
     * WorldSerializer.cpp. */
    class GAMECORE_API IEntityRemapper
    {
    public:
        virtual ~IEntityRemapper() = default;
        virtual void RemapEntity(EngineCore::Archive& Ar, EntityID& InOut) = 0;
    };

    /* ComponentArchive over an EngineCore::Archive, positionally. Constructed
     * inside the instance's DATA chunk, so the version is that chunk's. */
    class GAMECORE_API BinaryComponentArchive final : public ComponentArchive
    {
    public:
        BinaryComponentArchive(EngineCore::Archive& InAr, IEntityRemapper& InRemap) : Ar(InAr), Remap(InRemap) {}

        bool     IsLoading() const override;
        uint16_t GetVersion() const override;

        using ComponentArchive::Field;
        void Field(const char* InName, float& InOut) override;
        void Field(const char* InName, double& InOut) override;
        void Field(const char* InName, int32_t& InOut) override;
        void Field(const char* InName, uint32_t& InOut) override;
        void Field(const char* InName, uint8_t& InOut) override;
        void Field(const char* InName, bool& InOut) override;
        void Field(const char* InName, EngineCore::Vector3& InOut) override;
        void Field(const char* InName, AABB& InOut) override;
        void Field(const char* InName, EngineCore::Color& InOut) override;
        void FixedString(const char* InName, char* InOutBuffer, size_t InCapacity) override;
        void Flag(const char* InName, uint8_t& InOut) override;
        void Entity(const char* InName, EntityID& InOut) override;

    private:
        EngineCore::Archive& Ar;
        IEntityRemapper&     Remap;
    };
}

/*
 * One per component type, at file scope, in exactly one .cpp -- a header
 * definition registers once per including TU. The body that follows has Ar (a
 * ComponentArchive&) and C (a TypeName&) in scope:
 *
 *     GAMECORE_DEFINE_COMPONENT_SERIALIZER(TransformComponent, 1)
 *     {
 *         Ar.Field("Position", C.Position);
 *         Ar.Field("Rotation", C.Rotation);
 *         Ar.Field("Scale",    C.Scale);
 *     }
 *
 * To add a field, bump Version and append it behind a check, so an older
 * BINARY save (cooked data, a save game) leaves it at its default:
 *
 *     if(Ar.GetVersion() >= 2) { Ar.Field("NewField", C.NewField); }
 *
 * Never reorder or rename a field without thinking about both formats: binary
 * depends on the order, text on the name.
 *
 * Never serialize C.Owner: AddComponentRaw writes it, and it holds a live
 * World*. Registration runs from a file-scope object's constructor into a
 * constant-initialised table, the ordering CoreConfig.h's GConfigHead relies on.
 */
#define GAMECORE_DEFINE_COMPONENT_SERIALIZER(TypeName, Version)                                   \
    /* Declared first so the void* trampoline below can forward to it. */                          \
    static void SerializeBody_##TypeName(::GameCore::ComponentArchive& Ar, TypeName& C);           \
    static void TypeName##_Serialize(::GameCore::ComponentArchive& Ar, void* InComponent)          \
    {                                                                                              \
        SerializeBody_##TypeName(Ar, *static_cast<TypeName*>(InComponent));                        \
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
    static void SerializeBody_##TypeName(::GameCore::ComponentArchive& Ar, TypeName& C)

#endif //GAMECORE_COMPONENTSERIALIZER_H
