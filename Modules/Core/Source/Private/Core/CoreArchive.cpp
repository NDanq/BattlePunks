#include "Public/Core/CoreArchive.h"

#include <cstring>

#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"

/*
 * Chunk sizes are back-patched at CloseChunk rather than built in a scratch
 * archive and copied into the parent, which would copy every payload once per
 * nesting level.
 */

namespace EngineCore
{
namespace
{
    /* tag(4) + version(2) + size(4). ArchiveTextDump.cpp mirrors this. */
    constexpr size_t CHUNK_HEADER_BYTES = 4 + 2 + 4;

    /* Rejects a corrupt length prefix before it can become an allocation.
     * Nothing legitimate is near this long. */
    constexpr uint32_t STRING_LENGTH_SANITY_MAX = 1u << 24;   // 16 MiB
}

Archive Archive::ForWriting()
{
    Archive Ar;
    Ar.bLoading = false;
    return Ar;
}

Archive Archive::ForReading(std::vector<uint8_t> InBytes)
{
    Archive Ar;
    Ar.bLoading = true;
    Ar.Bytes    = std::move(InBytes);
    return Ar;
}

void Archive::SetError(const char* InReason)
{
    /* Log the first failure only; the rest are consequences of it. */
    if(!bError)
    {
        bError = true;
        CORE_LOG_ERROR(std::string("Archive: ") + (InReason ? InReason : "unspecified failure"))
    }
}

bool Archive::CanRead(const size_t InBytes) const
{
    if(bError) { return false; }

    /* Bounded by the innermost chunk, not the buffer, so an overlong nested
     * read cannot eat its parent's next fields. */
    const size_t Limit = OpenChunks.empty() ? Bytes.size() : OpenChunks.back().EndOffset;
    return Cursor + InBytes <= Limit;
}

// === Scalars ================================================================

namespace
{
    /* Little-endian a byte at a time: correct on any host with no #ifdef, and
     * at most eight iterations per field. */
    template<typename TUnsigned>
    void PutLittleEndian(std::vector<uint8_t>& OutBytes, TUnsigned InValue)
    {
        for(size_t Index = 0; Index < sizeof(TUnsigned); ++Index)
        {
            OutBytes.push_back(uint8_t((InValue >> (8 * Index)) & 0xFFu));
        }
    }

    template<typename TUnsigned>
    TUnsigned GetLittleEndian(const std::vector<uint8_t>& InBytes, const size_t InAt)
    {
        TUnsigned Value = 0;
        for(size_t Index = 0; Index < sizeof(TUnsigned); ++Index)
        {
            Value |= TUnsigned(InBytes[InAt + Index]) << (8 * Index);
        }
        return Value;
    }
}

/* Signed types reach this as the same-width unsigned, which round-trips under
 * two's complement (mandated by C++20, and what every supported compiler uses). */
template<typename TUnsigned>
void Archive::SerializeScalar(void* InOutValue)
{
    if(bError) { return; }

    if(bLoading)
    {
        if(!CanRead(sizeof(TUnsigned)))
        {
            SetError("read past the end of the buffer");
            return;
        }
        const TUnsigned Value = GetLittleEndian<TUnsigned>(Bytes, Cursor);
        Cursor += sizeof(TUnsigned);
        std::memcpy(InOutValue, &Value, sizeof(TUnsigned));
    }
    else
    {
        TUnsigned Value;
        std::memcpy(&Value, InOutValue, sizeof(TUnsigned));
        PutLittleEndian<TUnsigned>(Bytes, Value);
    }
}

Archive& Archive::operator<<(uint8_t&  InOutValue) { SerializeScalar<uint8_t> (&InOutValue); return *this; }
Archive& Archive::operator<<(uint16_t& InOutValue) { SerializeScalar<uint16_t>(&InOutValue); return *this; }
Archive& Archive::operator<<(uint32_t& InOutValue) { SerializeScalar<uint32_t>(&InOutValue); return *this; }
Archive& Archive::operator<<(uint64_t& InOutValue) { SerializeScalar<uint64_t>(&InOutValue); return *this; }
Archive& Archive::operator<<(int8_t&   InOutValue) { SerializeScalar<uint8_t> (&InOutValue); return *this; }
Archive& Archive::operator<<(int16_t&  InOutValue) { SerializeScalar<uint16_t>(&InOutValue); return *this; }
Archive& Archive::operator<<(int32_t&  InOutValue) { SerializeScalar<uint32_t>(&InOutValue); return *this; }
Archive& Archive::operator<<(int64_t&  InOutValue) { SerializeScalar<uint64_t>(&InOutValue); return *this; }

/* Same byte order as the integers. The static_asserts make an unusual float
 * width a build error rather than silent truncation. */
Archive& Archive::operator<<(float& InOutValue)
{
    static_assert(sizeof(float) == 4, "Archive assumes a 4-byte IEEE-754 float");
    SerializeScalar<uint32_t>(&InOutValue);
    return *this;
}

Archive& Archive::operator<<(double& InOutValue)
{
    static_assert(sizeof(double) == 8, "Archive assumes an 8-byte IEEE-754 double");
    SerializeScalar<uint64_t>(&InOutValue);
    return *this;
}

Archive& Archive::operator<<(bool& InOutValue)
{
    uint8_t Byte = InOutValue ? 1u : 0u;
    SerializeScalar<uint8_t>(&Byte);

    /* Normalised: a raw corrupt byte would be a bool that is neither value. */
    if(bLoading && !bError) { InOutValue = (Byte != 0); }
    return *this;
}

Archive& Archive::operator<<(std::string& InOutValue)
{
    if(bError) { return *this; }

    uint32_t Length = uint32_t(InOutValue.size());
    SerializeScalar<uint32_t>(&Length);
    if(bError) { return *this; }

    if(bLoading)
    {
        /* Checked before assigning, so a corrupt prefix never allocates. */
        if(Length > STRING_LENGTH_SANITY_MAX)
        {
            SetError("string length prefix is implausibly large");
            return *this;
        }
        if(!CanRead(Length))
        {
            SetError("string length prefix runs past the end of the buffer");
            return *this;
        }

        InOutValue.assign(reinterpret_cast<const char*>(Bytes.data() + Cursor), Length);
        Cursor += Length;
    }
    else if(Length > 0)
    {
        Bytes.insert(Bytes.end(), InOutValue.begin(), InOutValue.end());
    }

    return *this;
}

void Archive::SerializeFixedString(char* InOutBuffer, const size_t InCapacity)
{
    if(bError) { return; }

    CORE_CHECK(InOutBuffer != nullptr && InCapacity > 0,
               "Archive::SerializeFixedString needs a real buffer")
    if(InOutBuffer == nullptr || InCapacity == 0) { return; }

    if(bLoading)
    {
        std::string Text;
        *this << Text;
        if(bError) { return; }

        /* Truncate rather than overrun, for saves from larger-buffer builds. */
        const size_t Copy = (Text.size() < InCapacity - 1) ? Text.size() : InCapacity - 1;
        if(Copy < Text.size())
        {
            CORE_LOG_WARN("Archive: truncating a stored string of " + std::to_string(Text.size())
                          + " bytes into a " + std::to_string(InCapacity) + "-byte buffer")
        }
        std::memcpy(InOutBuffer, Text.data(), Copy);
        std::memset(InOutBuffer + Copy, 0, InCapacity - Copy);
    }
    else
    {
        /* Bounded like strnlen, so an unterminated buffer is not read past. */
        size_t Length = 0;
        while(Length < InCapacity && InOutBuffer[Length] != '\0') { ++Length; }

        std::string Text(InOutBuffer, Length);
        *this << Text;
    }
}

void Archive::SerializeBytes(void* InOutData, const size_t InBytes)
{
    if(bError || InBytes == 0) { return; }

    CORE_CHECK(InOutData != nullptr, "Archive::SerializeBytes needs a real buffer")
    if(InOutData == nullptr) { return; }

    if(bLoading)
    {
        if(!CanRead(InBytes))
        {
            SetError("byte block runs past the end of the buffer");
            return;
        }
        std::memcpy(InOutData, Bytes.data() + Cursor, InBytes);
        Cursor += InBytes;
    }
    else
    {
        const uint8_t* Src = static_cast<const uint8_t*>(InOutData);
        Bytes.insert(Bytes.end(), Src, Src + InBytes);
    }
}

// === Chunks =================================================================

bool Archive::OpenChunk(const uint32_t InTag, uint16_t& InOutVersion)
{
    if(bError) { return false; }

    ChunkFrame Frame;
    Frame.Tag     = InTag;
    Frame.Version = InOutVersion;   // saving; the loading path overwrites it with the file's

    if(bLoading)
    {
        if(!CanRead(CHUNK_HEADER_BYTES))
        {
            /* Not an error: probing past the last chunk is how a
             * while(OpenChunk(...)) loop ends. */
            return false;
        }

        const uint32_t FoundTag = GetLittleEndian<uint32_t>(Bytes, Cursor);
        if(FoundTag != InTag)
        {
            /* Cursor unmoved, so the caller can probe for another tag. */
            return false;
        }

        const uint16_t Version = GetLittleEndian<uint16_t>(Bytes, Cursor + 4);
        const uint32_t Size    = GetLittleEndian<uint32_t>(Bytes, Cursor + 6);

        Frame.PayloadAt = Cursor + CHUNK_HEADER_BYTES;
        Frame.EndOffset = Frame.PayloadAt + Size;

        /* Caught here, or CloseChunk would jump past the parent's end. */
        const size_t Limit = OpenChunks.empty() ? Bytes.size() : OpenChunks.back().EndOffset;
        if(Frame.EndOffset > Limit)
        {
            SetError("chunk size runs past the end of its parent");
            return false;
        }

        Frame.Version = Version;
        InOutVersion  = Version;
        Cursor        = Frame.PayloadAt;
        OpenChunks.push_back(Frame);
        return true;
    }

    uint32_t Tag     = InTag;
    uint16_t Version = InOutVersion;
    SerializeScalar<uint32_t>(&Tag);
    SerializeScalar<uint16_t>(&Version);

    /* Reserved now, back-patched at CloseChunk. */
    Frame.SizeField = Bytes.size();
    uint32_t SizePlaceholder = 0;
    SerializeScalar<uint32_t>(&SizePlaceholder);

    Frame.PayloadAt = Bytes.size();
    OpenChunks.push_back(Frame);
    return true;
}

void Archive::CloseChunk()
{
    CORE_CHECK(!OpenChunks.empty(), "Archive::CloseChunk with no chunk open")
    if(OpenChunks.empty()) { return; }

    const ChunkFrame Frame = OpenChunks.back();
    OpenChunks.pop_back();

    if(bLoading)
    {
        /* Skip what the reader did not consume -- the forward-compatibility
         * mechanism: an older build reads the fields it knows and still lands
         * on the next chunk. */
        if(!bError) { Cursor = Frame.EndOffset; }
        return;
    }

    if(bError) { return; }

    const size_t PayloadSize = Bytes.size() - Frame.PayloadAt;
    if(PayloadSize > 0xFFFFFFFFull)
    {
        SetError("chunk payload exceeds the 4 GiB its size field can express");
        return;
    }

    const uint32_t Size = uint32_t(PayloadSize);
    for(size_t Index = 0; Index < 4; ++Index)
    {
        Bytes[Frame.SizeField + Index] = uint8_t((Size >> (8 * Index)) & 0xFFu);
    }
}

uint16_t Archive::GetChunkVersion() const
{
    return OpenChunks.empty() ? 0 : OpenChunks.back().Version;
}

uint32_t Archive::PeekChunkTag() const
{
    if(bError || !CanRead(CHUNK_HEADER_BYTES)) { return 0; }
    return GetLittleEndian<uint32_t>(Bytes, Cursor);
}

bool Archive::IsAtEnd() const
{
    if(bError) { return true; }
    const size_t Limit = OpenChunks.empty() ? Bytes.size() : OpenChunks.back().EndOffset;
    return Cursor >= Limit;
}

// === Files ==================================================================

namespace
{
    /* The directory part of a path, or "". Both separators on every platform:
     * this engine writes forward slashes, and Windows accepts them. */
    std::string ParentDirectoryOf(const std::string& InPath)
    {
        const size_t Slash = InPath.find_last_of("/\\");
        return (Slash == std::string::npos) ? std::string() : InPath.substr(0, Slash);
    }
}

bool SaveArchiveToFile(const std::string& InPath, const Archive& InArchive)
{
    if(InArchive.IsLoading())
    {
        CORE_LOG_ERROR("SaveArchiveToFile: the archive is open for reading, not writing")
        return false;
    }
    if(InArchive.HasError())
    {
        /* A half-built save on disk is worse than none. */
        CORE_LOG_ERROR("SaveArchiveToFile: refusing to write an archive that failed while building -- " + InPath)
        return false;
    }

    const std::string Parent = ParentDirectoryOf(InPath);
    if(!Parent.empty() && !appCreateDirectories(Parent))
    {
        CORE_LOG_ERROR("SaveArchiveToFile: could not create the directory " + Parent)
        return false;
    }

    FileHandle Handle = appFileOpenWrite(InPath);
    if(Handle == nullptr)
    {
        CORE_LOG_ERROR("SaveArchiveToFile: could not open " + InPath + " for writing")
        return false;
    }

    const std::vector<uint8_t>& Bytes = InArchive.GetBytes();
    const size_t Written = Bytes.empty() ? 0 : appFileWrite(Handle, Bytes.data(), Bytes.size());
    appFileClose(Handle);

    if(Written != Bytes.size())
    {
        CORE_LOG_ERROR("SaveArchiveToFile: wrote " + std::to_string(Written) + " of "
                       + std::to_string(Bytes.size()) + " bytes to " + InPath)
        return false;
    }

    return true;
}

bool LoadArchiveFromFile(const std::string& InPath, Archive& OutArchive)
{
    std::vector<uint8_t> Bytes;

    /* Provider first, then the platform layer, as LoadConfigFile does. */
    bool bRead = false;
    if(IFileProvider* Provider = GetFileProvider())
    {
        if(Provider->Exists(InPath))
        {
            bRead = Provider->ReadAll(InPath, Bytes);
        }
    }

    if(!bRead && appFileExists(InPath))
    {
        bRead = appFileReadAll(InPath, Bytes);
    }

    if(!bRead)
    {
        /* A warning: no save yet is the normal first run. */
        CORE_LOG_WARN("LoadArchiveFromFile: could not read " + InPath)
        return false;
    }

    OutArchive = Archive::ForReading(std::move(Bytes));
    return true;
}
}
