#include "Public/RenderCore/RenderContext.h"

#include <atomic>
#include <cmath>

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

#if WITH_EDITOR
    void RenderContext::DrawLine(const EngineCore::Vector3& InStart, const EngineCore::Vector3& InEnd,
                                 const EngineCore::Color InColor, const float InThicknessPixels)
    {
        using EngineCore::Vector3;

        const RenderView& View = P->View;
        const Vector3 Delta  = InEnd - InStart;
        const double  Length = Delta.GetLength();
        if(Length < 1e-6 || View.ViewportPixels.Height <= 0)
        {
            return;
        }
        const Vector3 Along = Delta * (1.0 / Length);
        const Vector3 Mid   = (InStart + InEnd) * 0.5;

        /*
         * World units per pixel at the segment's depth, read back from the
         * projection so any view works: Data[1][1] is the Y focal scale in
         * both MakeOrthographic and MakePerspective. An orthographic
         * projection has no w-divide (bottom row 0 0 0 1), so its scale is
         * the same at every depth; a perspective one grows with view depth.
         * The view's own row 2 is its forward axis (Matrix4x4::LookAt).
         */
        const EngineCore::Matrix4x4& Proj = View.Projection;
        const Vector3 Forward(View.View.Data[2][0], View.View.Data[2][1], View.View.Data[2][2]);
        const bool    bOrthographic = Proj.Data[3][2] == 0.0f && Proj.Data[3][3] != 0.0f;
        const double  FocalY        = std::abs(double(Proj.Data[1][1]));
        if(FocalY < 1e-12)
        {
            return;
        }
        double WorldPerPixel = 2.0 / (FocalY * double(View.ViewportPixels.Height));
        if(!bOrthographic)
        {
            const double Depth = (Mid - View.Origin).Dot(Forward);
            if(Depth <= double(View.ZNear))
            {
                return;   // behind or in the near plane -- clipped anyway
            }
            WorldPerPixel *= Depth;
        }
        const double Width = double(InThicknessPixels) * WorldPerPixel;

        /* The ribbon's width axis: across the segment and across the line of
         * sight, so the flat quad faces the camera. End-on, there is none. */
        const Vector3 Sight = bOrthographic ? Forward : (Mid - View.Origin);
        Vector3 Side = Along ^ Sight;
        const double SideLength = Side.GetLength();
        if(SideLength < 1e-9 * Sight.GetLength())
        {
            return;
        }
        Side = Side * (1.0 / SideLength);
        const Vector3 Normal = Along ^ Side;

        /* Unit quad (+-0.5 in X and Y, z = 0) -> the ribbon: X along the
         * segment (capped by half a width at each end), Y across it. Columns
         * are the images of the quad's axes; translation is the midpoint. */
        const Vector3 AxisX = Along * (Length + Width);
        const Vector3 AxisY = Side * Width;

        EngineCore::Matrix4x4 Segment;
        Segment.Data[0][0] = AxisX.GetX(); Segment.Data[0][1] = AxisY.GetX(); Segment.Data[0][2] = Normal.GetX(); Segment.Data[0][3] = Mid.GetX();
        Segment.Data[1][0] = AxisX.GetY(); Segment.Data[1][1] = AxisY.GetY(); Segment.Data[1][2] = Normal.GetY(); Segment.Data[1][3] = Mid.GetY();
        Segment.Data[2][0] = AxisX.GetZ(); Segment.Data[2][1] = AxisY.GetZ(); Segment.Data[2][2] = Normal.GetZ(); Segment.Data[2][3] = Mid.GetZ();
        Segment.Data[3][0] = 0.0f;         Segment.Data[3][1] = 0.0f;         Segment.Data[3][2] = 0.0f;          Segment.Data[3][3] = 1.0f;

        P->EnsureInitialised();
        if(!P->bShadersBound)
        {
            GDISetBoundShader(GDIGetShader<Basic_VS>(), GDIGetShader<Basic_PS>());
            P->bShadersBound = true;
        }

        /* The shader computes Projection * View * Model * v; with Model the
         * identity, View * Segment places the quad. Both uniforms are
         * uploaded per draw (GDIShader::UpdateCoreUniforms), so the next
         * DrawQuad's own View is unaffected. */
        P->View3D->Camera->Projection = View.Projection;
        P->View3D->Camera->ViewMat    = View.View * Segment;

        GDITransform* Transform = P->ViewObject->Transform;
        Transform->Translate = Vector3(0.0, 0.0, 0.0);
        Transform->Rotate    = Vector3(0.0, 0.0, 0.0);
        Transform->Scale     = Vector3(1.0, 1.0, 1.0);

        if(GDIShader* Pixel = GDIGetShader<Basic_PS>())
        {
            Pixel->SetUniform("TestColor", Vector3(InColor.Rf(), InColor.Gf(), InColor.Bf()));
        }

        GDIDrawObject(P->ViewObject);

        P->DrawCallCount.fetch_add(1, std::memory_order_relaxed);
    }
#endif // WITH_EDITOR

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
