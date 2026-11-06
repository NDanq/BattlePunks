#ifndef ENGINECORE_COREARCHIVE_H
#define ENGINECORE_COREARCHIVE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Public/Core/CoreAPI.h"

/*
 * Byte-level persistence: Archive, and the ISerializable interface every
 * serializable type implements.
 *
 * ONE CLASS WITH A DIRECTION, not a reader and a writer. With a Save/Load pair
 * a type states its field order twice and nothing checks the two agree; the
 * failure is a save that loads with every field shifted. Here operator<< reads
 * or writes depending on the direction, so the layout is stated once --
 * SetFromString's argument in CoreConfig.h.
 *
 * FIXED WIDTH, LITTLE ENDIAN -- never sizeof() of a host type. Vector3 is
 * float[4] under WITH_SIMD and Real[3] without, so a memcpy'd struct reads as
 * garbage in a differently-configured build. Types with variable layout
 * serialize their fields explicitly.
 *
 * ERRORS ARE STICKY: the first failure sets a flag and every later operation is
 * a no-op, so a Serialize body is a plain list of fields and the caller checks
 * HasError() once.
 *
 * Not thread safe; an Archive is a local owned by whoever is saving or loading.
 */

namespace EngineCore
{
    /* Four characters, so a hex dump of a save reads as text. Header-only and
     * not CORE_API, per CoreAPI.h. */
    constexpr uint32_t MakeArchiveTag(const char A, const char B, const char C, const char D)
    {
        return (uint32_t(uint8_t(A)) << 24) | (uint32_t(uint8_t(B)) << 16)
             | (uint32_t(uint8_t(C)) <<  8) |  uint32_t(uint8_t(D));
    }

    class CORE_API Archive
    {
    public:
        /* An empty archive that accumulates into its own buffer. */
        static Archive ForWriting();

        /* A cursor over InBytes, which it takes ownership of. Whole-buffer, not
         * streaming: every consumer already has the bytes in hand. */
        static Archive ForReading(std::vector<uint8_t> InBytes);

        bool IsLoading() const { return bLoading; }
        bool IsSaving()  const { return !bLoading; }

        /* True once anything has failed -- a read past the end, a chunk that
         * escapes its parent, a bad length prefix. Never cleared. */
        bool HasError() const { return bError; }

        /* Fails the archive with a logged reason; for a serializer that finds
         * impossible data or a version it cannot up-convert. */
        void SetError(const char* InReason);

        // === Scalars ==========================================================
        // One overload per width and no template: an operator<<(T&) would accept
        // size_t, long, or an enum whose size varies by compiler.

        Archive& operator<<(uint8_t&  InOutValue);
        Archive& operator<<(uint16_t& InOutValue);
        Archive& operator<<(uint32_t& InOutValue);
        Archive& operator<<(uint64_t& InOutValue);
        Archive& operator<<(int8_t&   InOutValue);
        Archive& operator<<(int16_t&  InOutValue);
        Archive& operator<<(int32_t&  InOutValue);
        Archive& operator<<(int64_t&  InOutValue);

        /* Bit-cast through the same-width integer, for byte order; every target
         * is IEEE-754. */
        Archive& operator<<(float&  InOutValue);
        Archive& operator<<(double& InOutValue);

        /* One byte. Normalised on load: a corrupt byte in a bool is UB later. */
        Archive& operator<<(bool& InOutValue);

        /* uint32_t length prefix, then the bytes, no NUL. A length past the end
         * of the buffer sets the error instead of allocating. */
        Archive& operator<<(std::string& InOutValue);

        /* A char[N] as a length-prefixed string. Truncates on load and always
         * NUL-terminates, so a bigger-buffer build's save still fits. */
        void SerializeFixedString(char* InOutBuffer, size_t InCapacity);

        /* Opaque payloads only (a nested archive, a hash) -- never a struct. */
        void SerializeBytes(void* InOutData, size_t InBytes);

        // === Chunks ===========================================================
        /*
         * A tagged, sized region: tag(4) + version(2) + size(4). The size is the
         * forward-compatibility story -- a reader that does not want a chunk, or
         * does not know it, steps over it.
         *
         * Saving: writes the header and back-patches the size at CloseChunk.
         * Loading: false WITHOUT consuming anything if the tag at the cursor
         * is not InTag, so optional chunks can be probed; otherwise reads the
         * version and moves to the payload.
         *
         * Every true return needs a CloseChunk(). On load, that skips whatever
         * the reader did not consume -- how an older build reads a newer chunk.
         */
        bool OpenChunk(uint32_t InTag, uint16_t& InOutVersion);
        void CloseChunk();

        /*
         * The innermost open chunk's version: on save the one being written, on
         * load the one the file carries. 0 outside any chunk. Lets one Serialize
         * body evolve in both directions:
         *
         *     Ar << OldField;
         *     if(Ar.GetChunkVersion() >= 2) { Ar << NewField; }
         *
         * Append new fields at the end: a newer save read by an older build is
         * then handled by CloseChunk skipping the fields it does not know.
         */
        uint16_t GetChunkVersion() const;

        /* The tag at the cursor, unconsumed. Zero at the end or on error. */
        uint32_t PeekChunkTag() const;

        /* At the end of the current chunk, or of the buffer outside one. */
        bool IsAtEnd() const;

        // === Buffer ===========================================================

        /* Saving: what to write to disk. Loading: the buffer being read. */
        const std::vector<uint8_t>& GetBytes() const { return Bytes; }

        size_t GetCursor() const { return Cursor; }
        size_t GetSize()   const { return Bytes.size(); }

    private:
        Archive() = default;

        /* One per open chunk. Saving uses SizeField to back-patch; loading uses
         * EndOffset to skip, and to stop a nested read escaping its parent. */
        struct ChunkFrame
        {
            uint32_t Tag       = 0;
            uint16_t Version   = 0;   // what GetChunkVersion() reports
            size_t   SizeField = 0;   // saving: where to back-patch
            size_t   PayloadAt = 0;
            size_t   EndOffset = 0;   // loading: one past the payload
        };

        bool CanRead(size_t InBytes) const;

        /* Where every scalar crosses the buffer, templated on unsigned width so
         * the byte-order code exists once. Signed types and floats are passed
         * through void*. Private, and only instantiated by the .cpp's own uses. */
        template<typename TUnsigned>
        void SerializeScalar(void* InOutValue);

        std::vector<uint8_t>    Bytes;
        std::vector<ChunkFrame> OpenChunks;
        size_t                  Cursor   = 0;
        bool                    bLoading = false;
        bool                    bError   = false;
    };

    /*
     * What every serializable type derives from: one virtual, both directions,
     * branching on Ar.IsLoading() only when the work genuinely differs. In Core
     * so NimbleEngine::GameState and a game's own types share it without
     * depending on each other.
     */
    class CORE_API ISerializable
    {
    public:
        virtual ~ISerializable() = default;

        virtual void Serialize(Archive& Ar) = 0;

        /* Bump when the fields change. The code that wraps this type writes it
         * as the chunk version, so Serialize reads it via Ar.GetChunkVersion(). */
        virtual uint16_t GetSerialVersion() const { return 1; }
    };

    // === Files ==============================================================
    /*
     * Writes through appFileOpenWrite/appFileWrite, never <fstream> (see
     * CoreConfig.h), creating missing parent directories. Not through
     * IFileProvider, which is a read-only asset seam -- a save is user data and
     * belongs in appGetAppDataDir. Refuses an archive that has already failed.
     */
    CORE_API bool SaveArchiveToFile(const std::string& InPath, const Archive& InArchive);

    /* Reads through GetFileProvider(), falling back to appFileReadAll, as
     * LoadConfigFile does. False with a warning if the file cannot be read --
     * no save yet is the normal first run. */
    CORE_API bool LoadArchiveFromFile(const std::string& InPath, Archive& OutArchive);
}

#endif //ENGINECORE_COREARCHIVE_H
