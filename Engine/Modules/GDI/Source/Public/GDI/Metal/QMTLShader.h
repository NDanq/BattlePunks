#ifndef CORECLIENT_QMTLSHADER_H
#define CORECLIENT_QMTLSHADER_H


#include "Public/GDI/GDI.h"
#include "Public/GDI/GDIDefinitions.h"
#include "Public/GDI/GDIShader.h"
#include "Public/GDI/Metal/MTLForward.h"
#include <string>
#include <unordered_map>

class QMTLDevice;

// ---------------------------------------------------------------------------
// Uniform staging types -- structurally identical to the Vulkan backend's
// QVKUniformValue / QVKTextureBinding (QVKShader.h). SetUniform writes into these
// maps; QMetalDynamicGDI::BindDrawState reads them to pack setVertexBytes/
// setFragmentBytes uniform blocks and bind textures/samplers.
// ---------------------------------------------------------------------------

/** One scalar/vector/matrix uniform, stored as float data ready to hand to
 *  setVertexBytes:/setFragmentBytes: without conversion. */
struct QMTLUniformValue
{
    enum Tag : uint8_t { INT, FLOAT, VEC2, VEC3, VEC4, MAT } Tag = INT;

    // Largest payload is mat4 = 16 floats = 64 bytes.
    union
    {
        int   i;        // INT
        float f;        // FLOAT
        float v[16];    // VEC2(2) / VEC3(3) / VEC4(4) / MAT(16)
    } Data{};
};

/** One texture/sampler binding, keyed by uniform name. Index is the SPIR-V
 *  binding slot, which (thanks to --msl-decoration-binding) equals the MSL
 *  texture(N)/sampler(N) index. */
struct QMTLTextureBinding
{
    GDITextureResource* Resource = nullptr;
    uint                Index    = 0;
};

/**
 * Metal implementation of GDIShaderResource.
 *
 * Construction reads the cooked MSL text from the Qt resource path returned by
 * GetCookedShaderPath(..., ERP_Metal) and compiles it at runtime via
 * newLibraryWithSource:, then looks up the entry point `main0` (spirv-cross's
 * fixed MSL entry-point name).
 *
 * GetFunction() returns nullptr if loading/compilation failed.
 */
class QMTLShaderResource : public GDIShaderResource
{
public:
    QMTLShaderResource(QMTLDevice* InDevice, EShaderType InType, const std::string& MslPath);

    // GDIShaderResource interface
    void SetUniform(std::string InName, const int InValue) override;
    void SetUniform(std::string InName, const float InValue) override;
    void SetUniform(std::string InName, const Vector2 InVector) override;
    void SetUniform(std::string InName, const Vector3 InVector) override;
    void SetUniform(std::string InName, const Vector4 InVector) override;
    void SetUniform(std::string InName, const Matrix4x4 InMatrix) override;
    void SetUniform(std::string InName, GDITexture2D* InTexture2D, const uint InIndex) override;
    void SetUniform(std::string InName, GDITextureResource* InTexResource, const uint InIndex) override;

    void Release(void) override;

    // Accessors for BindDrawState
    MTLFunctionRef GetFunction() const { return Function; }

    const std::unordered_map<std::string, QMTLUniformValue>&   GetScalars()  const { return Scalars; }
    const std::unordered_map<std::string, QMTLTextureBinding>& GetTextures() const { return Textures; }

private:
    QMTLDevice*     Device   = nullptr;
    MTLLibraryRef   Library  = nullptr;
    MTLFunctionRef  Function = nullptr;

    std::unordered_map<std::string, QMTLUniformValue>   Scalars;
    std::unordered_map<std::string, QMTLTextureBinding> Textures;
};

#endif //CORECLIENT_QMTLSHADER_H
