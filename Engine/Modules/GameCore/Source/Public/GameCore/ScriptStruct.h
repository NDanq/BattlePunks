#ifndef GAMECORE_SCRIPTSTRUCT_H
#define GAMECORE_SCRIPTSTRUCT_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/Math/Vector.h"

/*
 * Script value types: the field types scripts can read and write, and script
 * structs -- small value types a game declares itself, so a component field,
 * a library variable or a Native parameter can be one.
 *
 *     struct PickupStats                       // in a header
 *     {
 *         GAMECORE_SCRIPT_STRUCT_BODY()
 *         int32_t Value = 1;
 *         int32_t ID    = 0;
 *     };
 *
 *     GAMECORE_DEFINE_SCRIPT_STRUCT(PickupStats, "PickupStats")   // in one .cpp
 *     {
 *         Struct.Field("Value", &PickupStats::Value);
 *         Struct.Field("ID",    &PickupStats::ID);
 *     }
 *
 * A struct crosses into Lua as its fields' values in order, as a Vector3 is
 * three numbers: `local value, id = pickup:GetStats()`, `pickup:SetStats(5, 2)`.
 * In a script graph it is a pin type of its own, with Make and Break nodes.
 *
 * Fields are any of the field types below except another struct. The struct
 * must be default-constructible and trivially copyable (a component may hold
 * one, and components are copied as bytes).
 *
 * LIFETIME, as everything else a game module declares: the registrar's
 * destructor unregisters the fields when the image unloads. The struct's id
 * is found by name and kept, so a reloaded module gets the same id and a
 * binding built before the reload still names the right struct.
 */

namespace GameCore
{
    enum class EScriptFieldType : uint8_t
    {
        Float, Double, Int32, UInt8, Bool, Flag, Vector3, AABB, Color, FixedString,
        Struct,   // a registered script struct; its id travels beside the type
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

    enum : size_t
    {
        SCRIPT_BINDING_NAME_MAX      = 40,   // including the NUL; accessors add Get/Set
        SCRIPT_BINDINGS_PER_TYPE_MAX = 32,
        SCRIPT_SIGNATURE_MAX         = 96,   // including the NUL
        SCRIPT_STRUCT_NAME_MAX       = 32,   // including the NUL; a graph pin type
        SCRIPT_STRUCT_FIELDS_MAX     = 16,
        SCRIPT_STRUCT_MAX            = 64,
    };

    using ScriptStructID = uint16_t;
    constexpr ScriptStructID SCRIPT_STRUCT_INVALID = 0xFFFF;

    struct ScriptStructField
    {
        char             Name[SCRIPT_BINDING_NAME_MAX] = {};
        EScriptFieldType Type   = EScriptFieldType::Float;
        uint32_t         Offset = 0;
        uint32_t         Size   = 0;
    };

    /* FROZEN LAYOUT, read directly by every image (ComponentType.h's rule). */
    struct ScriptStructInfo
    {
        char              Name[SCRIPT_STRUCT_NAME_MAX] = {};
        ScriptStructID    ID          = SCRIPT_STRUCT_INVALID;
        uint16_t          FieldCount  = 0;
        uint16_t          Lanes       = 0;       // Lua values: the fields' lanes, summed
        bool              bRegistered = false;   // fields present; false after an unload
        uint32_t          SizeBytes   = 0;
        ScriptStructField Fields[SCRIPT_STRUCT_FIELDS_MAX];
    };

    /* The struct's identity: InName's id, assigned the first time anything
     * asks and the same for the rest of the process. SCRIPT_STRUCT_INVALID if
     * the table is full or the name does not fit. */
    GAMECORE_API ScriptStructID FindOrAddScriptStruct(const char* InName);

    /* Called by the macro. False on a refused registration: a struct already
     * registered under that id, an unusable field, or InRejected > 0 (logged). */
    GAMECORE_API bool RegisterScriptStruct(ScriptStructID InID, uint32_t InSizeBytes, const ScriptStructField* InFields,
                                           size_t InCount, size_t InRejected);

    /* Withdraws the fields; the id and name stay. */
    GAMECORE_API void UnregisterScriptStruct(ScriptStructID InID);

    /* nullptr unless InID is registered now. */
    GAMECORE_API const ScriptStructInfo* GetScriptStruct(ScriptStructID InID);

    /* InID's name, registered or not (an id is never forgotten); "" if unknown. */
    GAMECORE_API const char* GetScriptStructName(ScriptStructID InID);

    /* Ids run 0 .. count-1; some may be unregistered. */
    GAMECORE_API size_t GetScriptStructCount();

    /* Lua values a field of this type takes: 3 for a Vector3, a struct's
     * Lanes, 0 for an unregistered struct. */
    GAMECORE_API int GetScriptFieldLanes(EScriptFieldType InType, ScriptStructID InStruct);

    /* True for a type declared with GAMECORE_SCRIPT_STRUCT_BODY(). */
    template<typename T, typename = void> struct IsScriptStruct : std::false_type {};
    template<typename T> struct IsScriptStruct<T, std::void_t<decltype(T::StaticScriptStruct())>> : std::true_type {};

    /* A field's type, and its struct when it is one. What the binders record. */
    struct ScriptFieldDesc
    {
        EScriptFieldType Type   = EScriptFieldType::Float;
        ScriptStructID   Struct = SCRIPT_STRUCT_INVALID;
    };

    template<typename TField>
    ScriptFieldDesc ScriptFieldDescOf()
    {
        if constexpr(IsScriptStruct<TField>::value) { return { EScriptFieldType::Struct, TField::StaticScriptStruct() }; }
        else                                        { return { ScriptFieldTypeOf<TField>::Value, SCRIPT_STRUCT_INVALID }; }
    }

    /* What `Struct` is inside GAMECORE_DEFINE_SCRIPT_STRUCT. Plain data;
     * registration happens at Commit(). */
    template<typename T>
    class ScriptStructBinder
    {
    public:
        template<typename TField>
        void Field(const char* InName, TField T::* InMember)
        {
            static_assert(!IsScriptStruct<TField>::value, "a script struct's fields cannot be script structs themselves");
            AddField(InName, InMember, ScriptFieldTypeOf<TField>::Value);
        }

        /* A uint8_t 0/1 flag, as a boolean. */
        void Flag(const char* InName, uint8_t T::* InMember) { AddField(InName, InMember, EScriptFieldType::Flag); }

        bool Commit() { return RegisterScriptStruct(T::StaticScriptStruct(), uint32_t(sizeof(T)), Fields, Count, Rejected); }

    private:
        /* Offsets measured on a default-constructed instance, as
         * ComponentScriptBinder does. Over-long names are rejected, not cut. */
        template<typename TField>
        void AddField(const char* InName, TField T::* InMember, const EScriptFieldType InType)
        {
            size_t Length = 0;
            while(InName != nullptr && InName[Length] != '\0') { ++Length; }
            if(Count >= SCRIPT_STRUCT_FIELDS_MAX || Length == 0 || Length >= SCRIPT_BINDING_NAME_MAX)
            {
                ++Rejected;
                return;
            }

            const T Probe{};
            ScriptStructField& Out = Fields[Count++];
            for(size_t Index = 0; Index < Length; ++Index) { Out.Name[Index] = InName[Index]; }
            Out.Type   = InType;
            Out.Offset = uint32_t(reinterpret_cast<const char*>(&(Probe.*InMember)) - reinterpret_cast<const char*>(&Probe));
            Out.Size   = uint32_t(sizeof(TField));
        }

        ScriptStructField Fields[SCRIPT_STRUCT_FIELDS_MAX];
        size_t            Count    = 0;
        size_t            Rejected = 0;
    };
}

/* Inside the struct: declares the id function the definition provides. */
#define GAMECORE_SCRIPT_STRUCT_BODY() \
    static ::GameCore::ScriptStructID StaticScriptStruct();

/*
 * One per struct, at file scope, in exactly one .cpp. NameLiteral is the type's
 * name in signatures and graphs. The body that follows has `Struct` (a
 * ScriptStructBinder<TypeName>&) in scope.
 */
#define GAMECORE_DEFINE_SCRIPT_STRUCT(TypeName, NameLiteral)                                          \
    static_assert(std::is_trivially_copyable<TypeName>::value, "a script struct must be trivially copyable"); \
    static_assert(std::is_default_constructible<TypeName>::value, "a script struct must be default-constructible"); \
    ::GameCore::ScriptStructID TypeName::StaticScriptStruct()                                          \
    {                                                                                                  \
        static const ::GameCore::ScriptStructID ID = ::GameCore::FindOrAddScriptStruct(NameLiteral);   \
        return ID;                                                                                     \
    }                                                                                                  \
    static void TypeName##_ScriptStructBody(::GameCore::ScriptStructBinder<TypeName>& Struct);         \
    namespace                                                                                          \
    {                                                                                                  \
        struct TypeName##_ScriptStructRegistrar                                                        \
        {                                                                                              \
            TypeName##_ScriptStructRegistrar()                                                         \
            {                                                                                          \
                ::GameCore::ScriptStructBinder<TypeName> Struct;                                       \
                TypeName##_ScriptStructBody(Struct);                                                   \
                bRegistered = Struct.Commit();                                                         \
            }                                                                                          \
            ~TypeName##_ScriptStructRegistrar()                                                        \
            {                                                                                          \
                if(bRegistered) { ::GameCore::UnregisterScriptStruct(TypeName::StaticScriptStruct()); } \
            }                                                                                          \
            bool bRegistered = false;                                                                  \
        };                                                                                             \
        TypeName##_ScriptStructRegistrar G##TypeName##_ScriptStructRegistrar;                          \
    }                                                                                                  \
    static void TypeName##_ScriptStructBody(::GameCore::ScriptStructBinder<TypeName>& Struct)

#endif //GAMECORE_SCRIPTSTRUCT_H
