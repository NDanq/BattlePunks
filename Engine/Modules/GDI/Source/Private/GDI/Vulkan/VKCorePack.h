#ifndef CORECLIENT_VKCOREPACK_H
#define CORECLIENT_VKCOREPACK_H

#include "Public/GDI/GDIView.h"

#include <cstring>

/*
 * The core uniform blocks packed as the shaders read them (std140), with no
 * string per value (MULTITHREADED_RENDERING.md, phase 1): DrawObject's and a
 * recording context's RecordDrawObject's (phase 2).
 */
namespace VKCorePack
{
    /* std140 mat4 from Matrix4x4's row-major doubles: column-major floats,
     * transposed as VKShaderResource::SetUniform(Matrix4x4) stores them. */
    inline void MatrixToStd140(const Matrix4x4& InMatrix, float* OutColumns)
    {
        for(int Row = 0; Row < 4; ++Row)
            for(int Col = 0; Col < 4; ++Col)
                OutColumns[Col * 4 + Row] = static_cast<float>(InMatrix.Data[Row][Col]);
    }

    /* The ViewParameters block of InView, as UpdateCoreUniforms' four scalars
     * pack it (PackViewParamsUBO's layout), before the clip-space remap. */
    inline void PackCoreView(GDIView* InView, float (&OutView)[40])
    {
        MatrixToStd140(InView->Camera->Projection, OutView + 0);
        MatrixToStd140(InView->Camera->ViewMat, OutView + 16);
        const Vector3 Mouse(float(InView->MouseState.PosX), float(InView->MouseState.PosY),
                            InView->MouseState.bMouseDown ? 1.0f : 0.0f);
        OutView[32] = static_cast<float>(Mouse.GetX());
        OutView[33] = static_cast<float>(Mouse.GetY());
        OutView[34] = static_cast<float>(Mouse.GetZ());
        OutView[35] = 0.0f;
        const Vector4 Resolution(float(InView->Viewport.ViewportWidth), float(InView->Viewport.ViewportHeight),
                                 1.0f / InView->Viewport.ViewportWidth, 1.0f / InView->Viewport.ViewportHeight);
        OutView[36] = static_cast<float>(Resolution.GetX());
        OutView[37] = static_cast<float>(Resolution.GetY());
        OutView[38] = static_cast<float>(Resolution.GetZ());
        OutView[39] = static_cast<float>(Resolution.GetW());
    }

    /* The Transform block of InTransform (PackTransformUBO's layout). */
    inline void PackCoreTransform(const GDITransform* InTransform, float (&OutTransform)[36])
    {
        const Matrix4x4 Model = InTransform->GetModel();
        OutTransform[0] = static_cast<float>(InTransform->Translate.GetX());
        OutTransform[1] = static_cast<float>(InTransform->Translate.GetY());
        OutTransform[2] = static_cast<float>(InTransform->Translate.GetZ());
        OutTransform[3] = 0.0f;
        MatrixToStd140(Model, OutTransform + 4);
        MatrixToStd140(InTransform->bHasPrevModel ? InTransform->PrevModel : Model, OutTransform + 20);
    }

    /* The camera's GL projection to Vulkan's clip z (see PackViewParamsUBO). */
    inline void RemapClipZ(float* InOutView)
    {
        for(int Col = 0; Col < 4; ++Col)
        {
            const float Z = InOutView[Col * 4 + 2];
            const float W = InOutView[Col * 4 + 3];
            InOutView[Col * 4 + 2] = 0.5f * Z + 0.5f * W;
        }
    }

}

#endif //CORECLIENT_VKCOREPACK_H
