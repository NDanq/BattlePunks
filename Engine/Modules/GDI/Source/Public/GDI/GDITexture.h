#ifndef CORECLIENT_GDITEXTURE_H
#define CORECLIENT_GDITEXTURE_H

#include "Public/Core/CoreImage.h"

#include "GDIDefinitions.h"

class GDITexture2D;
class GDITextureResource;

/* A texture resource is going away: the driver forgets it wherever it holds
 * it for a later draw (GDIDynamicDriver::ForgetTextureResource), so a texture
 * freed between draws never reaches a backend as a dangling material slot.
 * Called by ~GDITextureResource. */
void GDIForgetTextureResource(const GDITextureResource* InTexture);

/*
 * ORIENTATION AND FORMAT -- read before changing anything in this file.
 *
 * The pixel FORMAT differs per backend and must not be unified: OpenGL works
 * in BGRA8 (what Qt calls QImage::Format_ARGB32 -- that name describes the packed
 * integer, not the byte order), while Vulkan and Metal work in RGBA8. Image
 * carries its format so each backend keeps its own, and the Qt bridge maps
 * them back faithfully. Forcing one format swaps red and blue on the other.
 *
 * The ORIENTATION contract is asymmetric and deliberately so:
 *
 *   - Constructing a GDITexture2D from an Image applies a HORIZONTAL flip.
 *   - UpdateTexture applies NO flip.
 *
 * That asymmetry is relied upon by every persistent-texture path in the canvas
 * renderer, which compensates by hand. Do not "tidy" the two into one
 * convention here; that is a separate behaviour-changing refactor needing its
 * own test pass.
 */

class GDITextureResource {
public:
    GDITextureResource() = default;

    /* Virtual: owners delete through this base. Each backend's destructor
     * calls its own Release(), which is safe after an explicit one. */
    virtual ~GDITextureResource() { GDIForgetTextureResource(this); }

    explicit GDITextureResource(const EngineCore::Image& InImage, const uint InIdx = 0, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest):
    bInitialized(false){
        Width = InImage.GetWidth();
        Height = InImage.GetHeight();
    }

    explicit  GDITextureResource(GDITexture2D* InParent, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest){}

    explicit GDITextureResource(GDITexture2D* InParent, const EngineCore::Image& InImage, const uint InIdx = 0, const EGDITextureBorder InBorder = GDI_TB_Repeat, const EGDITextureFilter InFilter = GDI_TF_Nearest);

    virtual void Update(const EngineCore::Image& InImage) = 0;

    virtual void Release() = 0;

    /* Host readback. Format is the backend's own -- BGRA8 for OpenGL, RGBA8
     * for Vulkan and Metal. */
    virtual EngineCore::Image ToImage() const = 0;

    /* Its name in capture tools (GDISetDebugName). */
    virtual void SetDebugName(const std::string& InName) { DebugName = InName; }
    const std::string& GetDebugName() const { return DebugName; }
protected:
    std::string   DebugName;
    GDITexture2D* Parent;
    bool bInitialized = false;
    int Width = 0;
    int Height = 0;
};

class GDITexture2D {
public:
    GDITexture2D()
            : Width(0),
              Height(0),
              TextureType(EGDITextureType::GDI_TT_QtNative),
              Resource(nullptr) {}

    GDITexture2D(const uint InWidth, const uint InHeight, void **Data)
            : Width(InWidth),
              Height(InHeight),
              TextureType(EGDITextureType::GDI_TT_QtNative),
              Resource(nullptr) {
        InitResourceNoData();
    }

    /*
     * Applies a HORIZONTAL flip on upload. UpdateTexture deliberately does
     * not -- see the note at the top of this file.
     */
    explicit GDITexture2D(const EngineCore::Image &InImage) :
            Resource(nullptr) {
        Width = uint(InImage.GetWidth());
        Height = uint(InImage.GetHeight());
        TextureType = GDI_TT_QtNative;
        Image = InImage;
        Image.FlipHorizontal();
        InitResource();
    }

    void InitResource();

    void InitResourceNoData();

    /* No implicit flip. Callers that need the constructor's horizontal mirror
     * fold it in themselves. */
    void UpdateTexture(const EngineCore::Image& InImage);

    void Release();

    EngineCore::Image ToImage();

    uint Width;
    uint Height;
    EGDITextureType TextureType;
    GDITextureResource* Resource;
    EngineCore::Image Image;
};

#endif //CORECLIENT_GDITEXTURE_H
