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
 * Opt-in per-type serializers naming each field and skipping per-run state. (A
 * raw byte copy is wrong: Vector3's layout depends on WITH_SIMD.) Types
 * without one are skipped with a warning. Game thread only. See DESIGN.md.
 *
 * Fields are NAMED because one serializer writes two formats:
 *
 *   binary   cooked worlds and saves (WorldSerializer.h): call order, names
 *            ignored.
 *   text     the editor's XML (WorldPackage.h): matched by name; missing
 *            fields keep defaults, unknown ones are skipped.
 */

namespace GameCore
{
    /*
     * `Ar` inside the macro; the same calls save and load. Names must be
     * unique per component and valid XML element names.
     */
    class GAMECORE_API ComponentArchive
    {
    public:
        virtual ~ComponentArchive() = default;

        virtual bool IsLoading() const = 0;
        bool IsSaving() const { return !IsLoading(); }

        /* Current version on save, the file's on load. Guard later fields
         * with `if(Ar.GetVersion() >= N)` (needed for binary). */
        virtual uint16_t GetVersion() const = 0;

        virtual void Field(const char* InName, float& InOut)    = 0;
        virtual void Field(const char* InName, double& InOut)   = 0;
        virtual void Field(const char* InName, int32_t& InOut)  = 0;
        virtual void Field(const char* InName, uint32_t& InOut) = 0;
        /* 0..255. Use Flag() for a 0/1 flag (a boolean in text). */
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

        /* Stored as a save-local index (binary) or EntityGuid (text) and
         * resolved on load. Unresolvable loads as InvalidEntityID(), warned. */
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

    /* Called by the macro. InVersion is the saving version. False on a
     * duplicate (first kept). */
    GAMECORE_API bool RegisterComponentSerializer(ComponentTypeID InType, uint16_t InVersion,
                                                  ComponentSerializeFn InFn,
                                                  ComponentDefaultsFn InDefaultsFn);

    /* nullptr if InType has no serializer -- a normal answer, not an error. */
    GAMECORE_API const ComponentSerializerInfo* FindComponentSerializer(ComponentTypeID InType);

    GAMECORE_API size_t GetComponentSerializerCount();

    // === The binary backend =================================================

    /* Maps binary EntityID fields to save-local indices (WorldSerializer.cpp). */
    class GAMECORE_API IEntityRemapper
    {
    public:
        virtual ~IEntityRemapper() = default;
        virtual void RemapEntity(EngineCore::Archive& Ar, EntityID& InOut) = 0;
    };

    /* Positional, inside the instance's DATA chunk (whose version it reports). */
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
 * Once per type, at file scope in one .cpp. The body has Ar and C in scope:
 *
 *     GAMECORE_DEFINE_COMPONENT_SERIALIZER(TransformComponent, 1)
 *     {
 *         Ar.Field("Position", C.Position);
 *         Ar.Field("Rotation", C.Rotation);
 *         Ar.Field("Scale",    C.Scale);
 *     }
 *
 * To add a field, bump Version and append it behind a check:
 *
 *     if(Ar.GetVersion() >= 2) { Ar.Field("NewField", C.NewField); }
 *
 * Binary depends on field order, text on names: never reorder or rename
 * casually. Never serialize C.Owner (a live World*).
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
