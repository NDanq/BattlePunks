#ifndef NIMBLEEDITOR_RENDERTARGETASSETS_H
#define NIMBLEEDITOR_RENDERTARGETASSETS_H

#if WITH_EDITOR

#include <functional>

#include <QObject>
#include <QString>

#include "Public/Core/CoreImage.h"
#include "Public/RenderCore/RenderTargetAsset.h"

class QWidget;
class EditorDocument;
class EditorContext;

/*
 * Render target assets in the editor (RenderCore's RenderTargetAsset.h):
 * RenderTargetTex2D (.nmrt2d) and RenderTargetTex3D (.nmrt3d) files made
 * from the Content Browser, their settings edited in a dialog, and a probe's
 * capture baked into them.
 */
namespace EditorRenderTargets
{
    /* Asks for a name and writes a new target of InKind in InFolder (default
     * settings), staged. Its path; empty if cancelled or it failed (said so). */
    QString CreateRenderTarget(const QString& InFolder, RenderCore::ERenderTargetKind InKind, QWidget* InParent);

    /* The target at InPath's settings -- size, filter, wrap, clear colour --
     * and its bake (shown, and clearable), in a dialog; saved and staged on OK. */
    void EditRenderTarget(const QString& InPath, QWidget* InParent);

    /* InPixels stored as InPath's bake (a probe's capture, read back), staged.
     * False, with OutError, if the file cannot be read or written or the
     * pixels do not fit it. */
    bool BakeRenderTarget(const QString& InPath, const EngineCore::Image& InPixels, QString& OutError);

    /*
     * World > Bake Scene Captures: every Scene Capture Probe in InDocument's
     * World captures on the next frame, and each one's target (a file; a
     * transient one is skipped) stores what it shows as its bake. InDone gets
     * a line for the status bar once every readback has come back.
     */
    void BakeSceneCaptures(EditorDocument& InDocument, EditorContext& InContext, QObject* InOwner,
                           std::function<void(const QString&)> InDone);

    /* "512 x 512 RenderTargetTex2D, baked" -- the Content Browser's summary. */
    QString DescribeRenderTarget(const QString& InPath);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_RENDERTARGETASSETS_H
