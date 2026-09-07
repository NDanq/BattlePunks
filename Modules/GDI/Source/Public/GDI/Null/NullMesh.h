#ifndef CORECLIENT_NULLMESH_H
#define CORECLIENT_NULLMESH_H

#include "Public/GDI/GDIMesh.h"

class NullMesh : public GDIMesh
{
public:
    NullMesh() = default;

    void InitResource() override {}
    void Release() override {}
    void Draw() override {}

protected:
    void AddInstancedTransformInner() override {}
    void ResetInstancedTransformInner() override {}
};

#endif //CORECLIENT_NULLMESH_H
