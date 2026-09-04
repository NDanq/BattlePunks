#include "Public/GDI/Metal/QMTLShader.h"
#include "Public/Core/CoreFileProvider.h"
#include <vector>
#include "Public/GDI/Metal/QMTLDevice.h"
#include "Public/GDI/GDITexture.h"

#include "Public/GDI/GDILog.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

QMTLShaderResource::QMTLShaderResource(QMTLDevice* InDevice, EShaderType InType, const std::string& MslPath)
    : GDIShaderResource(InType, MslPath)
    , Device(InDevice)
{
    if(!InDevice || !InDevice->IsValid())
    {
        GDI_LOG("[QMTLShaderResource] Null/invalid device passed for: " + MslPath)
        return;
    }

    // Read through the resource provider, BINARY. Under Qt this lands in the
    // .qrc bundle; a standalone host reads the same relative path off disk.
    EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
    if(Provider == nullptr)
    {
        GDI_LOG("[QMTLShaderResource] No file provider installed for: " + MslPath)
        return;
    }

    std::vector<uint8_t> Bytes;
    if(!Provider->ReadAll(MslPath, Bytes))
    {
        GDI_LOG("[QMTLShaderResource] Failed to open MSL resource: " + MslPath)
        return;
    }

    if(Bytes.empty())
    {
        GDI_LOG("[QMTLShaderResource] MSL resource is empty: " + MslPath)
        return;
    }

    NSString* Source = [[[NSString alloc] initWithBytes:Bytes.data()
                                                 length:(NSUInteger)Bytes.size()
                                               encoding:NSUTF8StringEncoding] autorelease];
    if(Source == nil)
    {
        GDI_LOG("[QMTLShaderResource] MSL source is not valid UTF-8: " + MslPath)
        return;
    }

    NSError* Error = nil;
    MTLCompileOptions* Options = [[[MTLCompileOptions alloc] init] autorelease];
    Library = [InDevice->GetDevice() newLibraryWithSource:Source options:Options error:&Error];  // +1
    if(Library == nil)
    {
        const char* Desc = Error ? [[Error localizedDescription] UTF8String] : "unknown error";
        GDI_LOG("[QMTLShaderResource] newLibraryWithSource failed for " + MslPath + ": " + ::GDILogDetail::ToLogString(Desc))
        return;
    }

    // spirv-cross always names the MSL entry point "main0".
    Function = [Library newFunctionWithName:@"main0"];  // +1
    if(Function == nil)
    {
        GDI_LOG("[QMTLShaderResource] Entry point 'main0' not found in: " + MslPath)
        [Library release];
        Library = nullptr;
        return;
    }

    bInitialized = true;
    GDI_LOG("[QMTLShaderResource] Compiled MSL module: " + MslPath)
}

// ---------------------------------------------------------------------------
// SetUniform -- scalar / vector / matrix overloads (mirrors QVKShaderResource)
// ---------------------------------------------------------------------------

void QMTLShaderResource::SetUniform(std::string InName, const int InValue)
{
    QMTLUniformValue V;
    V.Tag    = QMTLUniformValue::INT;
    V.Data.i = InValue;
    Scalars[InName] = V;
}

void QMTLShaderResource::SetUniform(std::string InName, const float InValue)
{
    QMTLUniformValue V;
    V.Tag    = QMTLUniformValue::FLOAT;
    V.Data.f = InValue;
    Scalars[InName] = V;
}

void QMTLShaderResource::SetUniform(std::string InName, const Vector2 InVector)
{
    QMTLUniformValue V;
    V.Tag       = QMTLUniformValue::VEC2;
    V.Data.v[0] = InVector.GetX();
    V.Data.v[1] = InVector.GetY();
    Scalars[InName] = V;
}

void QMTLShaderResource::SetUniform(std::string InName, const Vector3 InVector)
{
    QMTLUniformValue V;
    V.Tag       = QMTLUniformValue::VEC3;
    V.Data.v[0] = InVector.GetX();
    V.Data.v[1] = InVector.GetY();
    V.Data.v[2] = InVector.GetZ();
    Scalars[InName] = V;
}

void QMTLShaderResource::SetUniform(std::string InName, const Vector4 InVector)
{
    QMTLUniformValue V;
    V.Tag       = QMTLUniformValue::VEC4;
    V.Data.v[0] = InVector.GetX();
    V.Data.v[1] = InVector.GetY();
    V.Data.v[2] = InVector.GetZ();
    V.Data.v[3] = InVector.GetW();
    Scalars[InName] = V;
}

void QMTLShaderResource::SetUniform(std::string InName, const Matrix4x4 InMatrix)
{
    QMTLUniformValue V;
    V.Tag = QMTLUniformValue::MAT;
    // Matrix4x4::Data is row-major double[4][4]; MSL (and the std140 layout the
    // spirv-cross-generated structs expect) is column-major float4x4, so TRANSPOSE
    // while converting -- exactly as the Vulkan path does (QVKShaderResource) and the
    // OpenGL path does via transpose=GL_TRUE. Without this, translation lands in the
    // w row and the canvas quad is clipped into a diagonal wedge.
    for(int Row = 0; Row < 4; ++Row)
        for(int Col = 0; Col < 4; ++Col)
            V.Data.v[Col * 4 + Row] = static_cast<float>(InMatrix.Data[Row][Col]);
    Scalars[InName] = V;
}

// ---------------------------------------------------------------------------
// SetUniform -- texture overloads
// ---------------------------------------------------------------------------

void QMTLShaderResource::SetUniform(std::string InName, GDITexture2D* InTexture2D, const uint InIndex)
{
    if(!InTexture2D) return;
    SetUniform(InName, InTexture2D->Resource, InIndex);
}

void QMTLShaderResource::SetUniform(std::string InName, GDITextureResource* InTexResource, const uint InIndex)
{
    if(!InTexResource) return;
    QMTLTextureBinding Binding;
    Binding.Resource = InTexResource;
    Binding.Index    = InIndex;
    Textures[InName] = Binding;
}

// ---------------------------------------------------------------------------
// Release
// ---------------------------------------------------------------------------

void QMTLShaderResource::Release(void)
{
    Scalars.clear();
    Textures.clear();

    if(Function != nullptr)
    {
        [Function release];
        Function = nullptr;
    }
    if(Library != nullptr)
    {
        [Library release];
        Library = nullptr;
    }
    Device = nullptr;
    bInitialized = false;
}
