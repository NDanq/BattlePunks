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
        const float				ScreenTexCoordData[12] = {	0.0f, 0.0f,
                                                                 0.0f, 1.0f,
                                                                 1.0f, 1.0f,
                                                                 1.0f, 1.0f,
                                                                 1.0f, 0.0f,
                                                                 0.0f, 0.0f };
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

void OpenGLMesh::InitResource()
{
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

    glBindBuffer(GL_ARRAY_BUFFER, VBO[ORenderData::Buffers::TEXCOORD_BUFFER]);
    glBufferData(GL_ARRAY_BUFFER, NumVertices*sizeof(float), RenderData.TexCoord, GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

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
    glDeleteBuffers(NumBuffersToDelete, VBO);
    glDeleteVertexArrays(1, &VAO);
    free(RenderData.Position);
    free(RenderData.TexCoord);
}

void OpenGLMesh::Draw()
{
    GDI_CHECK(VAO, "VAO Not Valid");
    glBindVertexArray(VAO);

    const uint DrawCount = RenderData.NumVertices / 3;

    if( bSupportsInstanced )
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


