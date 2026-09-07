#ifndef CORECLIENT_QOGLMESH_H
#define CORECLIENT_QOGLMESH_H
#include "Public/GDI/OpenGL/GDIOpenGL.h"


#include "Public/GDI/GDIMesh.h"

class QOpenGLMesh : public GDIMesh,
        protected GDIGLFunctions
{
public:
    QOpenGLMesh();

    QOpenGLMesh(const ORenderData& InData);

    QOpenGLMesh(const EMeshType& InMeshType, const ORenderData& InData);

    ~QOpenGLMesh() override;

    void InitResource() override;

    void Release() override;

    void Draw() override;

protected:
    void AddInstancedTransformInner() override;

    void ResetInstancedTransformInner() override;

private:
    uint VAO;
    uint VBO[ORenderData::Buffers::NUM_BUFFERS];
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

#endif //CORECLIENT_QOGLMESH_H
