#ifndef CORECLIENT_GDIBUFFER_H
#define CORECLIENT_GDIBUFFER_H

#include <cstdint>
#include <string>

/*
 * A GPU buffer the CPU fills once and shaders then own (GDICreateBuffer):
 * read and written by compute shaders (GDIDispatchCompute), and read by
 * instanced draws as per-instance vertex data (GDISetInstanceBuffer).
 *
 * Only on a backend with GDISupportsCompute(). Created on the render
 * thread; Release defers the free until the GPU is done with it, as for
 * textures, so it may follow the last draw that read it.
 */
enum EGDIBufferUsage : uint8_t
{
    GDI_BU_Storage  = 1 << 0,   // compute shaders read and write it
    GDI_BU_Instance = 1 << 1,   // instanced draws read it, GDI_INSTANCE_STRIDE bytes an instance
    GDI_BU_Indirect = 1 << 2,   // instanced draws read their counts from it (GDISetIndirectArgs)
};

/*
 * One instance of an instanced draw, as the buffer holds it:
 *
 *   bytes  0-63  mat4 transform, column-major   locations 2-5
 *   bytes 64-79  vec4 colour (RGBA, linear)     location 7
 *
 * The shader multiplies the model matrix by the instance's transform and
 * the vertex colour by its colour.
 */
constexpr uint32_t GDI_INSTANCE_STRIDE = 80;

/*
 * One indexed draw's arguments, as an indirect buffer holds them
 * (GDISetIndirectArgs) -- Vulkan's VkDrawIndexedIndirectCommand, D3D12's
 * D3D12_DRAW_INDEXED_ARGUMENTS:
 *
 *   uint32 index count, uint32 instance count, uint32 first index,
 *   int32 vertex offset, uint32 first instance
 *
 * Typically the CPU fills the range once and a compute shader writes the
 * instance count each frame.
 */
constexpr uint32_t GDI_INDIRECT_ARGS_STRIDE = 20;

class GDIBuffer
{
public:
    explicit GDIBuffer(const uint32_t InBytes, const uint8_t InUsage) : Bytes(InBytes), Usage(InUsage) {}
    virtual ~GDIBuffer() = default;

    uint32_t GetSize()  const { return Bytes; }
    uint8_t  GetUsage() const { return Usage; }

    /* Frees it once the GPU no longer reads it; the object is then deleted. */
    virtual void Release() = 0;

    /* Its name in capture tools (GDISetDebugName). */
    virtual void SetDebugName(const std::string& InName) { DebugName = InName; }
    const std::string& GetDebugName() const { return DebugName; }

protected:
    uint32_t    Bytes = 0;
    uint8_t     Usage = 0;
    std::string DebugName;
};

#endif //CORECLIENT_GDIBUFFER_H
