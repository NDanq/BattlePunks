#include "Private/NetCore/NetReplication.h"

#include <cstring>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/Math/Vector.h"

namespace NetCore
{
namespace
{
    constexpr uint32_t TAG_DATA = EngineCore::MakeArchiveTag('D', 'A', 'T', 'A');

    template<typename T>
    T& At(void* InComponent, const uint32_t InOffset) { return *reinterpret_cast<T*>(static_cast<char*>(InComponent) + InOffset); }

    template<typename T>
    T Copy(const void* InComponent, const uint32_t InOffset)
    {
        T Value;
        std::memcpy(&Value, static_cast<const char*>(InComponent) + InOffset, sizeof(T));
        return Value;
    }

    void SerializeVector(EngineCore::Archive& Ar, EngineCore::Vector3& InOut)
    {
        float X = float(InOut.GetX()), Y = float(InOut.GetY()), Z = float(InOut.GetZ());
        Ar << X << Y << Z;
        if(Ar.IsLoading() && !Ar.HasError())
        {
            InOut.SetX(X);
            InOut.SetY(Y);
            InOut.SetZ(Z);
        }
    }

    /* One value of InType at InField, either direction. Writing works on a
     * copy, so the component itself is never touched. */
    void SerializeValue(EngineCore::Archive& Ar, const GameCore::EScriptFieldType InType, void* InField, const uint32_t InSize)
    {
        using GameCore::EScriptFieldType;
        switch(InType)
        {
            case EScriptFieldType::Float:   Ar << *static_cast<float*>(InField);   break;
            case EScriptFieldType::Double:  Ar << *static_cast<double*>(InField);  break;
            case EScriptFieldType::Int32:   Ar << *static_cast<int32_t*>(InField); break;
            case EScriptFieldType::UInt8:
            case EScriptFieldType::Flag:    Ar << *static_cast<uint8_t*>(InField); break;
            case EScriptFieldType::Bool:    Ar << *static_cast<bool*>(InField);    break;
            case EScriptFieldType::Vector3: SerializeVector(Ar, *static_cast<EngineCore::Vector3*>(InField)); break;
            case EScriptFieldType::AABB:
            {
                GameCore::AABB& Box = *static_cast<GameCore::AABB*>(InField);
                SerializeVector(Ar, Box.Min);
                SerializeVector(Ar, Box.Max);
                break;
            }
            case EScriptFieldType::Color:
            {
                EngineCore::Color& Value = *static_cast<EngineCore::Color*>(InField);
                Ar << Value.R << Value.G << Value.B << Value.A;
                break;
            }
            case EScriptFieldType::FixedString: Ar.SerializeFixedString(static_cast<char*>(InField), InSize); break;
            case EScriptFieldType::Struct:
            default:
                Ar.SetError("a replicated field of an unsupported type");
                break;
        }
    }

    /* The serializer's EntityID fields as NetIds. */
    class NetEntityRemapper final : public GameCore::IEntityRemapper
    {
    public:
        explicit NetEntityRemapper(const INetEntityMap& InMap) : Map(InMap) {}

        void RemapEntity(EngineCore::Archive& Ar, GameCore::EntityID& InOut) override
        {
            int32_t NetId = Ar.IsLoading() ? 0 : Map.ToNetId(InOut);
            Ar << NetId;
            if(Ar.IsLoading()) { InOut = Map.FromNetId(NetId); }
        }

    private:
        const INetEntityMap& Map;
    };
}

void WriteReplicatedField(EngineCore::Archive& Ar, const GameCore::ReplicatedFieldEntry& InField,
                          const void* InComponent, const INetEntityMap& InMap)
{
    if(InField.Kind == GameCore::EReplicatedKind::Entity)
    {
        int32_t NetId = InMap.ToNetId(Copy<GameCore::EntityID>(InComponent, InField.Offset));
        Ar << NetId;
        return;
    }

    /* Through a copy of the field's bytes: SerializeValue takes a reference. */
    alignas(16) char Scratch[256];
    if(InField.Size > sizeof(Scratch)) { Ar.SetError("a replicated field larger than 256 bytes"); return; }
    std::memcpy(Scratch, static_cast<const char*>(InComponent) + InField.Offset, InField.Size);
    SerializeValue(Ar, InField.FieldType, Scratch, InField.Size);
}

void ReadReplicatedField(EngineCore::Archive& Ar, const GameCore::ReplicatedFieldEntry& InField,
                         void* InOutComponent, const INetEntityMap& InMap)
{
    if(InField.Kind == GameCore::EReplicatedKind::Entity)
    {
        int32_t NetId = 0;
        Ar << NetId;
        if(!Ar.HasError()) { At<GameCore::EntityID>(InOutComponent, InField.Offset) = InMap.FromNetId(NetId); }
        return;
    }

    /* Into a copy first, so a truncated packet never leaves half a value. */
    alignas(16) char Scratch[256];
    if(InField.Size > sizeof(Scratch)) { Ar.SetError("a replicated field larger than 256 bytes"); return; }
    std::memcpy(Scratch, static_cast<const char*>(InOutComponent) + InField.Offset, InField.Size);
    SerializeValue(Ar, InField.FieldType, Scratch, InField.Size);
    if(!Ar.HasError()) { std::memcpy(static_cast<char*>(InOutComponent) + InField.Offset, Scratch, InField.Size); }
}

bool ReplicatedFieldDiffers(const GameCore::ReplicatedFieldEntry& InField, const void* InA, const void* InB)
{
    /* A Vector3 is compared by its three components: under WITH_SIMD its
     * fourth lane is padding, and padding is not a change. */
    if(InField.Kind == GameCore::EReplicatedKind::Value && InField.FieldType == GameCore::EScriptFieldType::Vector3)
    {
        const EngineCore::Vector3 A = Copy<EngineCore::Vector3>(InA, InField.Offset);
        const EngineCore::Vector3 B = Copy<EngineCore::Vector3>(InB, InField.Offset);
        return A.GetX() != B.GetX() || A.GetY() != B.GetY() || A.GetZ() != B.GetZ();
    }
    return std::memcmp(static_cast<const char*>(InA) + InField.Offset,
                       static_cast<const char*>(InB) + InField.Offset, InField.Size) != 0;
}

bool WriteComponentData(EngineCore::Archive& Ar, const GameCore::ComponentTypeID InType, void* InComponent,
                        const INetEntityMap& InMap)
{
    const GameCore::ComponentSerializerInfo* Serializer = GameCore::FindComponentSerializer(InType);
    if(!Serializer) { return false; }

    uint16_t Version = Serializer->Version;
    Ar.OpenChunk(TAG_DATA, Version);
    NetEntityRemapper Remap(InMap);
    GameCore::BinaryComponentArchive ComponentAr(Ar, Remap);
    Serializer->Fn(ComponentAr, InComponent);
    Ar.CloseChunk();
    return !Ar.HasError();
}

bool ReadComponentData(EngineCore::Archive& Ar, const GameCore::ComponentTypeID InType, void* InOutComponent,
                       const INetEntityMap& InMap)
{
    const GameCore::ComponentSerializerInfo* Serializer = GameCore::FindComponentSerializer(InType);
    if(!Serializer) { return false; }

    uint16_t Version = 0;
    if(!Ar.OpenChunk(TAG_DATA, Version)) { return false; }
    NetEntityRemapper Remap(InMap);
    GameCore::BinaryComponentArchive ComponentAr(Ar, Remap);
    Serializer->Fn(ComponentAr, InOutComponent);
    Ar.CloseChunk();
    return !Ar.HasError();
}
}
