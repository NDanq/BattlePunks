#ifndef UI_UIINTERNAL_H
#define UI_UIINTERNAL_H

#include <string>

#include "Public/UI/UITypes.h"

/* Shared inside the UI module only. */
namespace UI
{
    /* InPath's size in pixels (TextureAssetCache), remembered; 0,0 if it does not load. */
    UIVec2 GetTextureSize(const std::string& InPath);

    /* InA and InB's overlap as a clip; none and none is none. */
    RenderCore::UIClipRect IntersectClip(const RenderCore::UIClipRect& InA, const RenderCore::UIClipRect& InB);

    inline bool Contains(const UIRect& InRect, const UIVec2& InPoint)
    {
        return InPoint.X >= InRect.X && InPoint.Y >= InRect.Y && InPoint.X < InRect.X + InRect.W && InPoint.Y < InRect.Y + InRect.H;
    }

    inline bool Contains(const RenderCore::UIClipRect& InClip, const UIVec2& InPoint)
    {
        return InClip.IsNone() || (InPoint.X >= float(InClip.X) && InPoint.Y >= float(InClip.Y)
                                   && InPoint.X < float(InClip.X + InClip.W) && InPoint.Y < float(InClip.Y + InClip.H));
    }
}

#endif //UI_UIINTERNAL_H
