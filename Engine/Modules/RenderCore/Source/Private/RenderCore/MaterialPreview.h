#ifndef RENDERCORE_MATERIALPREVIEW_H
#define RENDERCORE_MATERIALPREVIEW_H

#if WITH_EDITOR

#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "Public/Core/CoreImage.h"

/*
 * EDITOR ONLY -- where Renderer::RequestMaterialPreview's results wait. The
 * render thread delivers (RenderScene::Render), the game thread takes
 * (Renderer::TakeMaterialPreview); an Image owns a buffer, so it cannot ride
 * in a render command's captures (RenderCommand.h) and crosses here instead,
 * under a lock. Owned by Renderer::Impl, which outlives every command that
 * could deliver into it (Renderer::Shutdown flushes the queue first).
 */
namespace RenderCore
{
    class MaterialPreviewMailbox
    {
    public:
        void Deliver(const uint64_t InRequest, EngineCore::Image InImage, std::string InError)
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Done[InRequest] = Entry{ std::move(InImage), std::move(InError) };
        }

        /* True once per request, when it is done; moves the result out. */
        bool Take(const uint64_t InRequest, EngineCore::Image& OutImage, std::string& OutError)
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            const auto Found = Done.find(InRequest);
            if(Found == Done.end()) { return false; }
            OutImage = std::move(Found->second.Pixels);
            OutError = std::move(Found->second.Error);
            Done.erase(Found);
            return true;
        }

    private:
        struct Entry
        {
            EngineCore::Image Pixels;
            std::string       Error;   // "" when Pixels is the preview
        };

        std::mutex                  Mutex;
        std::map<uint64_t, Entry>   Done;
    };
}

#endif // WITH_EDITOR

#endif //RENDERCORE_MATERIALPREVIEW_H
