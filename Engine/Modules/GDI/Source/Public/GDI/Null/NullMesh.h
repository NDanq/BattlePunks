#ifndef CORECLIENT_NULLMESH_H
#define CORECLIENT_NULLMESH_H

#include "Public/GDI/GDIMesh.h"

class NullMesh : public GDIMesh
{
public:
    NullMesh() = default;

    /* An indexed mesh keeps its copied arrays, so IsIndexed() and the draw
     * range behave as on a real backend; nothing is uploaded anywhere. */
    explicit NullMesh(const ORenderData& InData) : GDIMesh(InData) {}

    /* A dynamic mesh: its updates are checked and counted, nothing stored. */
    NullMesh(const uint32_t InMaxVertices, const uint32_t InMaxIndices) { MakeDynamic(InMaxVertices, InMaxIndices); }
    ~NullMesh() override { FreeRenderData(); }

    void InitResource() override {}
    void Release() override { FreeRenderData(); }
    void Draw() override {}

protected:
    bool WriteDynamic(const GDIMeshDesc&, uint32_t, uint32_t) override { return true; }
    void AddInstancedTransformInner() override {}
    void ResetInstancedTransformInner() override {}
};

#endif //CORECLIENT_NULLMESH_H
