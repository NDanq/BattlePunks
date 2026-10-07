#ifndef RENDERCORE_ASSETCACHECOMMON_H
#define RENDERCORE_ASSETCACHECOMMON_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Public/RenderCore/RenderCoreLog.h"

/*
 * MeshAssetCache's shape, for the asset types that came after it (the
 * skeleton, clip and anim state caches): loaded files by path, shared by the
 * game and render threads, a failed load remembered (and warned once) until
 * Invalidate. Private to RenderCore.
 */
namespace RenderCore
{
    template<typename T>
    class TAssetCache
    {
    public:
        using LoadFn = bool (*)(const std::string& InPath, T& OutData);

        std::shared_ptr<const T> Find(const std::string& InPath, const LoadFn InLoad, const char* InWhat)
        {
            if(InPath.empty())
            {
                return nullptr;
            }
            {
                std::lock_guard<std::mutex> Guard(Lock);
                const auto Found = Items.find(InPath);
                if(Found != Items.end())
                {
                    return Found->second;
                }
            }

            /* Loaded outside the lock; if two threads race, the first stored wins. */
            auto Loaded = std::make_shared<T>();
            std::shared_ptr<const T> Result;
            if(InLoad(InPath, *Loaded))
            {
                Result = std::move(Loaded);
            }
            else
            {
                RENDERCORE_LOG_WARN(std::string(InWhat) + " '" + InPath + "' cannot be loaded")
            }

            std::lock_guard<std::mutex> Guard(Lock);
            return Items.emplace(InPath, Result).first->second;
        }

        void Invalidate(const std::string& InPath)
        {
            std::lock_guard<std::mutex> Guard(Lock);
            Generation.fetch_add(1, std::memory_order_relaxed);
            if(InPath.empty())
            {
                Items.clear();
            }
            else
            {
                Items.erase(InPath);
            }
        }

        uint64_t GetGeneration() const { return Generation.load(std::memory_order_relaxed); }

    private:
        std::mutex                                                Lock;
        std::unordered_map<std::string, std::shared_ptr<const T>> Items;   // null: failed
        std::atomic<uint64_t>                                     Generation{0};
    };
}

#endif //RENDERCORE_ASSETCACHECOMMON_H
