#include "Public/RenderCore/CustomRenderPass.h"
#include "Private/RenderCore/CustomRenderPassInternal.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

/*
 * The registry of custom render passes, by World (RENDERPIPELINE.md, "Custom
 * render passes"). The game thread registers and extracts; the render thread
 * gathers a World's list each time it draws one of its views, keeping the
 * passes alive (shared) for that frame, and frees the GPU resources of the
 * ones unregistered. RenderPassContext's members are in RenderContext.cpp,
 * beside what they draw with.
 */
namespace RenderCore
{
    namespace
    {
        struct Registration
        {
            const void*     World = nullptr;
            CustomPassEntry Entry;
        };

        std::mutex                    GLock;
        std::vector<Registration>     GPasses;
        std::vector<RenderPassHandle> GRetired;
        RenderPassHandle              GNextHandle = 1;
    }

    const char* GetRenderHookName(const ERenderHook InHook)
    {
        static const char* const Names[size_t(ERenderHook::Count)] = {
            "BeforeScene", "AfterPrePass", "AfterBasePass", "AfterLighting",
            "AfterForward", "BeforePostProcess", "AfterToneMap", "AfterPostProcess", "Sky",
        };
        return InHook < ERenderHook::Count ? Names[size_t(InHook)] : "";
    }

    bool CustomRenderPass::ShouldRun(const RenderPassContext& InContext) const
    {
        return InContext.IsMainView() || InContext.GetHook() == ERenderHook::Sky;
    }

    RenderPassHandle RegisterCustomRenderPass(const void* InWorld, std::shared_ptr<CustomRenderPass> InPass,
                                              const ERenderHook InHook, const int32_t InOrder)
    {
        if(!InPass || InHook >= ERenderHook::Count)
        {
            return 0;
        }
        std::lock_guard<std::mutex> Lock(GLock);
        Registration Added;
        Added.World        = InWorld;
        Added.Entry.Pass   = std::move(InPass);
        Added.Entry.Handle = GNextHandle++;
        Added.Entry.Hook   = InHook;
        Added.Entry.Order  = InOrder;
        GPasses.push_back(std::move(Added));
        return GPasses.back().Entry.Handle;
    }

    bool UnregisterCustomRenderPass(const RenderPassHandle InHandle)
    {
        std::lock_guard<std::mutex> Lock(GLock);
        const auto Found = std::find_if(GPasses.begin(), GPasses.end(),
                                        [InHandle](const Registration& Each) { return Each.Entry.Handle == InHandle; });
        if(Found == GPasses.end())
        {
            return false;
        }
        GRetired.push_back(InHandle);
        GPasses.erase(Found);
        return true;
    }

    void UnregisterCustomRenderPasses(const void* InWorld)
    {
        std::lock_guard<std::mutex> Lock(GLock);
        for(const Registration& Each : GPasses)
        {
            if(Each.World == InWorld) { GRetired.push_back(Each.Entry.Handle); }
        }
        GPasses.erase(std::remove_if(GPasses.begin(), GPasses.end(),
                                     [InWorld](const Registration& Each) { return Each.World == InWorld; }),
                      GPasses.end());
    }

    size_t GetCustomRenderPassCount(const void* InWorld)
    {
        std::lock_guard<std::mutex> Lock(GLock);
        return size_t(std::count_if(GPasses.begin(), GPasses.end(), [InWorld](const Registration& Each) { return Each.World == InWorld; }));
    }

    void GatherCustomRenderPasses(const void* InWorld, std::vector<CustomPassEntry>& OutPasses)
    {
        OutPasses.clear();
        if(!InWorld)
        {
            return;
        }
        {
            std::lock_guard<std::mutex> Lock(GLock);
            for(const Registration& Each : GPasses)
            {
                if(Each.World == InWorld) { OutPasses.push_back(Each.Entry); }
            }
        }
        /* Registration order is handle order: the last key keeps it. */
        std::sort(OutPasses.begin(), OutPasses.end(), [](const CustomPassEntry& InA, const CustomPassEntry& InB)
        {
            if(InA.Hook != InB.Hook)   { return InA.Hook < InB.Hook; }
            if(InA.Order != InB.Order) { return InA.Order < InB.Order; }
            return InA.Handle < InB.Handle;
        });
    }

    void ExtractCustomRenderPasses(const void* InWorld)
    {
        /* Outside the lock: a pass's Extract may register or unregister. */
        std::vector<CustomPassEntry> Passes;
        GatherCustomRenderPasses(InWorld, Passes);
        for(const CustomPassEntry& Each : Passes)
        {
            Each.Pass->Extract();
        }
    }

    void TakeRetiredCustomRenderPasses(std::vector<RenderPassHandle>& OutHandles)
    {
        std::lock_guard<std::mutex> Lock(GLock);
        OutHandles.swap(GRetired);
        GRetired.clear();
    }
}
