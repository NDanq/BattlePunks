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

bool IFileProvider::ReadRange(const std::string& InPath, const uint64_t InOffset, const uint64_t InSize, std::vector<uint8_t>& OutBytes)
{
    std::vector<uint8_t> All;
    if(!ReadAll(InPath, All) || InOffset > All.size() || InSize > All.size() - InOffset)
    {
        OutBytes.clear();
        return false;
    }
    OutBytes.assign(All.begin() + std::ptrdiff_t(InOffset), All.begin() + std::ptrdiff_t(InOffset + InSize));
    return true;
}

bool FilesystemProvider::ReadRange(const std::string& InPath, const uint64_t InOffset, const uint64_t InSize, std::vector<uint8_t>& OutBytes)
{
    return appFileReadRange(Resolve(InPath), InOffset, InSize, OutBytes);
}

bool FilesystemProvider::Exists(const std::string& InPath)
{
    return appFileExists(Resolve(InPath));
}

bool MemoryFileProvider::ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes)
{
    const auto Found = Files.find(InPath);
    if(Found == Files.end() || !Found->second)
    {
        return false;
    }
    OutBytes = *Found->second;
    return true;
}

bool MemoryFileProvider::Exists(const std::string& InPath)
{
    return Files.count(InPath) != 0;
}

std::vector<std::shared_ptr<IFileProvider>> LayeredFileProvider::Snapshot() const
{
    std::vector<std::shared_ptr<IFileProvider>> Order;
    std::lock_guard<std::mutex> Lock(Mutex);
    Order.reserve(Layers.size());
    for(auto It = Layers.rbegin(); It != Layers.rend(); ++It)
    {
        Order.push_back(It->Provider);
    }
    return Order;
}

bool LayeredFileProvider::ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes)
{
    if(bBaseFirst && Base && Base->ReadAll(InPath, OutBytes))
    {
        return true;
    }
    for(const std::shared_ptr<IFileProvider>& Each : Snapshot())
    {
        if(Each->ReadAll(InPath, OutBytes))
        {
            return true;
        }
    }
    return !bBaseFirst && Base && Base->ReadAll(InPath, OutBytes);
}

/* The first provider ReadAll would take the file from: the one that has it. */
bool LayeredFileProvider::ReadRange(const std::string& InPath, const uint64_t InOffset, const uint64_t InSize, std::vector<uint8_t>& OutBytes)
{
    if(bBaseFirst && Base && Base->Exists(InPath))
    {
        return Base->ReadRange(InPath, InOffset, InSize, OutBytes);
    }
    for(const std::shared_ptr<IFileProvider>& Each : Snapshot())
    {
        if(Each->Exists(InPath))
        {
            return Each->ReadRange(InPath, InOffset, InSize, OutBytes);
        }
    }
    if(!bBaseFirst && Base && Base->Exists(InPath))
    {
        return Base->ReadRange(InPath, InOffset, InSize, OutBytes);
    }
    OutBytes.clear();
    return false;
}

bool LayeredFileProvider::Exists(const std::string& InPath)
{
    if(Base && Base->Exists(InPath))
    {
        return true;
    }
    for(const std::shared_ptr<IFileProvider>& Each : Snapshot())
    {
        if(Each->Exists(InPath))
        {
            return true;
        }
    }
    return false;
}

bool LayeredFileProvider::MountLayer(const std::string& InName, std::shared_ptr<IFileProvider> InLayer)
{
    if(!InLayer)
    {
        return false;
    }
    std::lock_guard<std::mutex> Lock(Mutex);
    for(Layer& Each : Layers)
    {
        if(Each.Name == InName)
        {
            Each.Provider = std::move(InLayer);
            return true;
        }
    }
    Layers.push_back({ InName, std::move(InLayer) });
    return true;
}

bool LayeredFileProvider::UnmountLayer(const std::string& InName)
{
    std::lock_guard<std::mutex> Lock(Mutex);
    for(auto It = Layers.begin(); It != Layers.end(); ++It)
    {
        if(It->Name == InName)
        {
            Layers.erase(It);
            return true;
        }
    }
    return false;
}

std::vector<std::string> LayeredFileProvider::GetLayerNames() const
{
    std::lock_guard<std::mutex> Lock(Mutex);
    std::vector<std::string> Names;
    for(const Layer& Each : Layers)
    {
        Names.push_back(Each.Name);
    }
    return Names;
}

void SetFileProvider(IFileProvider* InProvider)
{
    GFileProvider = InProvider;
}

IFileProvider* GetFileProvider()
{
    return GFileProvider;
}

bool ReadFileRange(const std::string& InPath, const uint64_t InOffset, const uint64_t InSize, std::vector<uint8_t>& OutBytes)
{
    if(IFileProvider* Provider = GetFileProvider())
    {
        if(Provider->Exists(InPath))
        {
            return Provider->ReadRange(InPath, InOffset, InSize, OutBytes);
        }
    }
    return appFileReadRange(InPath, InOffset, InSize, OutBytes);
}
}
