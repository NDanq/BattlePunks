#include "Public/GDI/Vulkan/VKShader.h"
#include "Public/GDI/Vulkan/VKDevice.h"
#include "Public/GDI/Vulkan/VulkanDynamicGDI.h"
#include "Public/GDI/Vulkan/VKPipelineCache.h"
#include "Public/GDI/GDITexture.h"

#include "Public/GDI/GDILog.h"
#include <string>
#include "Public/Core/CoreFileProvider.h"
#include <vector>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

VKShaderResource::VKShaderResource(VKDevice* InDevice, EShaderType InType, const std::string& SpvPath)
    : GDIShaderResource(InType, SpvPath)
    , Device(InDevice)
{
    if(!InDevice)
    {
        GDI_LOG("[VKShaderResource] Null device passed for: " + SpvPath)
        return;
    }

    // Read through the resource provider, BINARY. Under Qt this lands in the
    // .qrc bundle; a standalone host reads the same relative path off disk.
    EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
    if(Provider == nullptr)
    {
        GDI_LOG("[VKShaderResource] No file provider installed for: " + SpvPath)
        return;
    }

    std::vector<uint8_t> Bytes;
    if(!Provider->ReadAll(SpvPath, Bytes))
    {
        GDI_LOG("[VKShaderResource] Failed to open SPIR-V resource: " + SpvPath)
        return;
    }

    if(Bytes.empty())
    {
        GDI_LOG("[VKShaderResource] SPIR-V resource is empty: " + SpvPath)
        return;
    }

    if(Bytes.size() % 4 != 0)
    {
        GDI_LOG("[VKShaderResource] SPIR-V byte count is not a multiple of 4 for: " + SpvPath)
        return;
    }

    VkShaderModuleCreateInfo CreateInfo{};
    CreateInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    CreateInfo.codeSize = static_cast<size_t>(Bytes.size());
    CreateInfo.pCode    = reinterpret_cast<const uint32_t*>(Bytes.data());

    const VkResult Result = vkCreateShaderModule(InDevice->GetDevice(), &CreateInfo, nullptr, &ShaderModule);
    if(Result != VK_SUCCESS)
    {
        GDI_LOG("[VKShaderResource] vkCreateShaderModule failed (" + ::GDILogDetail::GDINum(Result) + ") for: " + SpvPath)
        return;
    }

    /* Named by its file, so a capture reads which material or pass it is. */
    InDevice->SetObjectName(VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<uint64_t>(ShaderModule), SpvPath);

    /* [Shaders] ShaderPDB: RenderDoc reads the cook's debug SPIR-V (its source
     * and names) for this module from the path tagged with its magic value,
     * RENDERDOC_ShaderDebugMagicValue_truncated in renderdoc_app.h. */
    const std::string DebugInfo = GDIGetShaderPDBPath(SpvPath);
    if(!DebugInfo.empty())
    {
        constexpr uint64_t RENDERDOC_SHADER_DEBUG_MAGIC = 0x48656670eab25520ULL;
        InDevice->SetObjectTag(VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<uint64_t>(ShaderModule),
                               RENDERDOC_SHADER_DEBUG_MAGIC, DebugInfo);
    }
    bInitialized = true;
    GDI_LOG("[VKShaderResource] Loaded SPIR-V module: " + SpvPath)
}

// ---------------------------------------------------------------------------
// SetUniform — scalar / vector / matrix overloads
//
// Each overload writes one VKUniformValue into the Scalars map, keyed by
// name.  Duplicate calls with the same name overwrite the previous value,
// matching OpenGL glUniform* semantics (last write before draw wins).
// DrawObject reads these via GetScalars() and packs them into push constants.
// ---------------------------------------------------------------------------

void VKShaderResource::SetUniform(std::string InName, const int InValue)
{
    VKUniformValue V;
    V.Tag    = VKUniformValue::INT;
    V.Data.i = InValue;
    Scalars[InName] = V;
}

void VKShaderResource::SetUniform(std::string InName, const float InValue)
{
    VKUniformValue V;
    V.Tag    = VKUniformValue::FLOAT;
    V.Data.f = InValue;
    Scalars[InName] = V;
}

void VKShaderResource::SetUniform(std::string InName, const Vector2 InVector)
{
    VKUniformValue V;
    V.Tag       = VKUniformValue::VEC2;
    V.Data.v[0] = InVector.GetX();
    V.Data.v[1] = InVector.GetY();
    Scalars[InName] = V;
}

void VKShaderResource::SetUniform(std::string InName, const Vector3 InVector)
{
    VKUniformValue V;
    V.Tag       = VKUniformValue::VEC3;
    V.Data.v[0] = InVector.GetX();
    V.Data.v[1] = InVector.GetY();
    V.Data.v[2] = InVector.GetZ();
    Scalars[InName] = V;
}

void VKShaderResource::SetUniform(std::string InName, const Vector4 InVector)
{
    VKUniformValue V;
    V.Tag       = VKUniformValue::VEC4;
    V.Data.v[0] = InVector.GetX();
    V.Data.v[1] = InVector.GetY();
    V.Data.v[2] = InVector.GetZ();
    V.Data.v[3] = InVector.GetW();
    Scalars[InName] = V;
}

void VKShaderResource::SetUniform(std::string InName, const Matrix4x4 InMatrix)
{
    VKUniformValue V;
    V.Tag = VKUniformValue::MAT;
    // Matrix4x4::Data is row-major double[4][4]; std140 mat4 is column-major, so
    // TRANSPOSE while converting — exactly what the OpenGL path does by passing
    // transpose=GL_TRUE to glUniformMatrix4fv. Without this every shader receives
    // transposed matrices (translation lands in the w row → the canvas quad is
    // clipped into a giant diagonal wedge).
    for(int Row = 0; Row < 4; ++Row)
        for(int Col = 0; Col < 4; ++Col)
            V.Data.v[Col * 4 + Row] = static_cast<float>(InMatrix.Data[Row][Col]);
    Scalars[InName] = V;
}

// ---------------------------------------------------------------------------
// SetUniform — texture overloads
//
// Store the resource pointer and binding index; DrawObject writes the
// appropriate VkDescriptorImageInfo for the bound descriptor set.
// GDITexture2D* variant delegates to the GDITextureResource* variant so
// there is a single code path for descriptor bookkeeping.
// ---------------------------------------------------------------------------

void VKShaderResource::SetUniform(std::string InName, GDITexture2D* InTexture2D, const uint InIndex)
{
    if(!InTexture2D) return;
    SetUniform(InName, InTexture2D->Resource, InIndex);
}

void VKShaderResource::SetUniform(std::string InName, GDITextureResource* InTexResource, const uint InIndex)
{
    if(!InTexResource) return;
    VKTextureBinding Binding;
    Binding.Resource = InTexResource;
    Binding.Index    = InIndex;
    Textures[InName] = Binding;
}

// ---------------------------------------------------------------------------
// Release
// ---------------------------------------------------------------------------

void VKShaderResource::Release(void)
{
    Scalars.clear();
    Textures.clear();

    if(ShaderModule != VK_NULL_HANDLE && Device)
    {
        if(GetShaderType() == ST_Compute)
        {
            if(auto* GDI = dynamic_cast<VulkanDynamicGDI*>(GDynamicGDI))
            {
                if(GDI->GetPipelineCache()) { GDI->GetPipelineCache()->ForgetComputeModule(ShaderModule); }
            }
        }
        vkDestroyShaderModule(Device->GetDevice(), ShaderModule, nullptr);
        ShaderModule = VK_NULL_HANDLE;
    }
    Device = nullptr;
    bInitialized = false;
}

bool VKShaderResource::HasCustomScalars() const
{
    if(CustomScanSize != Scalars.size())
    {
        bCustomScalars = false;
        for(const auto& Each : Scalars)
        {
            const std::string& N = Each.first;
            if(N.compare(0, 15, "ViewParameters.") != 0 && N.compare(0, 10, "Transform.") != 0)
            {
                bCustomScalars = true;
                break;
            }
        }
        CustomScanSize = Scalars.size();
    }
    return bCustomScalars;
}
