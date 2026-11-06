#include "Public/RenderCore/ShaderCache.h"

#include "Public/Core/CoreArchive.h"

namespace RenderCore
{
    namespace
    {
        constexpr uint32_t TAG_HEADER = EngineCore::MakeArchiveTag('N','M','S','C');
        constexpr uint32_t TAG_BLOB   = EngineCore::MakeArchiveTag('B','L','O','B');
        constexpr uint32_t TAG_FILE   = EngineCore::MakeArchiveTag('F','I','L','E');
        constexpr uint32_t TAG_HASHED = EngineCore::MakeArchiveTag('F','H','S','H');

        uint64_t Fnv1a64(const std::vector<uint8_t>& InBytes)
        {
            uint64_t Hash = 0xcbf29ce484222325ull;
            for(const uint8_t Byte : InBytes)
            {
                Hash ^= Byte;
                Hash *= 0x100000001b3ull;
            }
            return Hash;
        }
    }

    bool ReadShaderCache(std::vector<uint8_t> InBytes, ShaderCacheContents& OutContents, std::string& OutError)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading(std::move(InBytes));
        ShaderCacheContents Result;

        uint16_t Version = 1;
        if(!Ar.OpenChunk(TAG_HEADER, Version))
        {
            OutError = "not a shader cache (no NMSC header)";
            return false;
        }
        Ar << Result.Module << Result.Platform << Result.TemplateVersion;
        Ar.CloseChunk();

        std::map<uint64_t, std::shared_ptr<const std::vector<uint8_t>>> Blobs;
        while(!Ar.HasError() && !Ar.IsAtEnd())
        {
            uint16_t ChunkVersion = 1;
            if(Ar.OpenChunk(TAG_BLOB, ChunkVersion))
            {
                uint64_t Hash = 0;
                uint32_t Size = 0;
                Ar << Hash << Size;
                if(!Ar.HasError() && Size > Ar.GetSize() - Ar.GetCursor())
                {
                    Ar.SetError("blob size runs past the end of the cache");
                }
                auto Bytes = std::make_shared<std::vector<uint8_t>>(Ar.HasError() ? 0 : Size);
                if(!Ar.HasError() && Size > 0) { Ar.SerializeBytes(Bytes->data(), Size); }
                Ar.CloseChunk();
                if(!Ar.HasError() && Fnv1a64(*Bytes) != Hash)
                {
                    OutError = "a shader in the cache is corrupt (hash mismatch)";
                    return false;
                }
                Blobs[Hash] = std::move(Bytes);
                continue;
            }
            if(Ar.OpenChunk(TAG_FILE, ChunkVersion))
            {
                std::string Path;
                uint64_t    Hash = 0;
                Ar << Path << Hash;
                Ar.CloseChunk();
                const auto Found = Blobs.find(Hash);
                if(!Ar.HasError() && Found == Blobs.end())
                {
                    OutError = "'" + Path + "' names content the cache does not hold";
                    return false;
                }
                if(!Ar.HasError()) { Result.Files[Path] = Found->second; }
                continue;
            }

            if(Ar.OpenChunk(TAG_HASHED, ChunkVersion))
            {
                uint64_t PathHash = 0;
                uint64_t Hash     = 0;
                Ar << PathHash << Hash;
                Ar.CloseChunk();
                const auto Found = Blobs.find(Hash);
                if(!Ar.HasError() && Found == Blobs.end())
                {
                    OutError = "a hashed path names content the cache does not hold";
                    return false;
                }
                if(!Ar.HasError()) { Result.HashedFiles[PathHash] = Found->second; }
                continue;
            }

            /* Not ours: skip it whole, as an older reader skips a newer chunk. */
            const uint32_t Unknown = Ar.PeekChunkTag();
            uint16_t Skipped = 0;
            if(Unknown == 0 || !Ar.OpenChunk(Unknown, Skipped))
            {
                Ar.SetError("unreadable chunk");
                break;
            }
            Ar.CloseChunk();
        }

        if(Ar.HasError())
        {
            OutError = "the cache is truncated or malformed";
            return false;
        }
        OutContents = std::move(Result);
        return true;
    }

    const char* GetShaderPlatformName(const ERenderBackend InBackend)
    {
        switch(InBackend)
        {
            case ERenderBackend::OpenGL: return "OpenGL";
            case ERenderBackend::Vulkan: return "Vulkan";
            case ERenderBackend::D3D12:  return "D3D12";
            case ERenderBackend::Metal:  return "Metal";
            case ERenderBackend::Null:   break;
        }
        return "";
    }

    std::string GetShaderCachePath(const std::string& InModule, const ERenderBackend InBackend)
    {
        return "Shaders/Cache/" + InModule + "." + GetShaderPlatformName(InBackend) + ".nmsc";
    }

    std::string GetShaderCacheLayerName(const std::string& InModule)
    {
        return "ShaderCache:" + InModule;
    }
}
