#ifndef CORECLIENT_VKSHADER_H
#define CORECLIENT_VKSHADER_H

#include <vulkan/vulkan.h>

#include "Public/GDI/GDI.h"
#include "Public/GDI/GDIDefinitions.h"
#include "Public/GDI/GDIShader.h"
#include <string>
#include <unordered_map>

class VKDevice;

// ---------------------------------------------------------------------------
// Uniform staging types
//
// SetUniform writes into these maps; VulkanDynamicGDI::DrawObject reads them
// to pack push constants and write descriptor sets.
// ---------------------------------------------------------------------------

/**
 * One scalar/vector/matrix uniform value, stored as float data so it is
 * ready to hand directly to vkCmdPushConstants without further conversion.
 *
 * Tag enum matches GDIShader::GDIUniformData::DataFrequency for INT..MAT.
 */
struct VKUniformValue
{
    enum Tag : uint8_t { INT, FLOAT, VEC2, VEC3, VEC4, MAT } Tag = INT;

    // Largest payload is mat4 = 16 floats = 64 bytes.
    // Smaller types use the leading elements; the rest is unused.
    union
    {
        int   i;        // INT
        float f;        // FLOAT
        float v[16];    // VEC2(2) / VEC3(3) / VEC4(4) / MAT(16)
    } Data{};
};

/** One texture/sampler binding, keyed by uniform name. */
struct VKTextureBinding
{
    GDITextureResource* Resource = nullptr;
    uint                Index    = 0;      // sampler binding slot (0 or 1)
};

/**
 * Vulkan implementation of GDIShaderResource.
 *
 * Construction loads a SPIR-V binary from the Qt resource path returned by
 * ProcessShaderFromSource(..., ERP_Vulkan, ...) and creates a VkShaderModule.
 *
 * SetUniform overloads stage uniform data into two flat maps:
 *   Scalars  — int / float / vecN / mat4 values, consumed by DrawObject as
 *              push constants (vkCmdPushConstants).
 *   Textures — GDITextureResource* + binding index, consumed by DrawObject
 *              for descriptor-set writes.
 *
 * GetShaderModule() returns VK_NULL_HANDLE if loading failed.
 */
class VKShaderResource : public GDIShaderResource
{
public:
    VKShaderResource(VKDevice* InDevice, EShaderType InType, const std::string& SpvPath);

    // ------------------------------------------------------------------
    // GDIShaderResource interface
    // ------------------------------------------------------------------
    void SetUniform(std::string InName, const int InValue) override;
    void SetUniform(std::string InName, const float InValue) override;
    void SetUniform(std::string InName, const Vector2 InVector) override;
    void SetUniform(std::string InName, const Vector3 InVector) override;
    void SetUniform(std::string InName, const Vector4 InVector) override;
    void SetUniform(std::string InName, const Matrix4x4 InMatrix) override;
    void SetUniform(std::string InName, GDITexture2D* InTexture2D, const uint InIndex) override;
    void SetUniform(std::string InName, GDITextureResource* InTexResource, const uint InIndex) override;

    void Release(void) override;

    // ------------------------------------------------------------------
    // Accessors for DrawObject
    // ------------------------------------------------------------------
    inline VkShaderModule GetShaderModule() const { return ShaderModule; }

    /** Scalar/vector/matrix uniforms staged this frame. */
    inline const std::unordered_map<std::string, VKUniformValue>&  GetScalars()  const { return Scalars; }
    /** Texture/sampler uniforms staged this frame. */
    inline const std::unordered_map<std::string, VKTextureBinding>& GetTextures() const { return Textures; }

private:
    VKDevice*       Device        = nullptr;
    VkShaderModule  ShaderModule  = VK_NULL_HANDLE;

    std::unordered_map<std::string, VKUniformValue>  Scalars;
    std::unordered_map<std::string, VKTextureBinding> Textures;
};

#endif //CORECLIENT_VKSHADER_H
