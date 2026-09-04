#ifndef CORECLIENT_GDIVIEW_H
#define CORECLIENT_GDIVIEW_H


#include "Public/Math/Vector.h"
#include "Public/Math/Matrix.h"
#include <vector>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.


class GDITransform
{
public:
    Vector3 Translate;
    Vector3 Rotate;
    Vector3 Scale = Vector3(1, 1, 1);
    float TransformScale = 1.0f;

    const Matrix4x4 GetModel() const
    {
        auto AdjustedTranslate = Vector3(Translate.GetX() * TransformScale,
                                         Translate.GetY() * TransformScale,
                                         Translate.GetZ() * TransformScale);
        Matrix4x4 Translation = Matrix4x4().InitTranslation(AdjustedTranslate );
        Matrix4x4 Rotation = Matrix4x4().InitRotation(Rotate.GetZ(), Vector3(0, 0, 1))
                * Matrix4x4().InitRotation(Rotate.GetY(), Vector3(0, 1, 0))
                * Matrix4x4().InitRotation(Rotate.GetX(), Vector3(1, 0, 0));
        Matrix4x4 ScaleMat = Matrix4x4().InitScale(Scale);
        return ScaleMat * Rotation * Translation;
        //return Translation;
    }
    static GDITransform* Identity()
    {
        if(IdentityTransform == nullptr)
        {
            IdentityTransform = new GDITransform;
        }
        return IdentityTransform;
    }
protected:
    static GDITransform* IdentityTransform;
};

class GDICamera
{
public:
    GDICamera()
    {
        Transform = new GDITransform;
        Transform->Translate = Vector3(0, 1, -1);
        Transform->Scale = Vector3(1, 1, 1);
        ZNear = -1.f;
        ZFar = 1000.f;
    }

    void UpdateDimensions(float NewWidth, float NewHeight, float NewZNear = -1.0f, float NewZFar = -1.0f )
    {
        ZNear = (NewZNear == -1.f) ? ZNear : NewZNear;
        ZFar = (NewZFar == -1.f) ? ZFar : NewZFar;
        Width = NewWidth;
        Height = NewHeight;
        Projection = Matrix4x4().InitOrthographicView(0, Height, 0, Width, ZNear, ZFar);
        ViewMat = Matrix4x4().LookAt(Transform->Translate, Vector3(0, 0, 1), Vector3(0, 1, 0));
        Matrix4x4 Translate = Matrix4x4().InitTranslation(Transform->Translate);
        CameraProjection = Projection * ViewMat * Translate;
    }

    [[nodiscard]]
    const Matrix4x4 GetViewProjectionMat() const
    {
        return Projection * ViewMat;
    }

    float ZNear;
    float ZFar;
    float Width;
    float Height;

    Matrix4x4 CameraProjection;
    Matrix4x4 Projection;
    Matrix4x4 ViewMat;
    GDITransform* Transform;
};

class GDIViewObject
{
public:
    GDIViewObject() = default;
    GDIViewObject(class GDIView* InView);

    void Init(class GDIView* InView);

    class GDIMesh*          Mesh;
    class GDIView*          View;
    GDITransform*           Transform;
    GDIShader*              VS;
    GDIShader*              PS;
    EngineCore::Rect             Bounds;
    bool                    bInitialized = false;
protected:
    typedef GDIViewObject Super;
};

class GDICursorObject : public GDIViewObject
{
public:
    GDICursorObject(class GDIView* InView);
    GDICursorObject(class GDIView* InView, const float& InWidth, const float& InHeight);
    void Resize(const float& InWidth, const float& InHeight);

    float Width;
    float Height;
};

class GDIBrushViewObject : public GDIViewObject
{
public:
    GDIBrushViewObject(class GDIView* InView);
    GDIBrushViewObject(class GDIView* InView, const int& InSize);
    void Resize(const int& NewSize);
    int Size;
};

class GDIView
{
public:
    struct GDIMouseState
    {
        int     PosX = 0;
        int     PosY = 0;
        bool    bMouseDown = false;
    } MouseState;

    struct GDIViewportData
    {
        uint ViewportWidth = 0;
        uint ViewportHeight = 0;
        EngineCore::Real ScaleFactor = 1.0;
        uint XOffset = 0;
        uint YOffset = 0;
    } Viewport;

    [[deprecated]]
    struct GDIProjectInfo
    {
        uint    WidthPX = 0;
        uint    HeightPX = 0;
        uint    DPI = 300;
        EngineCore::Rect   Bounds;
        EngineCore::Color  ClearColor;
    } ProjectInfo;

    GDIView()
    {
        Camera = new GDICamera();
    }

    GDIView(const uint InWidth, const uint InHeight, const float& InDevicePixelRatio = 1.0f) :
    DevicePixelRatio(InDevicePixelRatio)
    {
        Camera = new GDICamera();
        UpdateViewport(InWidth, InHeight);
    }

    ~GDIView()
    {
        delete Camera;
    }

     void AddViewObject(GDIShader* InVS, GDIShader* InPS);
     void DrawViewObject();

    void UpdateViewport(uint NewViewportWidth, uint NewViewportHeight)
    {
        Viewport.ViewportWidth = NewViewportWidth;
        Viewport.ViewportHeight = NewViewportHeight;
        Camera->UpdateDimensions(Viewport.ViewportWidth, Viewport.ViewportHeight);
    }

    float DevicePixelRatio = 1.0f;

    std::vector<GDIViewObject*> Objects;
    GDICamera* Camera;
};

#endif //CORECLIENT_GDIVIEW_H
