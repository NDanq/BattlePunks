#ifndef CORECLIENT_OGLMESH_H
#define CORECLIENT_OGLMESH_H
#include "Public/GDI/OpenGL/GDIOpenGL.h"


#include "Public/GDI/GDIMesh.h"

class OpenGLMesh : public GDIMesh,
        protected GDIGLFunctions
{
public:
    OpenGLMesh();

    OpenGLMesh(const ORenderData& InData);

    OpenGLMesh(const EMeshType& InMeshType, const ORenderData& InData);

    ~OpenGLMesh() override;

    void InitResource() override;

    void Release() override;

    void Draw() override;

protected:
    void AddInstancedTransformInner() override;

    void ResetInstancedTransformInner() override;

private:
    uint VAO;
    uint VBO[ORenderData::Buffers::NUM_BUFFERS];

    /* Indexed meshes only (GDICreateMesh): normals at location 8, and the
     * element buffer the VAO records. 0 for the quads. */
    uint NormalVBO = 0;
    uint ColorVBO  = 0;   // indexed only; location 9
    uint IndexEBO  = 0;
    inline GLenum DecodeMeshType()
    {
        switch(MeshType)
        {
            case GDIMesh::MT_Static:
                return GL_STATIC_DRAW;
            case GDIMesh::MT_Stream:
                return GL_STREAM_DRAW;
        }
        return GL_NONE;
    }
};

#endif //CORECLIENT_OGLMESH_H
