#include "Public/GDI/GDI.h"
#include "Public/GDI/GDIView.h"
#include "Public/GDI/Shaders/Classes/TestbedShaders.h"
#include "Public/GDI/Shaders/Classes/UIShaders.h"
#include "Public/GDI/Shaders/Classes/BrushShaders.h"

const float				HalfQuadVertData[18] = {	-0.5f, -0.5f, 0.0f,
                                                       -0.5f, 0.5f, 0.0f,
                                                       0.5f, 0.5f, 0.0f,
                                                       0.5f, 0.5f, 0.0f,
                                                       0.5f, -0.5f, 0.0f,
                                                       -0.5f, -0.5f, 0.0f };
const float				QuadTexCoordData[12] = {	0.0f, 0.0f,
                                                       0.0f, 1.0f,
                                                       1.0f, 1.0f,
                                                       1.0f, 1.0f,
                                                       1.0f, 0.0f,
                                                       0.0f, 0.0f };

GDITransform* GDITransform::IdentityTransform = nullptr;

GDIViewObject::GDIViewObject(class GDIView *InView)
        : View(InView),
        VS(nullptr),
        PS(nullptr)
{
    Transform = new GDITransform();
    {
        const int NumVerts = sizeof(HalfQuadVertData) / sizeof(HalfQuadVertData[0]);
        const int NumTexCoords = sizeof(QuadTexCoordData) / sizeof(QuadTexCoordData[0]);
        Mesh = GDICreateQuadMesh(HalfQuadVertData, QuadTexCoordData, NumVerts, NumTexCoords);
    }
}

void GDIViewObject::Init(class GDIView *InView)
{
    View = InView;
    VS = nullptr;
    PS = nullptr;
    Mesh = nullptr;
    Transform = new GDITransform();
}

void GDIView::AddViewObject(GDIShader* InVS, GDIShader* InPS)
{
    auto* ViewObject = new GDIViewObject(this);
    ViewObject->VS = InVS;
    ViewObject->PS = InPS;
    Objects.push_back(ViewObject);
}

GDICursorObject::GDICursorObject(GDIView *InView)
{
    Super::Init(InView);
    VS = GDIGetShader<Basic_VS>();
    PS = GDIGetShader<Cursor_PS>();
    Mesh = GDICreateQuadMesh(100, 100);
}

GDICursorObject::GDICursorObject( GDIView *InView, const float& InWidth, const float& InHeight )
{
    Super::Init(InView);
    VS = GDIGetShader<Basic_VS>();
    PS = GDIGetShader<Cursor_PS>();
    Mesh = GDICreateQuadMesh(InWidth, InHeight);
    Width = InWidth;
    Height = InHeight;
}

void GDICursorObject::Resize(const float &InWidth, const float &InHeight)
{
    delete Mesh;

    Mesh = GDICreateQuadMesh(InWidth, InHeight);
    Width = InWidth;
    Height = InHeight;
}

GDIBrushViewObject::GDIBrushViewObject( GDIView *InView ) {
    Super::Init(InView);
}

GDIBrushViewObject::GDIBrushViewObject(GDIView *InView, const int &InSize) :
Size(InSize)
{
    Super::Init(InView);
    VS = GDIGetShader<Basic_Instanced_VS>();
    PS = GDIGetShader<UberBrush_PS>();
    Mesh = GDICreateQuadMesh(Size, Size, true);
}

void GDIBrushViewObject::Resize(const int &NewSize)
{
    delete Mesh;

    Mesh = GDICreateQuadMesh(NewSize, NewSize, true);
    Size = NewSize;
}