#include "Public/GDI/OpenGL/OpenGLDynamicGDI.h"
#include "Public/GDI/OpenGL/OGLMesh.h"
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

void OpenGLDynamicGDI::DrawScreenQuadPrimitive()
{
    if(!ScreenMesh)
    {
        const float				ScreenVertData[18] = {	-1.0f, -1.0f, 0.0f,
                                                             -1.0f, 1.0f, 0.0f,
                                                             1.0f, 1.0f, 0.0f,
                                                             1.0f, 1.0f, 0.0f,
                                                             1.0f, -1.0f, 0.0f,
                                                             -1.0f, -1.0f, 0.0f };
        // v = 0 at NDC +1, the target's row 0: an identity copy.
        const float				ScreenTexCoordData[12] = {	0.0f, 1.0f,
                                                                 0.0f, 0.0f,
                                                                 1.0f, 0.0f,
                                                                 1.0f, 0.0f,
                                                                 1.0f, 1.0f,
                                                                 0.0f, 1.0f };
        const int VertSize = sizeof(ScreenVertData)/sizeof(ScreenVertData[0]);
        const int TexCoordSize = sizeof(ScreenTexCoordData)/sizeof(ScreenTexCoordData[0]);
        ScreenMesh = dynamic_cast<OpenGLMesh *>(CreateQuadMesh(ScreenVertData, ScreenTexCoordData, VertSize, TexCoordSize));
    }
    if(BoundPixelShader)
    {
        BoundPixelShader->UpdateUserDefinedUniforms();
    }
    ScreenMesh->Draw();
}

GDIMesh* OpenGLDynamicGDI::CreateQuadMesh(const float* VertData, const float* TexCoordData, const int InNumVerts, const int InNumTexCoords, bool bIsInstanced)
{
    GDIMesh::ORenderData RenderData;
    size_t VertArraySize = sizeof(float) * InNumVerts;
    RenderData.Position = (float*)malloc(VertArraySize );
    memcpy(RenderData.Position, VertData, VertArraySize );

    size_t TexArraySize = sizeof(float) * InNumTexCoords;
    RenderData.TexCoord = (float*)malloc(TexArraySize );
    memcpy(RenderData.TexCoord, TexCoordData, TexArraySize);

    RenderData.NumVertices = InNumVerts;

    auto* Mesh = new OpenGLMesh(RenderData);
    Mesh->SetSupportsInstanced(bIsInstanced);
    Mesh->InitResource();
    return (GDIMesh*)Mesh;
}

GDIMesh* OpenGLDynamicGDI::CreateMesh(const GDIMeshDesc& InDesc)
{
    GDIMesh::ORenderData RenderData;
    if(!GDIMakeIndexedRenderData(InDesc, RenderData)) return nullptr;

    auto* Mesh = new OpenGLMesh(RenderData);
    Mesh->InitResource();
    return Mesh;
}

GDIMesh* OpenGLDynamicGDI::CreateDynamicMesh(const uint32_t InMaxVertices, const uint32_t InMaxIndices)
{
    auto* Mesh = new OpenGLMesh(InMaxVertices, InMaxIndices);
    Mesh->InitResource();
    return Mesh;
}

GDIMesh* OpenGLDynamicGDI::CreateQuadMesh(const float& InWidth, const float& InHeight, bool bIsInstanced)
{
    const float HalfWidth = InWidth * 0.5f;
    const float HalfHeight = InHeight * 0.5f;

    const float VertQuadData[] = { -HalfWidth, -HalfHeight, 0.f,
                                    -HalfWidth, HalfHeight, 0.f,
                                    HalfWidth, HalfHeight, 0.f,
                                    HalfWidth, HalfHeight, 0.f,
                                    HalfWidth,  -HalfHeight, 0.f,
                                    -HalfWidth, -HalfHeight, 0.f};

    const float VertTexData[] = {	 0.0f, 0.0f,
                                     0.0f, 1.0f,
                                     1.0f, 1.0f,
                                     1.0f, 1.0f,
                                     1.0f, 0.0f,
                                     0.0f, 0.0f };

    return CreateQuadMesh(VertQuadData, VertTexData, 18, 12, bIsInstanced);
}

OpenGLMesh::OpenGLMesh()
{
    initializeOpenGLFunctions();
}

OpenGLMesh::~OpenGLMesh()
{
    Release();
}

OpenGLMesh::OpenGLMesh(const GDIMesh::ORenderData& InData)
{
    initializeOpenGLFunctions();
    SetRenderData(InData);
}

OpenGLMesh::OpenGLMesh(const EMeshType& InMeshType, const ORenderData& InRenderData) :
GDIMesh(InMeshType, InRenderData)
{
    initializeOpenGLFunctions();
}

OpenGLMesh::OpenGLMesh(const uint32_t InMaxVertices, const uint32_t InMaxIndices) :
GDIMesh(MT_Stream, ORenderData())
{
    initializeOpenGLFunctions();
    MakeDynamic(InMaxVertices, InMaxIndices);
}

void OpenGLMesh::InitDynamicResource()
{
    const GLsizeiptr NumVerts = GLsizeiptr(DynamicVertexCapacity);

    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);
    glGenBuffers(2, VBO);
    glGenBuffers(1, &NormalVBO);
    glGenBuffers(1, &ColorVBO);
    glGenBuffers(1, &UV1VBO);
    glGenBuffers(1, &IndexEBO);

    /* The streams of GDIMeshDesc's layout, at capacity. Normals and UV1 are
     * zero once here: UpdateDynamic writes them only when given. */
    const std::vector<float> Zero(size_t(NumVerts) * 3, 0.0f);
    const struct { uint Buffer; GLuint Location; GLint Floats; const float* Initial; } Streams[] = {
        { VBO[ORenderData::Buffers::VERTEX_BUFFER],   0,  3, nullptr     },
        { VBO[ORenderData::Buffers::TEXCOORD_BUFFER], 1,  2, nullptr     },
        { NormalVBO,                                  8,  3, Zero.data() },
        { ColorVBO,                                   9,  4, nullptr     },
        { UV1VBO,                                     10, 2, Zero.data() },
    };
    for(const auto& Each : Streams)
    {
        glBindBuffer(GL_ARRAY_BUFFER, Each.Buffer);
        glBufferData(GL_ARRAY_BUFFER, NumVerts * Each.Floats * GLsizeiptr(sizeof(float)), Each.Initial, GL_STREAM_DRAW);
        glEnableVertexAttribArray(Each.Location);
        glVertexAttribPointer(Each.Location, Each.Floats, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    // Bound while the VAO is: the VAO records the element buffer.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, IndexEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(DynamicIndexCapacity) * GLsizeiptr(sizeof(uint32_t)), nullptr, GL_STREAM_DRAW);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

bool OpenGLMesh::WriteDynamic(const GDIMeshDesc& InDesc, const uint32_t InBaseVertex, const uint32_t InBaseIndex)
{
    if(!VAO) return false;

    const auto Write = [this](const uint InBuffer, const GLenum InTarget, const size_t InElementBytes,
                              const uint32_t InBase, const uint32_t InCount, const void* InData)
    {
        glBindBuffer(InTarget, InBuffer);
        glBufferSubData(InTarget, GLintptr(InBase * InElementBytes), GLsizeiptr(InCount * InElementBytes), InData);
    };

    const uint32_t N = InDesc.NumVertices;
    Write(VBO[ORenderData::Buffers::VERTEX_BUFFER],   GL_ARRAY_BUFFER, sizeof(float) * 3, InBaseVertex, N, InDesc.Positions);
    Write(VBO[ORenderData::Buffers::TEXCOORD_BUFFER], GL_ARRAY_BUFFER, sizeof(float) * 2, InBaseVertex, N, InDesc.UV0);
    if(InDesc.Normals) Write(NormalVBO, GL_ARRAY_BUFFER, sizeof(float) * 3, InBaseVertex, N, InDesc.Normals);
    if(InDesc.UV1)     Write(UV1VBO,    GL_ARRAY_BUFFER, sizeof(float) * 2, InBaseVertex, N, InDesc.UV1);
    if(InDesc.Colors)
    {
        Write(ColorVBO, GL_ARRAY_BUFFER, sizeof(float) * 4, InBaseVertex, N, InDesc.Colors);
    }
    else
    {
        const std::vector<float> White(size_t(N) * 4, 1.0f);
        Write(ColorVBO, GL_ARRAY_BUFFER, sizeof(float) * 4, InBaseVertex, N, White.data());
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /* The element buffer belongs to the VAO, so bind it there to write it. */
    glBindVertexArray(VAO);
    Write(IndexEBO, GL_ELEMENT_ARRAY_BUFFER, sizeof(uint32_t), InBaseIndex, InDesc.NumIndices, InDesc.Indices);
    glBindVertexArray(0);
    return true;
}

void OpenGLMesh::InitResource()
{
    if(bDynamic)
    {
        InitDynamicResource();
        return;
    }

    const int NumVertices = RenderData.NumVertices;

    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    // Non-instanced meshes use only VERTEX + TEXCOORD; instanced meshes add the
    // transform and opacity buffers (the two trailing enum entries).
    const int NumBuffers = bSupportsInstanced ? (int)ORenderData::Buffers::NUM_BUFFERS
                                              : (int)ORenderData::Buffers::INSTANCED_TRANSFORM_BUFFER;
    glGenBuffers(NumBuffers, VBO);

    glBindBuffer(GL_ARRAY_BUFFER, VBO[ORenderData::Buffers::VERTEX_BUFFER]);
    glBufferData(GL_ARRAY_BUFFER, NumVertices * sizeof(float), RenderData.Position, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    // NumVertices counts position FLOATS (3 per vertex); a texcoord is 2.
    glBindBuffer(GL_ARRAY_BUFFER, VBO[ORenderData::Buffers::TEXCOORD_BUFFER]);
    glBufferData(GL_ARRAY_BUFFER, (NumVertices / 3) * 2 * sizeof(float), RenderData.TexCoord, GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    if( RenderData.Normal )
    {
        glGenBuffers(1, &NormalVBO);
        glBindBuffer(GL_ARRAY_BUFFER, NormalVBO);
        glBufferData(GL_ARRAY_BUFFER, NumVertices * sizeof(float), RenderData.Normal, GL_STATIC_DRAW);
        glEnableVertexAttribArray(8);
        glVertexAttribPointer(8, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    if( RenderData.Color && IsIndexed() )
    {
        glGenBuffers(1, &ColorVBO);
        glBindBuffer(GL_ARRAY_BUFFER, ColorVBO);
        glBufferData(GL_ARRAY_BUFFER, (NumVertices / 3) * 4 * sizeof(float), RenderData.Color, GL_STATIC_DRAW);
        glEnableVertexAttribArray(9);
        glVertexAttribPointer(9, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    if( RenderData.UV1 && IsIndexed() )
    {
        glGenBuffers(1, &UV1VBO);
        glBindBuffer(GL_ARRAY_BUFFER, UV1VBO);
        glBufferData(GL_ARRAY_BUFFER, (NumVertices / 3) * 2 * sizeof(float), RenderData.UV1, GL_STATIC_DRAW);
        glEnableVertexAttribArray(10);
        glVertexAttribPointer(10, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    if( IsSkinned() )
    {
        // Bone indices (as floats) and weights, locations 11 and 12.
        const struct { uint* Buffer; GLuint Location; const float* Data; } Streams[2] = {
            { &JointVBO, 11, RenderData.Joints }, { &WeightVBO, 12, RenderData.Weights } };
        for(const auto& Stream : Streams)
        {
            glGenBuffers(1, Stream.Buffer);
            glBindBuffer(GL_ARRAY_BUFFER, *Stream.Buffer);
            glBufferData(GL_ARRAY_BUFFER, (NumVertices / 3) * 4 * sizeof(float), Stream.Data, GL_STATIC_DRAW);
            glEnableVertexAttribArray(Stream.Location);
            glVertexAttribPointer(Stream.Location, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        }
    }

    if( IsIndexed() )
    {
        // Bound while the VAO is: the VAO records the element buffer.
        glGenBuffers(1, &IndexEBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, IndexEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, RenderData.NumIndices * sizeof(uint32_t), RenderData.Indices, GL_STATIC_DRAW);
    }

    if( bSupportsInstanced )
    {
        glBindBuffer(GL_ARRAY_BUFFER, VBO[ORenderData::Buffers::INSTANCED_TRANSFORM_BUFFER]);
        glBufferData(GL_ARRAY_BUFFER, sizeof(Matrix4x4) * MaxInstanced, nullptr, GL_DYNAMIC_DRAW);

        const size_t Mat4x4Size = sizeof(GLfloat) * 4 * 4;
        const size_t Vec4Size = sizeof(float) * 4;

        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, Mat4x4Size, (void*)0);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, Mat4x4Size, (void*)(Vec4Size));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, Mat4x4Size, (void*)(2 * Vec4Size));
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, Mat4x4Size, (void*)(3 * Vec4Size));

        glVertexAttribDivisor(2, 1);
        glVertexAttribDivisor(3, 1);
        glVertexAttribDivisor(4, 1);
        glVertexAttribDivisor(5, 1);

        // Per-instance opacity at location 6 (float, divisor 1) — mirrors the
        // Vulkan binding 3 / location 6 attribute for pressure-driven alpha.
        glBindBuffer(GL_ARRAY_BUFFER, VBO[ORenderData::Buffers::INSTANCED_OPACITY_BUFFER]);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * MaxInstanced, nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(6);
        glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(float), (void*)0);
        glVertexAttribDivisor(6, 1);

        // Per-instance color at location 7 (vec3, divisor 1) — mirrors the Vulkan
        // binding 4 / location 7 attribute for pressure-driven color transfer.
        glBindBuffer(GL_ARRAY_BUFFER, VBO[ORenderData::Buffers::INSTANCED_COLOR_BUFFER]);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 3 * MaxInstanced, nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(7);
        glVertexAttribPointer(7, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, (void*)0);
        glVertexAttribDivisor(7, 1);

        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glBindVertexArray(0);
}

void OpenGLMesh::Release()
{
    const int NumBuffersToDelete = bSupportsInstanced ? (int)ORenderData::Buffers::NUM_BUFFERS
                                                       : (int)ORenderData::Buffers::INSTANCED_TRANSFORM_BUFFER;
    if(VAO)
    {
        glDeleteBuffers(NumBuffersToDelete, VBO);
        if(NormalVBO) { glDeleteBuffers(1, &NormalVBO); NormalVBO = 0; }
        if(ColorVBO)  { glDeleteBuffers(1, &ColorVBO);  ColorVBO  = 0; }
        if(UV1VBO)    { glDeleteBuffers(1, &UV1VBO);    UV1VBO    = 0; }
        if(JointVBO)  { glDeleteBuffers(1, &JointVBO);  JointVBO  = 0; }
        if(WeightVBO) { glDeleteBuffers(1, &WeightVBO); WeightVBO = 0; }
        if(IndexEBO)  { glDeleteBuffers(1, &IndexEBO);  IndexEBO  = 0; }
        glDeleteVertexArrays(1, &VAO);
        VAO = 0;
    }
    FreeRenderData();
}

void OpenGLMesh::Draw()
{
    GDI_CHECK(VAO, "VAO Not Valid");
    glBindVertexArray(VAO);

    const uint DrawCount = RenderData.NumVertices / 3;

    if( IsIndexed() )
    {
        uint32_t First = 0, Count = 0;
        GetClampedDrawRange(First, Count);
        if( Count > 0 && bDynamic )
        {
            /* The last update's range of the stream buffers. */
            glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(Count), GL_UNSIGNED_INT,
                                     reinterpret_cast<const void*>(uintptr_t(GetDynamicBaseIndex() + First) * sizeof(uint32_t)),
                                     GLint(GetDynamicBaseVertex()));
        }
        else if( Count > 0 )
        {
            glDrawElements(GL_TRIANGLES, GLsizei(Count), GL_UNSIGNED_INT,
                           reinterpret_cast<const void*>(uintptr_t(First) * sizeof(uint32_t)));
        }
    }
    else if( bSupportsInstanced )
    {
        //GDI_LOG("Instances Drawn: " + ::GDILogDetail::GDINum(NumInstanced));
        glDrawArraysInstanced(GL_TRIANGLES, 0, DrawCount, NumInstanced);
    }
    else
    {
        glDrawArrays(GL_TRIANGLES, 0, DrawCount );
    }

    glBindVertexArray(0);
}

void OpenGLMesh::AddInstancedTransformInner()
{
    Matrix4x4 LastMatrix = InstancedTransforms.back();
    GLfloat MatrixData[4][4];
    auto LastMatrixData = LastMatrix.Data;
    // The mat4 attribute is consumed as 4 column vec4s, so pack column-major —
    // the same transpose the uniform path gets from transpose=GL_TRUE.
    for(ushort i = 0; i < 4; i++)
    {
        for(ushort j = 0; j < 4; j++)
        {
            MatrixData[j][i] = GLfloat(LastMatrixData[i][j]);
        }
    }

    const size_t Mat4x4Size = sizeof(GLfloat) * 4 * 4;

    // NumInstanced was already incremented by the base AddInstancedTransform(),
    // so this transform belongs in slot NumInstanced - 1.
    const int Slot = NumInstanced - 1;

    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO[ORenderData::Buffers::INSTANCED_TRANSFORM_BUFFER]);
    glBufferSubData(GL_ARRAY_BUFFER, Slot * Mat4x4Size, Mat4x4Size, reinterpret_cast<const GLfloat*>(MatrixData[0]));

    // Same slot in the parallel opacity buffer.
    if(!InstancedOpacities.empty())
    {
        const GLfloat Opac = GLfloat(InstancedOpacities.back());
        glBindBuffer(GL_ARRAY_BUFFER, VBO[ORenderData::Buffers::INSTANCED_OPACITY_BUFFER]);
        glBufferSubData(GL_ARRAY_BUFFER, Slot * sizeof(GLfloat), sizeof(GLfloat), &Opac);
    }

    // Same slot in the parallel color buffer (vec3).
    if(!InstancedColors.empty())
    {
        const Vector3& C = InstancedColors.back();
        const GLfloat ColorData[3] = { GLfloat(C.GetX()), GLfloat(C.GetY()), GLfloat(C.GetZ()) };
        glBindBuffer(GL_ARRAY_BUFFER, VBO[ORenderData::Buffers::INSTANCED_COLOR_BUFFER]);
        glBufferSubData(GL_ARRAY_BUFFER, Slot * sizeof(GLfloat) * 3, sizeof(GLfloat) * 3, ColorData);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

void OpenGLMesh::ResetInstancedTransformInner()
{

}


