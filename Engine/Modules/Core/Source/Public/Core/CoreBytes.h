#ifndef ENGINECORE_COREBYTES_H
#define ENGINECORE_COREBYTES_H

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "Public/Core/CoreAPI.h"
#include "Public/Core/CoreMemoryPool.h"

/*
 * PooledBytes: a byte buffer that allocates from a memory pool
 * (CoreMemoryPool.h) through Memory::Allocate, where std::vector<uint8_t>
 * would use the CRT heap. What Image's pixels and RenderCore's texture blocks
 * are kept in, so a subsystem can put its large buffers in a pool of its own
 * -- for reuse, and for an exact count of what they cost.
 *
 * The vector subset its callers use: size, data, resize (new bytes zeroed),
 * assign, clear, indexing, equality. A copy allocates from the source's
 * pool; a move takes the block. An allocation the pool refuses falls back to
 * POOL_Default, so a buffer is never left short.
 *
 * Out-of-line, so every allocation and free happens in EngineCore (the rule
 * at the top of CoreMemory.h). The bytes carry no destructor and are tagged
 * IMAGE_Host.
 */
namespace EngineCore
{
    class CORE_API PooledBytes
    {
    public:
        PooledBytes() = default;
        explicit PooledBytes(PoolID InPool) : Pool(InPool) {}
        PooledBytes(std::initializer_list<uint8_t> InBytes);
        ~PooledBytes();

        PooledBytes(const PooledBytes& InOther);
        PooledBytes& operator=(const PooledBytes& InOther);
        PooledBytes(PooledBytes&& InOther) noexcept;
        PooledBytes& operator=(PooledBytes&& InOther) noexcept;
        PooledBytes& operator=(std::initializer_list<uint8_t> InBytes);

        size_t         size()  const { return Size; }
        bool           empty() const { return Size == 0; }
        uint8_t*       data()        { return Data; }
        const uint8_t* data()  const { return Data; }
        uint8_t*       begin()       { return Data; }
        uint8_t*       end()         { return Data + Size; }
        const uint8_t* begin() const { return Data; }
        const uint8_t* end()   const { return Data + Size; }
        uint8_t&       operator[](size_t InIndex)       { return Data[InIndex]; }
        const uint8_t& operator[](size_t InIndex) const { return Data[InIndex]; }

        /* Keeps the first min(old, new) bytes; new ones are zero. */
        void resize(size_t InCount);
        void assign(const uint8_t* InFirst, const uint8_t* InLast);
        void assign(size_t InCount, uint8_t InValue);
        void pop_back() { if(Size > 0) { --Size; } }
        /* Frees the block. */
        void clear();

        bool operator==(const PooledBytes& InOther) const;
        bool operator!=(const PooledBytes& InOther) const { return !(*this == InOther); }

        /* The pool the next allocation comes from; a buffer holding bytes
         * moves them into it. */
        PoolID GetPool() const { return Pool; }
        void   SetPool(PoolID InPool);

    private:
        /* Exactly InCount bytes from Pool (else POOL_Default); the old block's
         * first InKeep bytes copied over, the rest zeroed. */
        void Reallocate(size_t InCount, size_t InKeep);

        uint8_t* Data = nullptr;
        size_t   Size = 0;
        size_t   Capacity = 0;
        PoolID   Pool = POOL_Default;
    };
}

#endif //ENGINECORE_COREBYTES_H
