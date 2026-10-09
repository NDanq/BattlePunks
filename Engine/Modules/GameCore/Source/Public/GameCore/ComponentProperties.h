#ifndef GAMECORE_COMPONENTPROPERTIES_H
#define GAMECORE_COMPONENTPROPERTIES_H

#include <cstddef>
#include <cstdint>
#include <cstring>     // std::memcpy

#include "Public/GameCore/ComponentScript.h"   // EScriptFieldType / ScriptFieldTypeOf
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/GameCoreAPI.h"

/* EDITOR ONLY. Wrap every GAMECORE_DEFINE_COMPONENT_PROPERTIES in
 * `#if WITH_EDITOR`; a forgotten wrapper fails to compile in a game build. */
#if WITH_EDITOR

/*
 * Editor-visible properties: the fields a tool may show and edit. Opt-in,
 * and deliberately separate from the Lua bindings (ComponentScript.h).
 *
 *     GAMECORE_DEFINE_COMPONENT_PROPERTIES(RenderComponent)
 *     {
 *         Props.Flag("Visible", &RenderComponent::bVisible);
 *         Props.Array("Materials", &RenderComponent::Materials, PF_AssetMaterial);
 *         Props.Field("World", &BoundsComponent::World, PF_ReadOnly);
 *     }
 *
 * CATEGORIES. Props.Category("Name") files every property declared after it
 * under Name, until the next Category; Category(nullptr) or "" ends it. A
 * tool groups a category's properties (the Details panel: a titled box,
 * after the uncategorised ones, in order of first use).
 *
 *         Props.Flag("Enabled", &CollisionComponent::bEnabled);
 *         Props.Category("Filtering");
 *         Props.Field("Layer", &CollisionComponent::Layer);
 *
 * STRUCT ARRAYS. Props.StructArray lists an array of small structs, each
 * element shown as a group of its members; the members are entries of their
 * own, after the array's, with Parent naming it:
 *
 *         Props.StructArray("Parameters", &RenderComponent::Parameters, [](auto& Member)
 *         {
 *             Member.Field("Slot", &RenderMaterialParameter::Slot);
 *             Member.Field("Name", &RenderMaterialParameter::Name);
 *             Member.Element("X", &RenderMaterialParameter::Value, 0);
 *         });
 *
 * Unlisted fields (per-run state above all) are invisible. Entries are plain
 * data (name, type, offset, size, flags), so a tool needs no C++ type.
 * Registered at static init; lookups are game thread only.
 */

namespace GameCore
{
    /* Same field types as the Lua bindings. */
    using EPropertyType = EScriptFieldType;

    /* 16 bits: the asset hints fill bits 1..11. */
    enum EPropertyFlags : uint16_t
    {
        PF_None     = 0,
        PF_ReadOnly = 1 << 0,   // shown, never written by a tool

        /* Asset hints on a string field, for a picker / drag-drop. */
        PF_AssetMesh     = 1 << 1,   // a runtime-relative path to a mesh asset
        PF_AssetMaterial = 1 << 2,   // a material's name
        PF_AssetPostProcess = 1 << 3,   // a post process stack (*.pp.nmgraph), or a PostProcess material graph
        PF_AssetRenderTarget = 1 << 4,  // a render target texture (*.nmrt2d, *.nmrt3d; RenderTargetAsset.h)
        PF_AssetTexture  = 1 << 5,   // a texture asset (*.nmtexture), e.g. a Sky Light's HDRI
        PF_AssetParticleSystem = 1 << 6,   // a particle system (*.nmparticle; RenderCore's ParticleSystemAsset.h)
        PF_AssetAnimation = 1 << 7,   // an animation clip (*.nmanim) or anim state (*.nmanimstate; RenderCore's ANIMATION.md)
        PF_AssetSound    = 1 << 8,   // a sound (*.nmsound; the Audio module's SoundAsset.h)
        PF_AssetPrefab   = 1 << 9,   // a prefab (*.nmprefab, or a native one's "native:Name"; PREFABS.md)
        PF_AssetSequence = 1 << 10,  // a sequence (*.nmseq; Engine/Modules/Sequencer)
        PF_AssetTerrain  = 1 << 11,  // a terrain heightfield (*.nmterrain; GameCore's Terrain.h)
    };

    enum : size_t
    {
        COMPONENT_PROPERTY_NAME_MAX       = 40,   // including the NUL
        COMPONENT_PROPERTIES_PER_TYPE_MAX = 32,
        COMPONENT_PROPERTY_CHOICES_MAX    = 96,   // including the NUL
        COMPONENT_PROPERTY_CATEGORY_MAX   = 32,   // including the NUL
    };

    /* ComponentPropertyEntry::Parent of an entry that is not a struct member. */
    constexpr uint8_t PROPERTY_NO_PARENT = 0xFF;

    struct ComponentPropertyEntry
    {
        char          Name[COMPONENT_PROPERTY_NAME_MAX] = {};
        EPropertyType Type   = EPropertyType::Float;
        uint16_t      Flags  = PF_None;
        uint32_t      Offset = 0;   // from the start of the component, Owner header included
        uint32_t      Size   = 0;   // sizeof the member; a FixedString's buffer size including the NUL

        /* An array member (Props.Array): ArrayCount elements of Size bytes,
         * Stride apart, the first at Offset. 0: a single value. A tool shows
         * it as a list; a trailing run of default elements is "not in use". */
        uint16_t      ArrayCount = 0;
        uint32_t      Stride     = 0;

        /* A CHOICE (Props.Choice): an integer field whose values 0, 1, 2...
         * are these labels, '|'-separated -- a tool shows a dropdown. Empty:
         * not a choice. */
        char          Choices[COMPONENT_PROPERTY_CHOICES_MAX] = {};

        /* The group a tool shows it in (Props.Category). Empty: none. */
        char          Category[COMPONENT_PROPERTY_CATEGORY_MAX] = {};

        /* A STRUCT ARRAY (Props.StructArray) is an array entry of Type
         * Struct, Size the element's size; its members follow it, each with
         * Parent its index in the type's list and Offset from the start of
         * an element. A tool reaches a member of element i with
         * GetMember(i, Member). PROPERTY_NO_PARENT: not a member. */
        uint8_t       Parent = PROPERTY_NO_PARENT;

        bool IsChoice() const    { return Choices[0] != '\0'; }
        bool HasCategory() const { return Category[0] != '\0'; }

        bool IsReadOnly() const { return (Flags & PF_ReadOnly) != 0; }
        bool IsArray() const    { return ArrayCount != 0; }
        bool IsMember() const   { return Parent != PROPERTY_NO_PARENT; }
        bool IsStructArray() const { return IsArray() && Type == EPropertyType::Struct; }

        /* Every byte the entry covers: the whole array, or the one value. */
        uint32_t GetSpan() const { return IsArray() ? Stride * ArrayCount : Size; }

        /* Element InIndex as a single value: the same name, type and flags. */
        ComponentPropertyEntry GetElement(const size_t InIndex) const
        {
            ComponentPropertyEntry Element = *this;
            Element.Offset     = Offset + uint32_t(InIndex) * Stride;
            Element.ArrayCount = 0;
            Element.Stride     = 0;
            return Element;
        }

        /* On a struct array: InMember (one of its members) in element
         * InIndex, as a single value at its place in the component. */
        ComponentPropertyEntry GetMember(const size_t InIndex, const ComponentPropertyEntry& InMember) const
        {
            ComponentPropertyEntry Member = InMember;
            Member.Offset = Offset + uint32_t(InIndex) * Stride + InMember.Offset;
            Member.Parent = PROPERTY_NO_PARENT;
            if(IsReadOnly()) { Member.Flags |= PF_ReadOnly; }
            return Member;
        }
    };

    /* Writes a default-constructed instance into OutBytes (SizeBytes long). */
    using ComponentPropertyDefaultsFn = void (*)(void* OutBytes);

    enum EComponentPropertyTypeFlags : uint8_t
    {
        CPF_None         = 0,
        /* Hidden from "Add Component" (e.g. added and owned by a system). */
        CPF_NotAddable   = 1 << 0,
    };

    /* Called by the macro. InRejected (too many, or over-long names) is logged.
     * False on a duplicate (first kept) or a full table. */
    GAMECORE_API bool RegisterComponentProperties(ComponentTypeID InType, const ComponentPropertyEntry* InEntries,
                                                  size_t InCount, size_t InRejected,
                                                  ComponentPropertyDefaultsFn InDefaultsFn, uint8_t InTypeFlags);

    /* Withdraws InType's properties; the macro's registrar calls it when its
     * image unloads, as the serializer and script registrars do -- so a game
     * module's defaults function never outlives its code, and a reloaded
     * module registers again (into the same slots). */
    GAMECORE_API void UnregisterComponentProperties(ComponentTypeID InType);

    /* nullptr/0 if InType has no editor-visible properties. */
    GAMECORE_API const ComponentPropertyEntry* FindComponentProperties(ComponentTypeID InType, size_t& OutCount);

    /* InType's default-constructed bytes (SizeBytes long), for "changed"
     * markers, reset and Add Component. False if InType has no property list.
     * Never zero-fill instead: zero is not the default (see ProxyID). */
    GAMECORE_API bool GetComponentPropertyDefaults(ComponentTypeID InType, void* OutBytes);

    /* True if InType registered a property list without CPF_NotAddable. */
    GAMECORE_API bool IsComponentAddable(ComponentTypeID InType);

    template<typename T> class ComponentPropertyBinder;

    /* `Member` inside Props.StructArray's body: TElement's members, with
     * offsets from the start of an element. */
    template<typename TOwner, typename TElement>
    class ComponentStructBinder
    {
    public:
        ComponentStructBinder(ComponentPropertyBinder<TOwner>& InOwner, const uint8_t InParent)
            : Owner(InOwner), Parent(InParent) {}

        template<typename TField>
        void Field(const char* InName, TField TElement::* InMember, const uint16_t InFlags = PF_None)
        {
            const TElement Probe{};
            AddMember(InName, Probe, &(Probe.*InMember), uint32_t(sizeof(TField)), ScriptFieldTypeOf<TField>::Value, InFlags);
        }

        void Flag(const char* InName, uint8_t TElement::* InMember, const uint16_t InFlags = PF_None)
        {
            const TElement Probe{};
            AddMember(InName, Probe, &(Probe.*InMember), uint32_t(sizeof(uint8_t)), EPropertyType::Flag, InFlags);
        }

        /* One element of an array member of the struct, as its own field. */
        template<typename TPart, size_t N>
        void Element(const char* InName, TPart (TElement::* InArray)[N], const size_t InIndex, const uint16_t InFlags = PF_None)
        {
            if(InIndex >= N)
            {
                Owner.Reject();
                return;
            }
            const TElement Probe{};
            AddMember(InName, Probe, &((Probe.*InArray)[InIndex]), uint32_t(sizeof(TPart)), ScriptFieldTypeOf<TPart>::Value, InFlags);
        }

    private:
        void AddMember(const char* InName, const TElement& InProbe, const void* InField, const uint32_t InSize,
                       const EPropertyType InType, const uint16_t InFlags)
        {
            const uint32_t Offset = uint32_t(reinterpret_cast<const char*>(InField) - reinterpret_cast<const char*>(&InProbe));
            if(ComponentPropertyEntry* Entry = Owner.AddAt(InName, Offset, InSize, InType, InFlags))
            {
                Entry->Parent = Parent;
            }
        }

        ComponentPropertyBinder<TOwner>& Owner;
        uint8_t                          Parent;
    };

    /* `Props` inside GAMECORE_DEFINE_COMPONENT_PROPERTIES; registers at Commit(). */
    template<typename T>
    class ComponentPropertyBinder
    {
    public:
        template<typename TField>
        void Field(const char* InName, TField T::* InMember, const uint16_t InFlags = PF_None)
        {
            Add(InName, InMember, ScriptFieldTypeOf<TField>::Value, InFlags);
        }

        /*
         * A uint8_t or int32_t field shown as a dropdown: InLabels names its
         * values 0, 1, 2..., '|'-separated -- an enum to a tool.
         *
         *     Props.Choice("Source", &CollisionComponent::Source, "Bounds component|Render component mesh");
         */
        template<typename TField>
        void Choice(const char* InName, TField T::* InMember, const char* InLabels, const uint16_t InFlags = PF_None)
        {
            static_assert(std::is_same<TField, uint8_t>::value || std::is_same<TField, int32_t>::value,
                          "a choice is a uint8_t or int32_t field");
            const T Probe{};
            const char* Base   = reinterpret_cast<const char*>(&Probe);
            const char* Member = reinterpret_cast<const char*>(&(Probe.*InMember));
            if(ComponentPropertyEntry* Entry = AddAt(InName, uint32_t(Member - Base), uint32_t(sizeof(TField)),
                                                     ScriptFieldTypeOf<TField>::Value, InFlags))
            {
                size_t Length = 0;
                while(InLabels && InLabels[Length] != '\0' && Length + 1 < COMPONENT_PROPERTY_CHOICES_MAX)
                {
                    Entry->Choices[Length] = InLabels[Length];
                    ++Length;
                }
            }
        }

        /* A uint8_t 0/1 flag, shown as a checkbox rather than a number. */
        void Flag(const char* InName, uint8_t T::* InMember, const uint16_t InFlags = PF_None)
        {
            Add(InName, InMember, EPropertyType::Flag, InFlags);
        }

        /* One element of an array member, as its own field: a list of slots
         * (RenderComponent's materials) without an array property type. */
        template<typename TElement, size_t N>
        void Element(const char* InName, TElement (T::* InArray)[N], const size_t InIndex, const uint16_t InFlags = PF_None)
        {
            if(InIndex >= N)
            {
                ++Rejected;
                return;
            }
            const T Probe{};
            const char* Base   = reinterpret_cast<const char*>(&Probe);
            const char* Member = reinterpret_cast<const char*>(&((Probe.*InArray)[InIndex]));
            AddAt(InName, uint32_t(Member - Base), uint32_t(sizeof(TElement)), ScriptFieldTypeOf<TElement>::Value, InFlags);
        }

        /* A whole array member as one property, which a tool shows as a list
         * whose entries can be added and removed (ComponentPropertyEntry). */
        template<typename TElement, size_t N>
        void Array(const char* InName, TElement (T::* InArray)[N], const uint16_t InFlags = PF_None)
        {
            static_assert(N > 0 && N <= 0xFFFF, "an array property holds 1 to 65535 elements");
            const T Probe{};
            const char* Base   = reinterpret_cast<const char*>(&Probe);
            const char* Member = reinterpret_cast<const char*>(&((Probe.*InArray)[0]));
            if(ComponentPropertyEntry* Entry = AddAt(InName, uint32_t(Member - Base), uint32_t(sizeof(TElement)),
                                                     ScriptFieldTypeOf<TElement>::Value, InFlags))
            {
                Entry->ArrayCount = uint16_t(N);
                Entry->Stride     = uint32_t(sizeof(TElement));
            }
        }

        /*
         * An array of structs as one property, each element a group of the
         * members InBody declares (ComponentStructBinder). A tool shows it as
         * a list, like Array; an element is in use while any member differs
         * from the default.
         */
        template<typename TElement, size_t N, typename FBody>
        void StructArray(const char* InName, TElement (T::* InArray)[N], FBody&& InBody, const uint16_t InFlags = PF_None)
        {
            static_assert(N > 0 && N <= 0xFFFF, "an array property holds 1 to 65535 elements");
            const T Probe{};
            const char* Base   = reinterpret_cast<const char*>(&Probe);
            const char* Member = reinterpret_cast<const char*>(&((Probe.*InArray)[0]));
            const size_t Head  = Count;
            ComponentPropertyEntry* Entry = AddAt(InName, uint32_t(Member - Base), uint32_t(sizeof(TElement)),
                                                  EPropertyType::Struct, InFlags);
            if(!Entry)
            {
                return;
            }
            Entry->ArrayCount = uint16_t(N);
            Entry->Stride     = uint32_t(sizeof(TElement));

            ComponentStructBinder<T, TElement> Members(*this, uint8_t(Head));
            InBody(Members);
        }

        /* Files the properties declared after this under InName; nullptr or
         * "" ends the category. An over-long name is rejected (logged at
         * Commit), and the properties after it have none. */
        void Category(const char* InName)
        {
            size_t Length = 0;
            while(InName != nullptr && InName[Length] != '\0') { ++Length; }

            for(char& Each : CurrentCategory) { Each = '\0'; }
            if(Length >= COMPONENT_PROPERTY_CATEGORY_MAX)
            {
                ++Rejected;
                return;
            }
            for(size_t Index = 0; Index < Length; ++Index) { CurrentCategory[Index] = InName[Index]; }
        }

        /* Keeps the type out of a tool's "Add Component" (CPF_NotAddable). */
        void NotAddable() { TypeFlags |= CPF_NotAddable; }

        bool Commit() { return RegisterComponentProperties(T::StaticType().ID, Entries, Count, Rejected, &Defaults, TypeFlags); }

    private:
        template<typename, typename> friend class ComponentStructBinder;

        void Reject() { ++Rejected; }

        /* memcpy is sound: GAMECORE_DEFINE_COMPONENT asserts trivial copyability. */
        static void Defaults(void* OutBytes)
        {
            const T Fresh{};
            std::memcpy(OutBytes, &Fresh, sizeof(T));
        }

        template<typename TField>
        void Add(const char* InName, TField T::* InMember, const EPropertyType InType, const uint16_t InFlags)
        {
            /* Measured on a default-constructed instance. */
            const T Probe{};
            const char* Base   = reinterpret_cast<const char*>(&Probe);
            const char* Member = reinterpret_cast<const char*>(&(Probe.*InMember));
            AddAt(InName, uint32_t(Member - Base), uint32_t(sizeof(TField)), InType, InFlags);
        }

        ComponentPropertyEntry* AddAt(const char* InName, const uint32_t InOffset, const uint32_t InSize,
                                      const EPropertyType InType, const uint16_t InFlags)
        {
            size_t Length = 0;
            while(InName != nullptr && InName[Length] != '\0') { ++Length; }

            if(Count >= COMPONENT_PROPERTIES_PER_TYPE_MAX || Length == 0 || Length >= COMPONENT_PROPERTY_NAME_MAX)
            {
                ++Rejected;
                return nullptr;
            }

            ComponentPropertyEntry& Entry = Entries[Count++];
            for(size_t Index = 0; Index < Length; ++Index) { Entry.Name[Index] = InName[Index]; }

            Entry.Type   = InType;
            Entry.Flags  = InFlags;
            Entry.Offset = InOffset;
            Entry.Size   = InSize;
            std::memcpy(Entry.Category, CurrentCategory, sizeof(CurrentCategory));
            return &Entry;
        }

        ComponentPropertyEntry Entries[COMPONENT_PROPERTIES_PER_TYPE_MAX];
        char                   CurrentCategory[COMPONENT_PROPERTY_CATEGORY_MAX] = {};
        size_t                 Count     = 0;
        size_t                 Rejected  = 0;
        uint8_t                TypeFlags = CPF_None;
    };
}

/* Once per type, at file scope in one .cpp. The body has `Props` in scope. */
#define GAMECORE_DEFINE_COMPONENT_PROPERTIES(TypeName)                                               \
    static void TypeName##_PropertiesBody(::GameCore::ComponentPropertyBinder<TypeName>& Props);     \
    namespace                                                                                         \
    {                                                                                                 \
        struct TypeName##_PropertiesRegistrar                                                         \
        {                                                                                             \
            TypeName##_PropertiesRegistrar()                                                          \
            {                                                                                         \
                ::GameCore::ComponentPropertyBinder<TypeName> Props;                                  \
                TypeName##_PropertiesBody(Props);                                                     \
                bRegistered = Props.Commit();                                                         \
            }                                                                                         \
            /* Only its own registration: a refused duplicate leaves the first. */                    \
            ~TypeName##_PropertiesRegistrar()                                                         \
            {                                                                                         \
                if(bRegistered) { ::GameCore::UnregisterComponentProperties(TypeName::StaticType().ID); } \
            }                                                                                         \
            bool bRegistered = false;                                                                 \
        };                                                                                            \
        TypeName##_PropertiesRegistrar G##TypeName##_PropertiesRegistrar;                             \
    }                                                                                                 \
    static void TypeName##_PropertiesBody(::GameCore::ComponentPropertyBinder<TypeName>& Props)

#endif // WITH_EDITOR

#endif //GAMECORE_COMPONENTPROPERTIES_H
