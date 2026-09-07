#include "Public/Core/CoreFileProvider.h"

#include "Public/Core/CorePlatform.h"

namespace EngineCore
{
namespace
{
    IFileProvider* GFileProvider = nullptr;
}

bool IFileProvider::ReadAllText(const std::string& InPath, std::string& OutText)
{
    std::vector<uint8_t> Bytes;
    if(!ReadAll(InPath, Bytes))
    {
        return false;
    }

    OutText.assign(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
    return true;
}

std::string FilesystemProvider::Resolve(const std::string& InPath) const
{
    if(BaseDir.empty())
    {
        return InPath;
    }

    const char Last = BaseDir.back();
    if(Last == '/' || Last == '\\')
    {
        return BaseDir + InPath;
    }
    return BaseDir + "/" + InPath;
}

bool FilesystemProvider::ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes)
{
    return appFileReadAll(Resolve(InPath), OutBytes);
}

bool FilesystemProvider::Exists(const std::string& InPath)
{
    return appFileExists(Resolve(InPath));
}

void SetFileProvider(IFileProvider* InProvider)
{
    GFileProvider = InProvider;
}

IFileProvider* GetFileProvider()
{
    return GFileProvider;
}
}
