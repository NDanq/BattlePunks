#include "Public/RenderCore/RenderContext.h"

#include <atomic>

#include "Public/GDI/Shaders/Classes/TestbedShaders.h"
#include "Private/RenderCore/RenderCoreInternal.h"

/*
 * DrawQuad's whole job: one 1x1 quad mesh, the Basic_VS/Basic_PS pair every
 * backend already registers in InitializeGDI (GDI.cpp), and a GDIView/
 * GDIViewObject pair whose Camera projection/view matrices are overwritten
 * from RenderView every SetView() -- so the shaders see exactly what a
 * GDICamera would have produced, without RenderContext owning one itself
 * (GDICamera's own UpdateDimensions recomputes from a width/height it does
 * not have here; RenderView already carries the finished matrices).
 */

namespace RenderCore
{
    struct RenderContext::Impl
    {
        ERenderBackend Backend      = ERenderBackend::Null;
        uint64_t       FrameNumber  = 0;
        RenderView     View;

        GDIView*       View3D    = nullptr;   // owns the GDICamera DrawQuad overwrites per SetView()
        GDIViewObject* ViewObject = nullptr;   // owns the shared unit quad mesh
        bool           bShadersBound = false;

        std::atomic<uint64_t> DrawCallCount{0};

        ~Impl()
        {
            delete ViewObject;
            delete View3D;
        }

        void EnsureInitialised()
        {
            if(View3D)
            {
                return;
            }

            View3D = new GDIView();

            /* GDIViewObject's own constructor creates a half-unit quad
             * (HalfQuadVertData in GDIView.cpp) through GDICreateQuadMesh --
             * exactly the "one mesh, scaled per draw" shape DrawQuad wants;
             * GDITransform::Scale (set per call below) is what actually
             * varies a primitive's on-screen size. */
            ViewObject = new GDIViewObject(View3D);
        }
    };

    RenderContext::RenderContext() : P(new Impl())
    {
    }

    RenderContext::~RenderContext()
    {
        delete P;
    }

    ERenderBackend RenderContext::GetBackend() const
    {
        return P->Backend;
    }

    uint64_t RenderContext::GetFrameNumber() const
    {
        return P->FrameNumber;
    }

    const RenderView& RenderContext::GetView() const
    {
        return P->View;
    }

    void RenderContext::DrawQuad(const RenderTransform& InTransform, const float InWidth, const float InHeight,
                                  const EngineCore::Color InTint)
    {
        P->EnsureInitialised();

        if(!P->bShadersBound)
        {
            GDISetBoundShader(GDIGetShader<Basic_VS>(), GDIGetShader<Basic_PS>());
            P->bShadersBound = true;
        }

        /* Camera's matrices are the finished RenderView's, not recomputed
         * from a width/height GDICamera would otherwise need -- overwritten
         * here rather than in SetView() so a RenderContext used with no
         * RenderScene at all (a hand-built scene in a test, per this
         * build-order step's own smoke cases) still draws correctly as long
         * as SetView() was called at least once. */
        P->View3D->Camera->Projection = P->View.Projection;
        P->View3D->Camera->ViewMat    = P->View.View;

        GDITransform* Transform = P->ViewObject->Transform;
        Transform->Translate = InTransform.Position;
        Transform->Rotate    = InTransform.Rotation;
        Transform->Scale     = EngineCore::Vector3(InTransform.Scale.GetX() * double(InWidth),
                                                    InTransform.Scale.GetY() * double(InHeight),
                                                    InTransform.Scale.GetZ());

        /* Basic_PS::InitUniformBindings (TestbedShaders.h) registers
         * "TestColor" as a Vector3 -- overwritten here, per draw, with
         * InTint's normalised channels, so two proxies with different tints
         * drawn in the same frame each see their own colour rather than
         * whichever one bound the shader last. */
        if(GDIShader* Pixel = GDIGetShader<Basic_PS>())
        {
            Pixel->SetUniform("TestColor", EngineCore::Vector3(InTint.Rf(), InTint.Gf(), InTint.Bf()));
        }

        GDIDrawObject(P->ViewObject);

        P->DrawCallCount.fetch_add(1, std::memory_order_relaxed);
    }

    void RenderContext::BeginDrawEvent(const char* InName)
    {
        GDIBeginDrawEvent(InName ? InName : "");
    }

    void RenderContext::EndDrawEvent()
    {
        GDIEndDrawEvent();
    }

    uint64_t RenderContext::GetDrawCallCount() const
    {
        return P->DrawCallCount.load(std::memory_order_relaxed);
    }

    void RenderContext::SetBackend(const ERenderBackend InBackend)
    {
        P->Backend = InBackend;
    }

    void RenderContext::SetFrameNumber(const uint64_t InFrameNumber)
    {
        P->FrameNumber = InFrameNumber;
    }

    void RenderContext::SetView(const RenderView& InView)
    {
        P->View = InView;
    }
}
