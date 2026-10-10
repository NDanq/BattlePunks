#include "Public/Core/CoreBytes.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "Public/Core/CoreMemory.h"

namespace EngineCore
{
    namespace
    {
        constexpr size_t BYTES_ALIGN = 16;   // what SIMD over pixels wants

        uint8_t* AllocateBytes(const size_t InCount, PoolID& InOutPool)
        {
            void* Block = Memory::Allocate(InCount, BYTES_ALIGN, InOutPool, nullptr, 0, IMAGE_Host);
            if(!Block && InOutPool != POOL_Default)
            {
                InOutPool = POOL_Default;   // refused: the CRT, rather than a short buffer
                Block = Memory::Allocate(InCount, BYTES_ALIGN, POOL_Default, nullptr, 0, IMAGE_Host);
            }
            return static_cast<uint8_t*>(Block);
        }
    }

    PooledBytes::PooledBytes(const std::initializer_list<uint8_t> InBytes)
    {
        assign(InBytes.begin(), InBytes.end());
    }

    PooledBytes::~PooledBytes()
    {
        clear();
    }

    PooledBytes::PooledBytes(const PooledBytes& InOther) : Pool(InOther.Pool)
    {
        assign(InOther.begin(), InOther.end());
    }

    PooledBytes& PooledBytes::operator=(const PooledBytes& InOther)
    {
        if(this != &InOther)
        {
            if(Pool != InOther.Pool) { clear(); Pool = InOther.Pool; }
            assign(InOther.begin(), InOther.end());
        }
        return *this;
    }

    PooledBytes::PooledBytes(PooledBytes&& InOther) noexcept
        : Data(InOther.Data), Size(InOther.Size), Capacity(InOther.Capacity), Pool(InOther.Pool)
    {
        InOther.Data = nullptr;
        InOther.Size = InOther.Capacity = 0;
    }

    PooledBytes& PooledBytes::operator=(PooledBytes&& InOther) noexcept
    {
        if(this != &InOther)
        {
            clear();
            Data = InOther.Data; Size = InOther.Size; Capacity = InOther.Capacity; Pool = InOther.Pool;
            InOther.Data = nullptr;
            InOther.Size = InOther.Capacity = 0;
        }
        return *this;
    }

    PooledBytes& PooledBytes::operator=(const std::initializer_list<uint8_t> InBytes)
    {
        assign(InBytes.begin(), InBytes.end());
        return *this;
    }

    void PooledBytes::Reallocate(const size_t InCount, const size_t InKeep)
    {
        uint8_t* Block = InCount > 0 ? AllocateBytes(InCount, Pool) : nullptr;
        if(InCount > 0 && !Block)
        {
            return;   // logged by Memory::Allocate; the buffer is left as it was
        }
        const size_t Keep = (std::min)(InKeep, InCount);
        if(Keep > 0)            { std::memcpy(Block, Data, Keep); }
        if(InCount > Keep)      { std::memset(Block + Keep, 0, InCount - Keep); }
        if(Data)                { Memory::Free(Data); }
        Data     = Block;
        Size     = InCount;
        Capacity = InCount;
    }

    void PooledBytes::resize(const size_t InCount)
    {
        if(InCount <= Capacity)
        {
            if(InCount > Size) { std::memset(Data + Size, 0, InCount - Size); }
            Size = InCount;
            return;
        }
        Reallocate(InCount, Size);
    }

    void PooledBytes::assign(const uint8_t* InFirst, const uint8_t* InLast)
    {
        const size_t Count = InLast > InFirst ? size_t(InLast - InFirst) : 0;
        if(Count > Capacity) { Reallocate(Count, 0); }
        if(Count > Capacity) { return; }   // the allocation failed
        if(Count > 0) { std::memmove(Data, InFirst, Count); }
        Size = Count;
    }

    void PooledBytes::assign(const size_t InCount, const uint8_t InValue)
    {
        if(InCount > Capacity) { Reallocate(InCount, 0); }
        if(InCount > Capacity) { return; }
        if(InCount > 0) { std::memset(Data, InValue, InCount); }
        Size = InCount;
    }

    void PooledBytes::clear()
    {
        if(Data) { Memory::Free(Data); }
        Data = nullptr;
        Size = Capacity = 0;
    }

    bool PooledBytes::operator==(const PooledBytes& InOther) const
    {
        return Size == InOther.Size && (Size == 0 || std::memcmp(Data, InOther.Data, Size) == 0);
    }

    void PooledBytes::SetPool(const PoolID InPool)
    {
        if(InPool == Pool) { return; }
        Pool = InPool;
        if(Data) { Reallocate(Size, Size); }
    }
}
