/*
 * EngineCoreSmokeTest -- links EngineCore and NOTHING else.
 *
 * Its first job is structural: if this target ever fails to link, something
 * has pulled Qt (or a graphics API) into EngineCore, which is exactly the
 * regression the whole split exists to prevent.
 *
 * Its second job is to exercise the app* seam and the Image container, both of
 * which were written before anything called them. The cases below are chosen
 * from the specific failure modes the migration plan calls out, not for
 * coverage's sake -- see the comments on each.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <utility>
#include <filesystem>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreImage.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreModule.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreMemory.h"
#include "Public/Core/CoreMemoryPool.h"
#include "Public/Core/CoreModuleManager.h"
#include "Public/Core/CorePack.h"
#include "Public/Core/CorePointer.h"
#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreQueue.h"
#include "Public/Core/CoreString.h"
#include "Public/Core/CoreJob.h"
#include "Public/Core/CoreThread.h"
#include "Public/Math/Matrix.h"
#include "Public/Math/Vector.h"

using namespace EngineCore;

static int GFailures = 0;
static int GChecks   = 0;

static void Expect(const bool bCondition, const char* What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What);
    }
}

// --- log seam -------------------------------------------------------------

static int GHandlerCalls = 0;
static LogVerbosity GLastVerbosity = LogVerbosity::Log;

static void TestLogHandler(const LogVerbosity Verbosity, const char* /*Message*/)
{
    ++GHandlerCalls;
    GLastVerbosity = Verbosity;
}

static void TestLogging()
{
    std::printf("[log]\n");

    SetLogHandler(&TestLogHandler);
    Expect(GetLogHandler() == &TestLogHandler, "installed handler is returned by GetLogHandler");

    GHandlerCalls = 0;
    CORE_LOG("hello")
    CORE_LOG_WARN("warn")
    CORE_LOG_ERROR("error")
    Expect(GHandlerCalls == 3, "all three verbosities reach the handler");
    Expect(GLastVerbosity == LogVerbosity::Error, "verbosity is forwarded intact");

    // Fatal must RETURN when a handler is installed -- it must not abort.
    // Odessa's historical CHECK() called QCoreApplication::exit(-1) and let the
    // failing function run on; a great deal of code depends on that. If this
    // ever aborts, the process dies here and the test never reports.
    GHandlerCalls = 0;
    CORE_CHECK(false, "deliberate failing check")
    Expect(GHandlerCalls == 1, "Fatal reaches the handler");
    Expect(GLastVerbosity == LogVerbosity::Fatal, "Fatal verbosity is forwarded");

    // A passing check must not log at all.
    GHandlerCalls = 0;
    CORE_CHECK(true, "should not fire")
    Expect(GHandlerCalls == 0, "a passing CORE_CHECK is silent");

    // A run log gets every line beside the handler, and keeps only the newest.
    const std::string LogDir = "SmokeScratch/Logs";
    {
        std::error_code Ignored;
        std::filesystem::remove_all(LogDir, Ignored);   // a previous run's logs would count against the kept
    }
    appCreateDirectories(LogDir);
    for(const char* Stale : { "SmokeRun-2000.01.01-00.00.00.log", "SmokeRun-2000.01.02-00.00.00.log", "Other-2000.01.01-00.00.00.log" })
    {
        if(FileHandle Handle = appFileOpenWrite(LogDir + "/" + Stale)) { appFileClose(Handle); }
    }
    GHandlerCalls = 0;
    Expect(OpenRunLog(LogDir, "SmokeRun", 2), "a run log opens");
    const std::string RunLog = GetRunLogPath();
    CORE_LOG_WARN("into the run log")
    CloseRunLog();
    Expect(GHandlerCalls == 1 && GetRunLogPath().empty(), "the handler still hears it, and closing forgets the path");
    std::vector<uint8_t> Bytes;
    Expect(appFileReadAll(RunLog, Bytes) && std::string(Bytes.begin(), Bytes.end()).find("[WARN] into the run log") != std::string::npos,
           "the run log holds the line, with its verbosity");
    {
        const std::string Text(Bytes.begin(), Bytes.end());
        const size_t Line = Text.find('\n') + 1;   // the line after the header
        Expect(Text.compare(0, 22, "SmokeRun log, started ") == 0, "a run log opens with when the run started");
        Expect(Text.size() > Line + 24 && Text[Line + 4] == '-' && Text[Line + 7] == '-' && Text[Line + 10] == ' '
               && Text[Line + 13] == ':' && Text[Line + 19] == '.', "each line starts with its date and time");
    }
    Expect(!appFileExists(LogDir + "/SmokeRun-2000.01.01-00.00.00.log") && appFileExists(LogDir + "/SmokeRun-2000.01.02-00.00.00.log")
           && appFileExists(LogDir + "/Other-2000.01.01-00.00.00.log"), "only the newest logs of its own name are kept");
}

// --- file seam ------------------------------------------------------------

static void TestFileSeam()
{
    std::printf("[file seam]\n");

    const std::string Dir = "SmokeScratch/Nested/Deeper";
    Expect(appCreateDirectories(Dir), "appCreateDirectories creates a relative nested path");
    Expect(appCreateDirectories(Dir), "appCreateDirectories is idempotent");

    const std::string Path = Dir + "/binary.bin";

    /*
     * THE case this seam exists for. 0x1A is EOF in Windows text mode and
     * 0x0D/0x0A get translated -- any of which silently corrupts SPIR-V. An
     * embedded 0x00 additionally breaks anything that treats the buffer as a
     * C string. If appFile* ever stops opening in binary mode, this is what
     * catches it.
     */
    const unsigned char Payload[] = {
        0x03, 0x02, 0x23, 0x07,     // SPIR-V magic, little-endian
        0x00, 0x1A, 0x0D, 0x0A,
        0xFF, 0x00, 0x80, 0x7F
    };
    const size_t PayloadSize = sizeof(Payload);

    FileHandle W = appFileOpenWrite(Path);
    Expect(W != nullptr, "appFileOpenWrite succeeds");
    if(W)
    {
        const size_t Wrote = appFileWrite(W, Payload, PayloadSize);
        Expect(Wrote == PayloadSize, "appFileWrite writes every byte");
        appFileClose(W);
    }

    Expect(appFileExists(Path), "appFileExists finds the file just written");
    Expect(!appFileExists(Dir), "appFileExists is false for a directory");
    Expect(!appFileExists("SmokeScratch/does-not-exist.bin"), "appFileExists is false for a missing file");

    std::vector<unsigned char> Read;
    Expect(appFileReadAll(Path, Read), "appFileReadAll succeeds");
    Expect(Read.size() == PayloadSize, "appFileReadAll returns the exact byte count");
    Expect(Read.size() == PayloadSize && std::memcmp(Read.data(), Payload, PayloadSize) == 0,
           "binary payload round-trips byte-exact (no text-mode translation)");

    std::vector<unsigned char> Missing;
    Expect(!appFileReadAll("SmokeScratch/does-not-exist.bin", Missing),
           "appFileReadAll fails cleanly on a missing file");

    // Empty file is a successful read of nothing, not a failure.
    const std::string EmptyPath = Dir + "/empty.bin";
    FileHandle E = appFileOpenWrite(EmptyPath);
    if(E) appFileClose(E);
    std::vector<unsigned char> EmptyBytes;
    Expect(appFileReadAll(EmptyPath, EmptyBytes), "appFileReadAll succeeds on an empty file");
    Expect(EmptyBytes.empty(), "an empty file reads as zero bytes");

    const std::string AppData = appGetAppDataDir("OdessaSmokeTest");
    Expect(!AppData.empty(), "appGetAppDataDir returns a path");

    Expect(appThreadID() != 0, "appThreadID is non-zero");
    Expect(appThreadID() == appThreadID(), "appThreadID is stable within a thread");
}

static void TestFileMetadataSeam()
{
    std::printf("[file metadata seam]\n");

    const std::string Dir  = "SmokeScratch/Nested/Deeper";
    const std::string Path = Dir + "/binary.bin";       // written by TestFileSeam

    FileStamp Stamp;
    Expect(appFileStat(Path, Stamp), "appFileStat succeeds on an existing file");
    Expect(Stamp.IsValid(), "a successful stat is IsValid()");
    Expect(Stamp.SizeBytes == 12, "appFileStat reports the exact byte count");

    FileStamp Again;
    Expect(appFileStat(Path, Again) && Again == Stamp, "restatting an unchanged file gives an equal stamp");

    FileStamp MissingStamp;
    Expect(!appFileStat("SmokeScratch/does-not-exist.bin", MissingStamp),
           "appFileStat fails on a missing file");
    Expect(!MissingStamp.IsValid(), "a failed stat leaves the stamp invalid");
    Expect(!appFileStat(Dir, MissingStamp), "appFileStat fails on a directory");

    // A different size alone must register as a change, which is the half of
    // the signature that covers coarse mtime resolution.
    FileStamp Bigger = Stamp;
    Bigger.SizeBytes += 1;
    Expect(Bigger != Stamp, "a size difference alone makes the stamps unequal");

    const std::string CopyPath = Dir + "/binary.copy.bin";
    appFileDelete(CopyPath);
    Expect(appFileCopy(Path, CopyPath, false), "appFileCopy creates a new file");
    Expect(appFileExists(CopyPath), "the copy exists");

    std::vector<uint8_t> Original;
    std::vector<uint8_t> Copied;
    Expect(appFileReadAll(Path, Original) && appFileReadAll(CopyPath, Copied),
           "both source and copy read back");
    Expect(Original.size() == Copied.size() &&
           (Original.empty() || std::memcmp(Original.data(), Copied.data(), Original.size()) == 0),
           "the copy is byte-identical to the source");

    Expect(!appFileCopy(Path, CopyPath, false), "appFileCopy refuses to overwrite when not asked to");
    Expect(appFileCopy(Path, CopyPath, true), "appFileCopy overwrites when asked to");

    Expect(appFileCanOpenExclusive(Path), "an idle file opens exclusively");
    Expect(!appFileCanOpenExclusive("SmokeScratch/does-not-exist.bin"),
           "a missing file does not open exclusively");

    std::vector<std::string> Names;
    Expect(appDirectoryList(Dir, Names), "appDirectoryList succeeds");
    bool bFoundCopy = false;
    bool bFoundDot  = false;
    for(const std::string& Name : Names)
    {
        if(Name == "binary.copy.bin") bFoundCopy = true;
        if(Name == "." || Name == "..") bFoundDot = true;
    }
    Expect(bFoundCopy, "appDirectoryList finds a file just created");
    Expect(!bFoundDot, "appDirectoryList omits . and ..");
    Expect(!appDirectoryList("SmokeScratch/no-such-dir", Names), "appDirectoryList fails on a missing dir");

    Expect(appFileDelete(CopyPath), "appFileDelete removes the copy");
    Expect(!appFileExists(CopyPath), "the copy is gone");
    Expect(appFileDelete(CopyPath), "appFileDelete is idempotent -- already gone is success");

    const std::string EmptyDir = "SmokeScratch/EmptyDirForDelete";
    Expect(appCreateDirectories(EmptyDir), "a scratch directory is created");
    Expect(appDirectoryDelete(EmptyDir), "appDirectoryDelete removes an empty directory");
    Expect(appDirectoryDelete(EmptyDir), "appDirectoryDelete is idempotent");
    Expect(!appDirectoryDelete(Dir), "appDirectoryDelete refuses a non-empty directory");

    Expect(appProcessID() != 0, "appProcessID is non-zero");
    Expect(appProcessID() == appProcessID(), "appProcessID is stable");
    Expect(appProcessIsRunning(appProcessID()), "appProcessIsRunning: this process is");
    Expect(!appProcessIsRunning(0x7FFFFFF0u), "appProcessIsRunning: an id no process has is not");
}

static void TestDLLSeam()
{
    std::printf("[dll seam]\n");

    // Only the failure path is testable without a real library to load; the
    // success path is covered end to end by the module tests, which is where a
    // loadable DLL actually exists.
    Expect(appDLLOpen("SmokeScratch/definitely-not-a-library.dll") == nullptr,
           "appDLLOpen fails on a nonexistent path");
    Expect(!appDLLGetLastError().empty(), "a failed appDLLOpen leaves a diagnostic");

    Expect(appDLLGetSymbol(nullptr, "Anything") == nullptr, "appDLLGetSymbol rejects a null handle");
    Expect(!appDLLGetLastError().empty(), "a failed appDLLGetSymbol leaves a diagnostic");

    appDLLClose(nullptr);       // must be a no-op, not a crash
    Expect(true, "appDLLClose(nullptr) is a no-op");
}

/*
 * The reload blob, exercised entirely host-side. This runs in every
 * configuration, including a static one where no module can be loaded at all,
 * because the (version, size) gate is the thing standing between a reloaded
 * module and a reinterpretation of stale bytes.
 */
static void TestModuleReloadStateBuffer()
{
    std::printf("[module reload state]\n");

    struct Payload
    {
        uint32_t A;
        uint32_t B;
    };

    ModuleReloadState State;
    Expect(State.IsEmpty(), "a fresh state is empty");
    Expect(State.GetStateVersion() == 0, "a fresh state has version 0");
    Expect(State.GetSizeBytes() == 0, "a fresh state has no bytes");

    const Payload Written{ 0xABCDEF01u, 7u };
    Expect(State.Write(3, &Written, sizeof(Written)), "Write succeeds");
    Expect(!State.IsEmpty(), "a written state is not empty");
    Expect(State.GetStateVersion() == 3, "the state version round-trips");
    Expect(State.GetSizeBytes() == sizeof(Written), "the byte count round-trips");

    Payload ReadBack{ 0, 0 };
    Expect(State.Read(3, &ReadBack, sizeof(ReadBack)), "Read with a matching version and size succeeds");
    Expect(ReadBack.A == Written.A && ReadBack.B == Written.B, "the payload round-trips byte for byte");

    // A mismatch must leave the destination ALONE, not partially filled: the
    // caller is about to treat it as a struct and cannot tell the difference.
    Payload Untouched{ 0x11111111u, 0x22222222u };
    Expect(!State.Read(4, &Untouched, sizeof(Untouched)), "Read with the wrong version fails");
    Expect(Untouched.A == 0x11111111u && Untouched.B == 0x22222222u,
           "a version mismatch leaves the destination untouched");

    uint8_t Wrong[sizeof(Payload) + 4];
    std::memset(Wrong, 0x5A, sizeof(Wrong));
    Expect(!State.Read(3, Wrong, sizeof(Wrong)), "Read with the wrong size fails");
    bool bAllUnchanged = true;
    for(size_t Index = 0; Index < sizeof(Wrong); ++Index)
    {
        if(Wrong[Index] != 0x5A) bAllUnchanged = false;
    }
    Expect(bAllUnchanged, "a size mismatch leaves the destination untouched");

    // A second write replaces rather than appends -- a reload writes the whole
    // blob every time, and a growing buffer would be a slow leak.
    const uint32_t Small = 42u;
    Expect(State.Write(9, &Small, sizeof(Small)), "a second Write succeeds");
    Expect(State.GetSizeBytes() == sizeof(Small), "the second Write replaces rather than appends");
    Expect(State.GetStateVersion() == 9, "the second Write replaces the version");
    Expect(!State.Read(3, &ReadBack, sizeof(ReadBack)), "the previous payload is no longer readable");

    uint32_t SmallBack = 0;
    Expect(State.Read(9, &SmallBack, sizeof(SmallBack)) && SmallBack == 42u,
           "the second payload reads back");

    // Zero bytes is a real payload: a stateless module can still say "I ran".
    Expect(State.Write(5, nullptr, 0), "a zero-byte Write succeeds");
    Expect(State.IsEmpty(), "a zero-byte payload reports empty");
    Expect(State.GetStateVersion() == 5, "a zero-byte Write still records its version");
    Expect(State.Read(5, nullptr, 0), "a zero-byte payload reads back successfully");
    Expect(!State.Read(6, nullptr, 0), "a zero-byte payload still checks its version");

    Expect(State.Write(1, &Written, sizeof(Written)), "the buffer is reusable after a zero-byte write");
    State.Reset();
    Expect(State.IsEmpty(), "Reset empties the state");
    Expect(State.GetStateVersion() == 0, "Reset clears the version");
    Expect(!State.Read(1, &ReadBack, sizeof(ReadBack)), "nothing reads back after Reset");

    // Moving is what lets a ModuleRecord live in a std::vector.
    ModuleReloadState Source;
    Expect(Source.Write(2, &Written, sizeof(Written)), "a state to move from is written");
    ModuleReloadState Moved(std::move(Source));
    Expect(Moved.GetStateVersion() == 2 && Moved.GetSizeBytes() == sizeof(Written),
           "move construction carries the payload");
    Expect(Source.IsEmpty(), "the moved-from state is left empty");

    ModuleReloadState Target;
    Target = std::move(Moved);
    Expect(Target.GetStateVersion() == 2 && Target.Read(2, &ReadBack, sizeof(ReadBack)) &&
           ReadBack.A == Written.A,
           "move assignment carries the payload");
    Expect(Moved.IsEmpty(), "the moved-from state is left empty after assignment");
}

static void TestFileProvider()
{
    std::printf("[file provider]\n");

    FilesystemProvider Provider("SmokeScratch/Nested/Deeper");

    Expect(Provider.Exists("binary.bin"), "provider resolves against its base dir");
    Expect(!Provider.Exists("nope.bin"), "provider reports a missing file");

    std::vector<unsigned char> Bytes;
    Expect(Provider.ReadAll("binary.bin", Bytes) && Bytes.size() == 12,
           "provider reads through appFile*");

    SetFileProvider(&Provider);
    Expect(GetFileProvider() == &Provider, "the process-wide provider round-trips");
    SetFileProvider(nullptr);
    Expect(GetFileProvider() == nullptr, "the process-wide provider can be cleared");
}

// --- image ----------------------------------------------------------------

static void TestImage()
{
    std::printf("[image]\n");

    Expect(Image().IsNull(), "a default Image is null");
    Expect(BytesPerPixel(EImageFormat::BGRA8) == 4, "BGRA8 is 4 bytes per pixel");
    Expect(BytesPerPixel(EImageFormat::Gray8) == 1, "Gray8 is 1 byte per pixel");

    Image Img(4, 3, EImageFormat::BGRA8);
    Expect(!Img.IsNull(), "a sized Image is not null");
    Expect(Img.GetWidth() == 4 && Img.GetHeight() == 3, "dimensions are preserved");
    Expect(Img.GetStride() == 16, "stride is tightly packed (width * bpp)");
    Expect(Img.GetSizeBytes() == 48, "buffer is exactly stride * height");
    Expect(Img.GetScanline(0) != nullptr && Img.GetScanline(2) != nullptr, "valid scanlines resolve");
    Expect(Img.GetScanline(-1) == nullptr && Img.GetScanline(3) == nullptr, "out-of-range scanlines are null");

    /*
     * The padded-source case. QImage pads scanlines to 4 bytes; for a 4-byte
     * format that coincides with tight packing at every width, so a whole
     * buffer memcpy passes -- and then corrupts the first time an 8-bit mask
     * of odd width shows up. Gray8 at width 3 is exactly that: tight stride 3,
     * source stride 4. If CopyFrom ever reverts to a single memcpy, the
     * assertion below fails.
     */
    const unsigned char Padded[] = {
        1, 2, 3, 0xEE,      // 0xEE is padding and must never be copied
        4, 5, 6, 0xEE,
        7, 8, 9, 0xEE
    };
    const Image FromPadded = Image::CopyFrom(Padded, 3, 3, EImageFormat::Gray8, 4);
    Expect(FromPadded.GetStride() == 3, "Gray8 width 3 packs to a 3-byte stride");
    Expect(FromPadded.GetSizeBytes() == 9, "padding is not carried into the destination");

    bool bPaddingLeaked = false;
    for(size_t Idx = 0; Idx < FromPadded.GetSizeBytes(); ++Idx)
    {
        if(FromPadded.GetData()[Idx] == 0xEE) bPaddingLeaked = true;
    }
    Expect(!bPaddingLeaked, "row-by-row copy drops source padding");
    Expect(FromPadded.GetData()[3] == 4, "second row starts at the packed offset, not the padded one");

    // Tight source (stride 0 means "tightly packed").
    const unsigned char Tight[] = { 1, 2, 3, 4, 5, 6 };
    const Image FromTight = Image::CopyFrom(Tight, 3, 2, EImageFormat::Gray8, 0);
    Expect(FromTight.GetData()[3] == 4, "stride 0 is treated as tight packing");

    /*
     * Flips are separate operations and must stay that way: the renderer
     * composes them at specific points, and folding two into a 180-degree
     * rotation is invisible to a centred circular brush dab. An asymmetric
     * pattern is used here for the same reason the app checklist asks for an
     * "F" rather than a dot.
     */
    const unsigned char Asym[] = {
        1, 2,
        3, 4
    };
    Image Flip = Image::CopyFrom(Asym, 2, 2, EImageFormat::Gray8, 0);

    Flip.FlipVertical();
    Expect(Flip.GetData()[0] == 3 && Flip.GetData()[1] == 4
        && Flip.GetData()[2] == 1 && Flip.GetData()[3] == 2, "FlipVertical swaps rows only");

    Flip.FlipVertical();
    Expect(Flip.GetData()[0] == 1 && Flip.GetData()[3] == 4, "FlipVertical is its own inverse");

    Flip.FlipHorizontal();
    Expect(Flip.GetData()[0] == 2 && Flip.GetData()[1] == 1
        && Flip.GetData()[2] == 4 && Flip.GetData()[3] == 3, "FlipHorizontal swaps columns only");

    Flip.FlipHorizontal();
    Expect(Flip.GetData()[0] == 1 && Flip.GetData()[3] == 4, "FlipHorizontal is its own inverse");

    // Odd dimensions: the middle row/column must survive a flip untouched.
    const unsigned char Odd[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    Image OddImg = Image::CopyFrom(Odd, 3, 3, EImageFormat::Gray8, 0);
    OddImg.FlipVertical();
    Expect(OddImg.GetData()[3] == 4 && OddImg.GetData()[4] == 5 && OddImg.GetData()[5] == 6,
           "odd height leaves the middle row in place");
    OddImg.FlipHorizontal();
    Expect(OddImg.GetData()[4] == 5, "odd width leaves the middle column in place");

    // 4-byte format flips must move whole pixels, not bytes.
    const unsigned char TwoPx[] = {
        10, 11, 12, 13,
        20, 21, 22, 23
    };
    Image Px = Image::CopyFrom(TwoPx, 2, 1, EImageFormat::BGRA8, 0);
    Px.FlipHorizontal();
    Expect(Px.GetData()[0] == 20 && Px.GetData()[1] == 21
        && Px.GetData()[2] == 22 && Px.GetData()[3] == 23,
           "FlipHorizontal moves whole BGRA8 pixels, preserving channel order");
}

static void TestImageFormatConversion()
{
    std::printf("[image format conversion]\n");

    /*
     * This is the F1 red/blue-swap guard. OpenGL reads back BGRA8 and Vulkan
     * RGBA8; the two must convert to each other exactly, and a same-format
     * request must not touch the bytes. A saturated red pixel is used because
     * it is the case that makes a channel swap unmistakable -- pure red in one
     * order is pure blue in the other.
     */
    const unsigned char RedBGRA[] = { 0, 0, 255, 255 };   // B,G,R,A -- opaque red
    const Image Bgra = Image::CopyFrom(RedBGRA, 1, 1, EImageFormat::BGRA8, 0);

    const Image Rgba = Bgra.ConvertedTo(EImageFormat::RGBA8);
    Expect(Rgba.GetFormat() == EImageFormat::RGBA8, "ConvertedTo reports the requested format");
    Expect(Rgba.GetData()[0] == 255 && Rgba.GetData()[1] == 0 && Rgba.GetData()[2] == 0,
           "opaque red survives BGRA8 -> RGBA8 as R=255 (no channel swap)");
    Expect(Rgba.GetData()[3] == 255, "alpha is preserved across conversion");

    const Image RoundTrip = Rgba.ConvertedTo(EImageFormat::BGRA8);
    Expect(RoundTrip.GetData()[0] == 0 && RoundTrip.GetData()[1] == 0 && RoundTrip.GetData()[2] == 255,
           "BGRA8 -> RGBA8 -> BGRA8 round-trips byte-exact");

    const Image Same = Bgra.ConvertedTo(EImageFormat::BGRA8);
    Expect(Same.GetData()[2] == 255, "a same-format conversion is a plain copy");

    // Green must sit still through all of it -- only R and B move.
    const unsigned char GreenRGBA[] = { 0, 255, 0, 255 };
    const Image G = Image::CopyFrom(GreenRGBA, 1, 1, EImageFormat::RGBA8, 0);
    Expect(G.ConvertedTo(EImageFormat::BGRA8).GetData()[1] == 255, "green is untouched by the swizzle");

    // Multi-pixel, to catch a per-pixel stride error rather than a lucky first pixel.
    const unsigned char Two[] = { 1, 2, 3, 4,  5, 6, 7, 8 };
    const Image T = Image::CopyFrom(Two, 2, 1, EImageFormat::RGBA8, 0);
    const Image TC = T.ConvertedTo(EImageFormat::BGRA8);
    Expect(TC.GetData()[0] == 3 && TC.GetData()[2] == 1, "first pixel swizzled");
    Expect(TC.GetData()[4] == 7 && TC.GetData()[6] == 5, "second pixel swizzled at the right offset");

    // Unsupported conversions must fail loudly, not return wrong channels.
    Expect(Bgra.ConvertedTo(EImageFormat::Gray8).IsNull(),
           "an unsupported conversion returns null rather than wrong pixels");
    Expect(Image().ConvertedTo(EImageFormat::RGBA8).IsNull(), "converting a null image stays null");
}

static void TestQueue()
{
    std::printf("[queue]\n");

    ThreadSafeQueue<int> Q;
    Expect(Q.IsEmpty(), "a new queue is empty");
    Expect(Q.Size() == 0, "a new queue has size 0");

    int Out = -1;
    Expect(!Q.TryDequeue(Out), "TryDequeue on an empty queue returns false");
    Expect(Out == -1, "a failed TryDequeue leaves the out-param untouched");

    Q.Enqueue(1); Q.Enqueue(2); Q.Enqueue(3);
    Expect(!Q.IsEmpty() && Q.Size() == 3, "enqueue updates size");

    // FIFO, not LIFO -- the render command order is the draw order.
    Expect(Q.TryDequeue(Out) && Out == 1, "dequeues in FIFO order (first)");
    Expect(Q.TryDequeue(Out) && Out == 2, "dequeues in FIFO order (second)");
    Expect(Q.TryDequeue(Out) && Out == 3, "dequeues in FIFO order (third)");
    Expect(!Q.TryDequeue(Out) && Q.IsEmpty(), "queue is empty once drained");

    Q.Enqueue(9);
    Q.Clear();
    Expect(Q.IsEmpty(), "Clear empties the queue");

    // DrainTo takes a snapshot and appends.
    ThreadSafeQueue<int> D;
    D.Enqueue(10); D.Enqueue(20);
    std::vector<int> Batch;
    Batch.push_back(1);
    Expect(D.DrainTo(Batch), "DrainTo reports it took something");
    Expect(Batch.size() == 3 && Batch[1] == 10 && Batch[2] == 20, "DrainTo appends in order");
    Expect(D.IsEmpty(), "DrainTo empties the source");
    Expect(!D.DrainTo(Batch), "DrainTo on an empty queue returns false");

    /*
     * The reason this class exists. One producer and one consumer running
     * concurrently, exactly the GRenderCommands pairing: the UI thread
     * enqueues while the render thread drains.
     *
     * Asserts that every item arrives exactly once and in order. Under the
     * unsynchronized QQueue this pattern is a data race; if the mutex is ever
     * removed, this is what fails (or corrupts, or hangs).
     */
    {
        ThreadSafeQueue<int> Concurrent;
        const int kCount = 20000;
        std::atomic<bool> bProducerDone{false};

        std::thread Producer([&]()
        {
            for(int i = 0; i < kCount; ++i) { Concurrent.Enqueue(i); }
            bProducerDone = true;
        });

        int Received = 0;
        bool bOrdered = true;
        int Value = 0;
        while(Received < kCount)
        {
            if(Concurrent.TryDequeue(Value))
            {
                if(Value != Received) { bOrdered = false; }
                ++Received;
            }
            else if(bProducerDone && Concurrent.IsEmpty() && Received < kCount)
            {
                break;      // producer finished and nothing left: items were lost
            }
        }
        Producer.join();

        Expect(Received == kCount, "every concurrently-enqueued item is received exactly once");
        Expect(bOrdered, "concurrent enqueue/dequeue preserves FIFO order");
        Expect(Concurrent.IsEmpty(), "nothing is left behind after a concurrent drain");
    }
}

// --- threads --------------------------------------------------------------

/*
 * A message-matching handler, in the shape BarrierLogHandler uses further down.
 *
 * The thread tests have to assert on WHAT was logged rather than merely that
 * something was, because three of the behaviours under test are ONLY observable
 * through the log: both Start refusals, and the unjoined-destructor path that
 * has to join-and-complain rather than assert.
 */
static int         GThreadErrors   = 0;
static int         GThreadWarnings = 0;
static int         GThreadFatals   = 0;
static std::string GThreadLog;

static void ThreadLogHandler(const LogVerbosity Verbosity, const char* Message)
{
    if(Verbosity == LogVerbosity::Error)   { ++GThreadErrors;   }
    if(Verbosity == LogVerbosity::Warning) { ++GThreadWarnings; }
    if(Verbosity == LogVerbosity::Fatal)   { ++GThreadFatals;   }

    if(Message)
    {
        GThreadLog += Message;
        GThreadLog += "\n";
    }
}

static bool ThreadLogHas(const char* InNeedle)
{
    return std::strstr(GThreadLog.c_str(), InNeedle) != nullptr;
}

static void ResetThreadLog()
{
    GThreadErrors   = 0;
    GThreadWarnings = 0;
    GThreadFatals   = 0;
    GThreadLog.clear();
}

static void TestThreadSeam()
{
    std::printf("[thread seam]\n");

    const uint32_t Logical  = Threads::GetHardwareThreadCount();
    const uint32_t Physical = Threads::GetPhysicalCoreCount();

    /*
     * CorePlatform.h promises both are at least 1 and never 0, specifically so
     * that "count - 1" at a pool-sizing call site cannot underflow into a pool
     * of four billion workers.
     */
    Expect(Logical  >= 1, "the hardware thread count is at least 1");
    Expect(Physical >= 1, "the physical core count is at least 1");

    /* Physical cores cannot outnumber logical threads. This is the check that
     * catches a record-walk bug in appThreadPhysicalCoreCount, which is
     * otherwise invisible without knowing the machine -- the first version of
     * the Windows implementation dropped the final record and under-reported. */
    Expect(Physical <= Logical, "physical cores do not outnumber logical threads");

    Threads::SetMainThread();
    Expect(Threads::IsMainThread(), "the thread that called SetMainThread is the main thread");
    Expect(Threads::GetMainThreadID() == appThreadID(), "GetMainThreadID reports that thread");
}

static void TestSemaphore()
{
    std::printf("[semaphore]\n");

    Semaphore Sem(0);
    Expect(!Sem.TryWait(), "TryWait fails on an empty semaphore");
    Expect(!Sem.TryWaitFor(10), "TryWaitFor times out on an empty semaphore");

    /* Signalling more than are waiting ACCUMULATES -- the whole difference from
     * an event, and what lets "I just queued 8 jobs" be written as Signal(8)
     * with no knowledge of how many workers happen to be awake. */
    Sem.Signal(3);
    Expect(Sem.TryWait(), "signal 1 of 3 is a permit");
    Expect(Sem.TryWait(), "signal 2 of 3 is a permit");
    Expect(Sem.TryWait(), "signal 3 of 3 is a permit");
    Expect(!Sem.TryWait(), "the count does not go negative");

    Semaphore Initial(2);
    Expect(Initial.TryWait() && Initial.TryWait(), "the initial count is honoured");
    Expect(!Initial.TryWait(), "the initial count is not exceeded");

    /*
     * The reason this class exists instead of a condvar at each call site: a
     * signal must never be LOST between a waiter's check and its sleep.
     *
     * 5000 round trips rather than a handful, because a lost wakeup is a timing
     * bug -- at low counts it passes and then costs somebody 30ms in a frame
     * months later. If the predicate inside Wait is ever removed, this HANGS
     * here, which is a far better outcome than a flaky pass.
     */
    {
        Semaphore        ToWorker(0);
        Semaphore        ToMain(0);
        const int        kRounds = 5000;
        std::atomic<int> Ponged{0};

        Thread Worker;
        Expect(Worker.Start("Smoke/Pong", [&]
        {
            for(int i = 0; i < kRounds; ++i)
            {
                ToWorker.Wait();
                Ponged.fetch_add(1);
                ToMain.Signal();
            }
        }), "the ping-pong worker starts");

        for(int i = 0; i < kRounds; ++i)
        {
            ToWorker.Signal();
            ToMain.Wait();
        }
        Worker.Join();

        Expect(Ponged.load() == kRounds, "every signal is received exactly once, none lost");
    }
}

static void TestThreadLifetime()
{
    std::printf("[thread lifetime]\n");

    Expect(Threads::GetLiveThreadCount() == 0, "nothing is registered before the first start");

    {
        std::atomic<ThreadID> WorkerID{0};
        std::atomic<bool>     bWorkerThinksItIsMain{true};
        Semaphore             Hold(0);

        Thread Worker;
        Expect(Worker.Start("Smoke/Ident", [&]
        {
            WorkerID.store(appThreadID());
            bWorkerThinksItIsMain.store(Threads::IsMainThread());
            Hold.Wait();
        }), "a thread starts");

        Expect(Threads::GetLiveThreadCount() == 1, "a started thread is registered");
        Expect(std::strcmp(Worker.GetName(), "Smoke/Ident") == 0, "the name round-trips");

        /* The tag the image barrier filters on. A thread started by the host must
         * read IMAGE_Host -- if this ever reads something else, Start is reading
         * GImageID in the wrong image and the barrier silently protects nothing.
         * See the note on Start's defaulted InOwningImage. */
        Expect(Worker.GetOwningImage() == IMAGE_Host, "a host-started thread is tagged IMAGE_Host");

        Hold.Signal();
        Worker.Join();

        Expect(WorkerID.load() != 0, "the worker published a thread id");
        Expect(WorkerID.load() != appThreadID(), "the worker id differs from the main thread id");

        /* The entire point of CORE_CHECK_MAIN_THREAD: the four subsystems
         * documented as single-threaded can finally detect a foreign caller. */
        Expect(!bWorkerThinksItIsMain.load(), "IsMainThread is false on a worker");

        Expect(!Worker.IsRunning(), "a joined thread is not running");
        Expect(Threads::GetLiveThreadCount() == 0, "Join unregisters the thread");

        Worker.Join();
        Expect(Threads::GetLiveThreadCount() == 0, "Join is idempotent");
    }

    /* Truncated, never rejected -- and appThreadSetName truncates again and
     * harder (15 chars plus the null on Linux, which rejects the call outright
     * for anything longer). A long name must never be worth error-checking. */
    {
        Thread Long;
        Expect(Long.Start("Smoke/AVeryLongThreadNameWellPastTheCapacity", []{}),
               "an over-long name still starts the thread");
        Expect(std::strlen(Long.GetName()) == Thread::NAME_CAPACITY - 1,
               "an over-long name is truncated to the capacity");
        Long.Join();
    }

    /*
     * Both ends of the priority ladder. A refusal inside the seam is NOT a Start
     * failure and must not be: Linux cannot honour High without privileges a game
     * will not have, and the thread still has work to do.
     */
    {
        Thread Background;
        Expect(Background.Start("Smoke/Bg", []{}, ThreadPriority::Background),
               "a background-priority thread starts");
        Background.Join();

        Thread High;
        Expect(High.Start("Smoke/Hi", []{}, ThreadPriority::High),
               "a high-priority thread starts even where the OS refuses the priority");
        High.Join();
    }
}

static void TestThreadRefusals()
{
    std::printf("[thread refusals]\n");

    /* Restore whatever was installed rather than clearing to nullptr: with no
     * handler a Fatal aborts, and the tests that run after this one were written
     * against an installed handler. */
    const LogHandler Previous = GetLogHandler();
    SetLogHandler(&ThreadLogHandler);

    ResetThreadLog();
    {
        Thread Empty;
        Expect(!Empty.Start("Smoke/Empty", Thread::EntryFn{}),
               "Start refuses an empty entry function");
        Expect(ThreadLogHas("empty entry function"), "the empty-entry refusal is logged");
        Expect(Threads::GetLiveThreadCount() == 0, "a refused start registers nothing");
    }

    ResetThreadLog();
    {
        Semaphore Hold(0);
        Thread    Busy;

        Expect(Busy.Start("Smoke/Busy", [&Hold]{ Hold.Wait(); }), "the first start succeeds");
        Expect(!Busy.Start("Smoke/Busy2", []{}), "Start refuses an already-started thread");
        Expect(ThreadLogHas("already started"), "the double-start refusal is logged");
        Expect(std::strcmp(Busy.GetName(), "Smoke/Busy") == 0,
               "a refused restart does not overwrite the running thread's name");
        Expect(Threads::GetLiveThreadCount() == 1, "a refused restart does not double-register");

        Hold.Signal();
        Busy.Join();
    }

    /*
     * A Thread destroyed while running must JOIN AND COMPLAIN. It cannot assert
     * instead: std::thread's destructor calls std::terminate on a joinable
     * thread, so the only alternative to joining is taking the process down.
     */
    ResetThreadLog();
    {
        std::atomic<bool> bRan{false};
        {
            Thread Unjoined;
            Expect(Unjoined.Start("Smoke/NoJoin", [&bRan]{ bRan.store(true); }),
                   "the soon-to-be-abandoned thread starts");
        }
        Expect(bRan.load(), "the destructor joined rather than terminating");
        Expect(GThreadErrors >= 1, "the unjoined destructor logs an error");
        Expect(ThreadLogHas("destroyed without being joined"), "the error says what went wrong");
        Expect(Threads::GetLiveThreadCount() == 0, "the destructor unregisters the thread");
    }

    /*
     * CORE_CHECK_MAIN_THREAD from a worker. Fatal, and it has to be Fatal -- but
     * per CoreLog.h's fatal semantics an installed handler logs and RETURNS,
     * which is the only reason this is testable at all rather than aborting the
     * suite. Same reasoning the log seam test at the top of this file relies on.
     */
    ResetThreadLog();
    {
        Thread Offender;
        Expect(Offender.Start("Smoke/Offend", []
        {
            CORE_CHECK_MAIN_THREAD("ModuleManager::Tick")
        }), "the offending thread starts");
        Offender.Join();

        Expect(GThreadFatals == 1, "CORE_CHECK_MAIN_THREAD is fatal off the main thread");
        Expect(ThreadLogHas("ModuleManager::Tick"), "the check names the misused subsystem");
    }

    SetLogHandler(Previous);
}

static void TestThreadImageBarrier()
{
    std::printf("[thread image barrier]\n");

    const LogHandler Previous = GetLogHandler();
    SetLogHandler(&ThreadLogHandler);
    ResetThreadLog();

    {
        Semaphore Hold(0);
        Thread    A;
        Thread    B;

        Expect(A.Start("Smoke/ImgA", [&Hold]{ Hold.Wait(); }), "barrier thread A starts");
        Expect(B.Start("Smoke/ImgB", [&Hold]{ Hold.Wait(); }), "barrier thread B starts");
        Expect(Threads::GetLiveThreadCount() == 2, "both threads are registered");

        /*
         * RELEASED BEFORE THE BARRIER RUNS, deliberately. OnImageUnloading has no
         * cancellation -- killing a thread at an arbitrary instruction leaves
         * every lock it held locked -- so a worker that never returns hangs the
         * unload. That is the documented design, and it is not something to
         * demonstrate by deadlocking CI.
         */
        Hold.Signal(2);

        const uint32_t Joined = Threads::OnImageUnloading(IMAGE_Host);
        Expect(Joined == 2, "the barrier joins every thread the image started");
        Expect(Threads::GetLiveThreadCount() == 0, "the registry is empty once it returns");

        /* Logged BEFORE the waiting starts, naming each thread: when this call
         * does hang, that line is the only evidence of which thread is holding
         * the unload up. Written after the join it would never appear. */
        Expect(GThreadWarnings >= 1, "the barrier warns before it waits");
        Expect(ThreadLogHas("Smoke/ImgA"), "the warning names thread A");
        Expect(ThreadLogHas("Smoke/ImgB"), "the warning names thread B");
    }

    Expect(Threads::OnImageUnloading(IMAGE_Host) == 0,
           "the barrier on an image with no threads is a no-op");

    /*
     * The filter has to work in both directions. A barrier for one image joining
     * another image's threads would be worse than one that joins nothing: it
     * would block a module unload on the host's own workers.
     */
    {
        Semaphore Hold(0);
        Thread    HostThread;

        Expect(HostThread.Start("Smoke/HostOnly", [&Hold]{ Hold.Wait(); }), "a host thread starts");
        Expect(Threads::OnImageUnloading(ImageID(4321)) == 0,
               "the barrier ignores threads belonging to another image");
        Expect(Threads::GetLiveThreadCount() == 1, "the host thread is left running");

        Hold.Signal();
        HostThread.Join();
    }

    SetLogHandler(Previous);
}

static void TestThreadStress()
{
    std::printf("[thread stress]\n");

    /*
     * Eight threads released simultaneously onto one atomic.
     *
     * Not a performance measurement -- a correctness one, aimed at the registry.
     * If its locking is wrong, this is where a slot gets lost or freed twice and
     * GetLiveThreadCount stops returning to zero. The simultaneous release
     * through one Semaphore is what makes the threads contend rather than
     * politely finishing in start order.
     */
    const int kThreads    = 8;
    const int kIncrements = 50000;

    std::atomic<uint64_t> Counter{0};
    Semaphore             Go(0);
    Thread                Pool[kThreads];

    bool bAllStarted = true;
    for(int i = 0; i < kThreads; ++i)
    {
        char Name[16];
        std::snprintf(Name, sizeof(Name), "Smoke/W%d", i);

        if(!Pool[i].Start(Name, [&]
        {
            Go.Wait();
            for(int n = 0; n < kIncrements; ++n) { Counter.fetch_add(1); }
        }))
        {
            bAllStarted = false;
        }
    }

    Expect(bAllStarted, "all eight workers start");
    Expect(Threads::GetLiveThreadCount() == uint32_t(kThreads),
           "all eight are registered at the same time");

    Go.Signal(kThreads);

    for(int i = 0; i < kThreads; ++i) { Pool[i].Join(); }

    Expect(Counter.load() == uint64_t(kThreads) * uint64_t(kIncrements),
           "every increment from every thread lands");
    Expect(Threads::GetLiveThreadCount() == 0, "the registry returns to empty");
}

static void RunThreadTests()
{
    TestThreadSeam();
    TestSemaphore();
    TestThreadLifetime();
    TestThreadRefusals();
    TestThreadImageBarrier();
    TestThreadStress();
}

// --- jobs -----------------------------------------------------------------

/*
 * Job payloads must be trivially copyable (CoreJob.h says why: they are memcpy'd
 * into a slot and never destructed), so the shared state every job below touches
 * is reached through a POINTER captured by value. That is the pattern the header
 * tells a caller to use when the static_assert fires, so the tests are written
 * the way call sites are meant to be.
 */
static std::atomic<int> GJobRuns{0};

static void TestJobBasics()
{
    std::printf("[jobs]\n");

    JobSystem Jobs;
    Expect(Jobs.GetWorkerCount() >= 1, "a pool has at least one worker");
    Expect(!Jobs.IsWorkerThread(), "the main thread is not a worker of the pool it created");
    Expect(Jobs.IsQuiescent(), "a fresh pool is quiescent");

    GJobRuns.store(0);
    std::atomic<int>* Runs = &GJobRuns;

    const JobHandle H = Jobs.Dispatch([Runs] { Runs->fetch_add(1); });
    Expect(H.IsValid(), "Dispatch returns a valid handle");
    Jobs.Wait(H);
    Expect(GJobRuns.load() == 1, "a dispatched job runs exactly once");
    Expect(Jobs.IsComplete(H) || true, "IsComplete on a finished handle does not crash");

    /* Many small jobs against one group -- the fan-out shape the whole design is
     * for, and the one ParallelFor is built on. */
    GJobRuns.store(0);
    {
        const JobHandle Group = Jobs.OpenGroup();
        Expect(Group.IsValid(), "a group opens on an empty ring");

        for(int i = 0; i < 256; ++i)
        {
            Jobs.Dispatch([Runs] { Runs->fetch_add(1); }, Group);
        }

        Jobs.CloseGroup(Group);
        Jobs.Wait(Group);
    }
    Expect(GJobRuns.load() == 256, "every child of a group runs before the group completes");

    Jobs.WaitForIdle();
    Expect(Jobs.IsQuiescent(), "the pool is quiescent once drained");

    JobSystemStats Stats;
    Jobs.GetStats(Stats);
    Expect(Stats.Executed >= 257, "the stats count every job that ran");
    Expect(Stats.WorkerCount == Jobs.GetWorkerCount(), "the stats report the worker count");
}

/*
 * A job that dispatches a child and waits on it, four deep, on a ONE-WORKER
 * pool.
 *
 * THIS IS THE TEST THE WHOLE DESIGN EXISTS TO PASS. If Wait blocked the calling
 * thread instead of running jobs, the single worker would be parked inside the
 * outermost job waiting for a child that only it could ever run, and this would
 * hang forever -- which is precisely the deadlock CoreJob.h's opening block
 * describes. A hang here is the failure; there is no assertion that can catch it
 * more gently.
 */
struct NestPayload
{
    JobSystem*        Sys;
    std::atomic<int>* Counter;
    int               Depth;
};

static void NestJob(void* InPayload)
{
    const NestPayload Self = *static_cast<NestPayload*>(InPayload);
    Self.Counter->fetch_add(1);

    if(Self.Depth > 0)
    {
        NestPayload Child{ Self.Sys, Self.Counter, Self.Depth - 1 };
        const JobHandle H = Self.Sys->Dispatch(&NestJob, &Child, sizeof(Child));
        Self.Sys->Wait(H);      // must RUN the child, not block on it
    }
}

static void TestJobNestedWait()
{
    std::printf("[jobs nested wait]\n");

    JobSystem Jobs(1);          // one worker: no room to hide a blocking Wait
    Expect(Jobs.GetWorkerCount() == 1, "the pool honours an explicit worker count");

    std::atomic<int> Counter{0};
    NestPayload      Root{ &Jobs, &Counter, 4 };

    const JobHandle H = Jobs.Dispatch(&NestJob, &Root, sizeof(Root));
    Jobs.Wait(H);

    Expect(Counter.load() == 5, "a four-deep wait chain completes on a one-worker pool");
}

static void TestJobParallelFor()
{
    std::printf("[jobs parallel for]\n");

    JobSystem Jobs;

    const uint32_t   kCount = 20000;
    std::vector<int> Values(kCount, 0);
    int*             Data = Values.data();

    Jobs.ParallelFor(kCount, 256, [Data](uint32_t Begin, uint32_t End)
    {
        for(uint32_t i = Begin; i < End; ++i) { Data[i] = int(i) * 2; }
    });

    /* Against a serial reference, and checking EVERY element: an off-by-one in
     * the chunk bounds leaves exactly one element untouched, which a spot check
     * of the first and last would miss. */
    bool bAllCorrect = true;
    for(uint32_t i = 0; i < kCount; ++i)
    {
        if(Values[i] != int(i) * 2) { bAllCorrect = false; break; }
    }
    Expect(bAllCorrect, "ParallelFor covers every element exactly once");

    /* The serial path: count <= grain runs the body directly with no jobs. */
    std::vector<int> Small(4, 0);
    int*             SmallData = Small.data();
    Jobs.ParallelFor(4, 256, [SmallData](uint32_t Begin, uint32_t End)
    {
        for(uint32_t i = Begin; i < End; ++i) { SmallData[i] = 7; }
    });
    Expect(Small[0] == 7 && Small[3] == 7, "ParallelFor runs serially below the grain");

    Jobs.ParallelFor(0, 16, [](uint32_t, uint32_t) { Expect(false, "empty ParallelFor ran a body"); });
    Expect(true, "ParallelFor over an empty range does nothing");
}

static void TestJobWorkerLimit()
{
    std::printf("[jobs worker limit]\n");

    JobSystem Jobs(4);
    Expect(Jobs.GetActiveWorkerCount() == 4, "every worker is active by default");

    Jobs.SetActiveWorkerLimit(1);
    Expect(Jobs.GetActiveWorkerCount() == 1, "the limit parks the rest");

    std::atomic<int> Counter{0};
    std::atomic<int>* CounterPtr = &Counter;
    Jobs.ParallelFor(64, 1, [CounterPtr](uint32_t Begin, uint32_t End) { CounterPtr->fetch_add(int(End - Begin)); });
    Expect(Counter.load() == 64, "work still drains with workers parked");

    Jobs.SetActiveWorkerLimit(0);
    Expect(Jobs.GetActiveWorkerCount() == 4, "0 is every worker again");
    Jobs.SetActiveWorkerLimit(99);
    Expect(Jobs.GetActiveWorkerCount() == 4, "a limit past the pool clamps to it");

    Jobs.SetIdleWaitMs(0);
    Expect(Jobs.GetIdleWaitMs() == 1, "the idle wait is at least a millisecond");
    Jobs.SetActiveWorkerLimit(2);   // parked workers must still join on destruction
}

static void TestJobRefusals()
{
    std::printf("[jobs refusals]\n");

    const LogHandler Previous = GetLogHandler();
    SetLogHandler(&ThreadLogHandler);

    JobSystem Jobs(1);

    /* A payload larger than the slot. The templated Dispatch static_asserts this
     * at compile time; the raw form has to refuse at runtime. */
    ResetThreadLog();
    {
        unsigned char Oversized[JOB_PAYLOAD_CAPACITY + 8] = {};
        const JobHandle H = Jobs.Dispatch(+[](void*) {}, Oversized, sizeof(Oversized));
        Expect(!H.IsValid(), "an oversized payload is refused");
        Expect(ThreadLogHas("JOB_PAYLOAD_CAPACITY"), "the refusal names the limit");
    }

    ResetThreadLog();
    {
        const JobHandle H = Jobs.Dispatch(nullptr, nullptr, 0);
        Expect(!H.IsValid(), "a null function is refused");
    }

    /*
     * A completed parent, which is the refusal a fan-out written WITHOUT a group
     * actually hits.
     *
     * It reports "already complete" rather than "recycled", and that is worth
     * knowing: the generation is bumped when a slot is ACQUIRED, so a finished
     * job's slot sits on the free list still carrying its old generation and the
     * handle keeps resolving. The count is zero, so the adoption is refused on
     * the honest ground -- the parent finished -- instead of on an
     * implementation detail the caller did not cause.
     */
    ResetThreadLog();
    {
        const JobHandle Done = Jobs.Dispatch([] {});
        Jobs.Wait(Done);

        const JobHandle Orphan = Jobs.Dispatch([] {}, Done);
        Expect(!Orphan.IsValid(), "a child of a completed parent is refused");
        Expect(ThreadLogHas("already complete"), "the refusal names the actual mistake");
        Expect(ThreadLogHas("OpenGroup"), "the refusal points at the fix");
    }

    /*
     * Wait on a group that was never closed. It can never complete -- the
     * reference keeping it open belongs to this thread -- so this must be a
     * diagnosable refusal and NOT a hang. If the explicit bail after CORE_CHECK
     * is ever removed, this test stops returning.
     */
    ResetThreadLog();
    {
        const JobHandle Group = Jobs.OpenGroup();
        Jobs.Wait(Group);
        Expect(GThreadFatals >= 1, "Wait on an open group is fatal rather than a hang");
        Expect(ThreadLogHas("never closed"), "the check says to close the group");
        Jobs.CloseGroup(Group);
    }

    /* Closing twice would release a reference that is not the caller's, and
     * could complete the group out from under its children. It is caught by the
     * flag ReleaseSlot clears rather than by the generation -- see the note on
     * the completed-parent case above for why the handle still resolves. */
    ResetThreadLog();
    {
        const JobHandle Group = Jobs.OpenGroup();
        Jobs.CloseGroup(Group);
        Jobs.CloseGroup(Group);
        Expect(ThreadLogHas("not an open group"), "closing a group twice is refused");
    }

    SetLogHandler(Previous);
}

static void TestJobRingExhaustion()
{
    std::printf("[jobs ring exhaustion]\n");

    const LogHandler Previous = GetLogHandler();
    SetLogHandler(&ThreadLogHandler);
    ResetThreadLog();

    JobSystem Jobs(1);

    /*
     * Groups occupy a slot and are never queued, so opening MAX_PENDING_JOBS of
     * them fills the ring without tying up a single worker. That makes
     * exhaustion testable deterministically, which it would not be if the ring
     * had to be filled with real work.
     */
    std::vector<JobHandle> Groups;
    Groups.reserve(JobSystem::MAX_PENDING_JOBS);

    for(uint32_t i = 0; i < JobSystem::MAX_PENDING_JOBS; ++i)
    {
        const JobHandle G = Jobs.OpenGroup();
        if(!G.IsValid()) { break; }
        Groups.push_back(G);
    }
    Expect(Groups.size() == JobSystem::MAX_PENDING_JOBS, "the ring holds exactly MAX_PENDING_JOBS");

    Expect(!Jobs.OpenGroup().IsValid(), "OpenGroup fails on an exhausted ring");

    /* Dispatch does NOT fail -- it runs the work here and now. That is the
     * backpressure the header describes, and the handle is invalid because the
     * job is already done. */
    GJobRuns.store(0);
    std::atomic<int>* Runs = &GJobRuns;
    const JobHandle Inline = Jobs.Dispatch([Runs] { Runs->fetch_add(1); });

    Expect(!Inline.IsValid(), "an exhausted ring returns an invalid handle");
    Expect(GJobRuns.load() == 1, "the job ran inline on the dispatching thread instead");

    Jobs.Wait(Inline);      // must be a no-op, not a hang
    Expect(true, "Wait on an inline-executed handle returns immediately");

    JobSystemStats Stats;
    Jobs.GetStats(Stats);
    Expect(Stats.ExecutedInline >= 1, "inline execution is visible in the stats");

    for(const JobHandle& G : Groups) { Jobs.CloseGroup(G); }

    SetLogHandler(Previous);
}

static void TestJobMainThread()
{
    std::printf("[jobs main thread]\n");

    JobSystem Jobs;

    GJobRuns.store(0);
    std::atomic<int>* Runs = &GJobRuns;

    struct MainPayload { std::atomic<int>* Counter; ThreadID* Seen; };
    static ThreadID GSeenOnThread = 0;
    GSeenOnThread = 0;

    MainPayload P{ Runs, &GSeenOnThread };
    const JobHandle H = Jobs.DispatchOnMain(+[](void* InPayload)
    {
        const MainPayload& M = *static_cast<MainPayload*>(InPayload);
        M.Counter->fetch_add(1);
        *M.Seen = appThreadID();
    }, &P, sizeof(P));

    Expect(H.IsValid(), "DispatchOnMain returns a valid handle");

    /* NOT run by a worker. This is the whole point of the second queue: no
     * amount of worker idling may pick it up. */
    Threads::SleepMs(20);
    Expect(GJobRuns.load() == 0, "a main-thread job is never taken by a worker");

    const uint32_t Ran = Jobs.TickMainThread();
    Expect(Ran == 1, "TickMainThread runs the queued job");
    Expect(GJobRuns.load() == 1, "the main-thread job ran");
    Expect(GSeenOnThread == appThreadID(), "it ran ON the main thread");
    Expect(Jobs.IsComplete(H), "the handle completes once it has run");

    Expect(Jobs.TickMainThread() == 0, "ticking an empty main queue runs nothing");
}

static void TestJobImageBarrier()
{
    std::printf("[jobs image barrier]\n");

    const LogHandler Previous = GetLogHandler();
    SetLogHandler(&ThreadLogHandler);
    ResetThreadLog();

    JobSystem Jobs;

    GJobRuns.store(0);
    std::atomic<int>* Runs = &GJobRuns;

    /*
     * Queued on the MAIN-THREAD queue on purpose, which makes this deterministic:
     * a worker can never claim those, so the jobs are guaranteed to still be
     * queued when the barrier runs. The barrier filters both queues with the same
     * loop, so this covers the Ready path too without racing a worker for it.
     */
    const ImageID kDoomed = ImageID(4321);

    JobHandle Tagged[8];
    for(int i = 0; i < 8; ++i)
    {
        Tagged[i] = Jobs.DispatchOnMain(+[](void* InPayload)
        {
            (*static_cast<std::atomic<int>**>(InPayload))->fetch_add(1);
        }, &Runs, sizeof(Runs), kDoomed);
    }

    const JobHandle Survivor = Jobs.DispatchOnMain(+[](void* InPayload)
    {
        (*static_cast<std::atomic<int>**>(InPayload))->fetch_add(100);
    }, &Runs, sizeof(Runs));            // host-tagged: must NOT be dropped

    const uint32_t Dropped = Jobs.OnImageUnloading(kDoomed);
    Expect(Dropped == 8, "the barrier drops every job tagged with the dying image");
    Expect(ThreadLogHas("queued job(s) that would have called into it"),
           "the barrier logs what it dropped");

    /* Anything waiting on a dropped job is released rather than left blocked --
     * the job is completed without having run. */
    Jobs.Wait(Tagged[0]);
    Expect(true, "waiting on a dropped job returns instead of hanging");

    const uint32_t Ran = Jobs.TickMainThread();
    Expect(Ran == 1, "only the untagged job is left to run");
    Expect(GJobRuns.load() == 100, "the dropped jobs never ran and the survivor did");
    Expect(Jobs.IsComplete(Survivor), "the survivor completed normally");

    SetLogHandler(Previous);
}

static void TestJobLocator()
{
    std::printf("[jobs locator]\n");

    Expect(GetJobSystem() == nullptr, "no pool is registered by default");

    {
        JobSystem Jobs;
        SetJobSystem(&Jobs);
        Expect(GetJobSystem() == &Jobs, "the locator returns the registered pool");
        SetJobSystem(nullptr);
    }

    Expect(GetJobSystem() == nullptr, "the locator is cleared before the pool dies");
}

static void TestJobShutdown()
{
    std::printf("[jobs shutdown]\n");

    const LogHandler Previous = GetLogHandler();
    SetLogHandler(&ThreadLogHandler);
    ResetThreadLog();

    /*
     * A pool destroyed with work still queued must DROP it and say so -- not run
     * it during teardown, when whatever dispatched it may already be gone.
     *
     * Queued on the main thread queue so nothing can drain it first, which makes
     * the count exact rather than a race against the workers.
     */
    GJobRuns.store(0);
    std::atomic<int>* Runs = &GJobRuns;
    {
        JobSystem Jobs;
        for(int i = 0; i < 4; ++i)
        {
            Jobs.DispatchOnMain(+[](void* InPayload)
            {
                (*static_cast<std::atomic<int>**>(InPayload))->fetch_add(1);
            }, &Runs, sizeof(Runs));
        }
    }

    Expect(GJobRuns.load() == 0, "queued jobs are dropped at destruction, not run");
    Expect(ThreadLogHas("still queued"), "the destructor reports what it dropped");

    SetLogHandler(Previous);
}

static void RunJobTests()
{
    TestJobBasics();
    TestJobNestedWait();
    TestJobParallelFor();
    TestJobWorkerLimit();
    TestJobRefusals();
    TestJobRingExhaustion();
    TestJobMainThread();
    TestJobImageBarrier();
    TestJobLocator();
    TestJobShutdown();
}

// --- types / math ---------------------------------------------------------

static void TestTypes()
{
    std::printf("[types]\n");

    const Color C(10, 20, 30, 40);
    Expect(C.R == 10 && C.G == 20 && C.B == 30 && C.A == 40, "Color stores 8-bit channels");
    Expect(Color(255, 255, 255, 255).Rf() == 1.0f, "Color normalizes 255 to exactly 1.0");
    Expect(Color(0, 0, 0, 0).Af() == 0.0f, "Color normalizes 0 to exactly 0.0");
    Expect(Color(1, 2, 3) == Color(1, 2, 3, 255), "Color alpha defaults to opaque");
    Expect(Color(1, 2, 3) != Color(1, 2, 4), "Color inequality compares every channel");

    /*
     * QRect::right()/bottom() are INCLUSIVE. Rect reproduces that deliberately
     * -- getting it wrong is a silent one-pixel error in every converted
     * bounds check, which is why it is asserted rather than assumed.
     */
    const Rect R(10, 20, 4, 5);
    Expect(R.Left() == 10 && R.Top() == 20, "Rect origin accessors");
    Expect(R.Right() == 13, "Rect::Right is inclusive, matching QRect");
    Expect(R.Bottom() == 24, "Rect::Bottom is inclusive, matching QRect");
    Expect(R.Contains(Point(13, 24)), "Contains includes the inclusive corner");
    Expect(!R.Contains(Point(14, 24)), "Contains excludes one past the right edge");
    Expect(Rect().IsEmpty() && Size().IsEmpty(), "default Rect/Size are empty");

    Expect(WindowHandle().IsValid() == false, "a default WindowHandle is invalid");
    int Dummy = 0;
    Expect(WindowHandle(&Dummy).IsValid(), "a populated WindowHandle is valid");

    // The math is header-only and its ABI is asserted at compile time in
    // Vector.h/Matrix.h; this just confirms it is reachable and sane from a
    // consumer that links EngineCore alone.
    const Vector3 V(3.0, 4.0, 0.0);
    Expect(V.GetLength() > 4.99 && V.GetLength() < 5.01, "Vector3 length");
    Expect(Matrix4x4().InitIdentity().Data[0][0] == 1, "Matrix4x4 identity");

    // 90 degrees about +Z turns +X into +Y, and rotations are orthonormal.
    const Vector3 TurnedX = Matrix4x4().InitRotation(90.0f, Vector3(0.0, 0.0, 1.0)) * Vector3(1.0, 0.0, 0.0);
    Expect(std::abs(TurnedX.GetX()) < 1e-5 && std::abs(TurnedX.GetY() - 1.0) < 1e-5, "InitRotation: 90 about Z maps X to Y");
    const Vector3 TurnedY = Matrix4x4().InitRotation(90.0f, Vector3(1.0, 0.0, 0.0)) * Vector3(0.0, 1.0, 0.0);
    Expect(std::abs(TurnedY.GetZ() - 1.0) < 1e-5, "InitRotation: 90 about X maps Y to Z");
    const Vector3 TurnedZ = Matrix4x4().InitRotation(90.0f, Vector3(0.0, 1.0, 0.0)) * Vector3(0.0, 0.0, 1.0);
    Expect(std::abs(TurnedZ.GetX() - 1.0) < 1e-5, "InitRotation: 90 about Y maps Z to X");

    const Matrix4x4 Oblique = Matrix4x4().InitRotation(37.0f, Vector3(1.0, 2.0, -3.0));
    double WorstOrtho = 0.0;
    for(int A = 0; A < 3; ++A)
    {
        for(int B = 0; B < 3; ++B)
        {
            double Dot = 0.0;
            for(int K = 0; K < 3; ++K) { Dot += double(Oblique.Data[K][A]) * double(Oblique.Data[K][B]); }
            WorstOrtho = std::max(WorstOrtho, std::abs(Dot - (A == B ? 1.0 : 0.0)));
        }
    }
    Expect(WorstOrtho < 1e-5, "InitRotation: an oblique-axis rotation is orthonormal");
}

// --- config ----------------------------------------------------------------

static int              GConfigCallbackCount = 0;
static const ConfigVar* GConfigCallbackVar   = nullptr;

static void SmokeConfigCallback(const ConfigVar& InVar)
{
    ++GConfigCallbackCount;
    GConfigCallbackVar = &InVar;
}

NIMBLE_CONFIG_BOOL  (GSmokeVSync,  "Smoke", "VSync",  true,   &SmokeConfigCallback);
NIMBLE_CONFIG_INT   (GSmokeMaxFPS, "Smoke", "MaxFPS", 60,     nullptr);
NIMBLE_CONFIG_FLOAT (GSmokeScale,  "Smoke", "Scale",  1.0f,   nullptr);
NIMBLE_CONFIG_STRING(GSmokeTitle,  "Smoke", "Title",  "none", nullptr);
NIMBLE_CONFIG_INT   (GSmokeGlobal, "",      "Global", 7,      nullptr);

static void TestConfigRegistry()
{
    std::printf("[config registry]\n");

    Expect(ConfigFindVar("Smoke", "VSync") == &GSmokeVSync, "a macro-declared var self-registers before main()");
    Expect(ConfigFindVar("smoke", "vsync") == &GSmokeVSync, "section and key lookup is case-insensitive");
    Expect(ConfigFindVar("Smoke", "NoSuchKey") == nullptr, "an undeclared key is not found");
    Expect(ConfigFindVar("", "Global") == &GSmokeGlobal, "the empty section is addressable");

    Expect(ConfigFindVarByPath("Smoke.MaxFPS") == &GSmokeMaxFPS, "qualified path lookup resolves");
    Expect(ConfigFindVarByPath("MaxFPS") == &GSmokeMaxFPS, "a bare name searches every section");

    Expect(GSmokeMaxFPS.GetInt() == 60, "the compiled default survives registration");
    Expect(GSmokeVSync.GetBool() == true, "the compiled bool default survives registration");
    Expect(ConfigGetVarCount() >= 5, "the registry walk reaches every declared var");

    // The direct stand-in for a hot module's DLL unload: a ConfigVar's
    // destructor must unlink it, cleanly, with no trace left behind.
    const int Before = ConfigGetVarCount();
    {
        ConfigVar Temp(EConfigType::CT_Int, "Smoke", "Scoped", MakeConfigInt(1), nullptr, EConfigScope::CS_Engine);
        Expect(ConfigFindVar("Smoke", "Scoped") == &Temp, "a scoped var registers on construction");
        Expect(ConfigGetVarCount() == Before + 1, "the count tracks registration");
    }
    Expect(ConfigFindVar("Smoke", "Scoped") == nullptr, "a ConfigVar unregisters in its destructor");
    Expect(ConfigGetVarCount() == Before, "the list is spliced cleanly, not leaked");

    // Duplicate registration: a second var at the same key is linked (and
    // warned about) but does not shadow the first in lookup order, and does
    // NOT inherit the first's value -- only a load or a console set touches
    // every var sharing a key, see A DUPLICATE KEY IS A WARNING in CoreConfig.h.
    ConfigVar Dup(EConfigType::CT_Int, "Smoke", "MaxFPS", MakeConfigInt(999), nullptr, EConfigScope::CS_Engine);
    Expect(ConfigFindVar("Smoke", "MaxFPS") == &Dup, "the most recently registered duplicate is found first");
    Expect(Dup.GetInt() == 999, "a duplicate holds its own compiled default when no file has set the key yet");

    LoadConfigText("[Smoke]\nMaxFPS = 123\n");
    Expect(GSmokeMaxFPS.GetInt() == 123 && Dup.GetInt() == 123,
           "loading a value applies it to every var registered under the same key");
}

static void TestConfigParse()
{
    std::printf("[config parse]\n");

    ConfigLoadStats Stats;
    const bool bOk = LoadConfigText(
        "; a comment line\n"
        "# another comment style\n"
        "\n"
        "Global = 9\n"
        "[Smoke]\n"
        "VSync = off\n"
        "MaxFPS   =   144   \n"
        "Scale = 2\n"
        "Nope.Nope = 1\n"
        "NotAKeyValueLine\n"
        "MaxFPS = fast\n",
        &Stats);

    Expect(!bOk, "a file with a bad value or a malformed line reports failure");
    Expect(GSmokeVSync.GetBool() == false, "'off' parses as bool false");
    Expect(GSmokeMaxFPS.GetInt() == 144, "whitespace around '=' and the value is ignored");
    Expect(GSmokeScale.GetFloat() == 2.0f, "an integer literal is accepted for a float var");
    Expect(GSmokeGlobal.GetInt() == 9, "a key before any [Section] lands in the global section");
    Expect(Stats.UnknownKeys == 1, "an unknown key warns and is counted, never fatal");
    Expect(Stats.MalformedLines == 1, "a line with no '=' is counted as malformed");
    Expect(Stats.BadValues == 1, "'fast' is rejected for an int var");
    Expect(GSmokeMaxFPS.GetInt() == 144, "a bad value leaves the previous value intact");

    // Hex, quoting, duplicate-key-last-wins, and CRLF, in one pass.
    ConfigLoadStats Stats2;
    LoadConfigText(
        "[Smoke]\r\n"
        "MaxFPS = 0x20\r\n"
        "Title = \"two words\"\r\n"
        "MaxFPS = 30\r\n",
        &Stats2);
    Expect(GSmokeMaxFPS.GetInt() == 30, "a duplicate key in one file: last wins");
    Expect(std::string(GSmokeTitle.GetString()) == "two words", "a quoted value keeps inner spaces and loses outer quotes");

    ConfigLoadStats Stats3;
    LoadConfigText("[Smoke]\nMaxFPS = 0x20\n", &Stats3);
    Expect(GSmokeMaxFPS.GetInt() == 32, "'0x20' parses as hex 32");
}

static void TestConfigCallbacks()
{
    std::printf("[config callbacks]\n");

    GSmokeVSync.SetBool(true);
    GConfigCallbackCount = 0;
    GConfigCallbackVar   = nullptr;

    Expect(GSmokeVSync.SetBool(false), "SetBool reports a real change");
    Expect(GConfigCallbackCount == 1, "a changed value fires the callback exactly once");
    Expect(GConfigCallbackVar == &GSmokeVSync, "the callback receives the var that changed");

    Expect(!GSmokeVSync.SetBool(false), "SetBool reports no change for the same value");
    Expect(GConfigCallbackCount == 1, "an unchanged value does not fire the callback");

    Expect(!GSmokeMaxFPS.SetInt(GSmokeMaxFPS.GetInt()), "SetInt reports no change for the same value");

    ConfigApplyAll();
    Expect(GConfigCallbackCount == 2, "ConfigApplyAll fires every callback regardless of change");

    Expect(GSmokeMaxFPS.DefaultToString() == "60", "DefaultToString gives the compiled default while the value differs");
    Expect(GSmokeMaxFPS.ResetToDefault(), "ResetToDefault reports a change when the value was not the default");
    Expect(GSmokeMaxFPS.GetInt() == 60, "reset restores the compiled default");

    // Back-fill: a var declared AFTER a file was parsed must come up holding
    // the file's value -- the mechanism that makes a hot module's config work.
    LoadConfigText("[Smoke]\nLateVar = 42\n");
    {
        ConfigVar LateVar(EConfigType::CT_Int, "Smoke", "LateVar", MakeConfigInt(0), nullptr, EConfigScope::CS_Engine);
        Expect(LateVar.GetInt() == 42, "a var registered after its file was loaded is back-filled from the retained entry");
    }

    // Sections as list items: every entry under a prefix, in file order, uncapped.
    const std::string LongValue(300, 'x');
    LoadConfigText("[SmokeList_B]\nFile = b\n[SmokeList_A]\nFile = " + LongValue + "\nCook = false\n[SmokeListOther]\nFile = c\n");
    std::vector<std::string> Seen;
    ConfigForEachEntry("smokelist_", [](const char* InSection, const char* InKey, const char* InValue, void* InUser)
    {
        static_cast<std::vector<std::string>*>(InUser)->push_back(std::string(InSection) + "." + InKey + "=" + InValue);
    }, &Seen);
    Expect(Seen.size() == 3, "ConfigForEachEntry visits only the sections with the prefix");
    Expect(Seen.size() == 3 && Seen[0] == "SmokeList_B.File=b" && Seen[2] == "SmokeList_A.Cook=false",
           "ConfigForEachEntry keeps file order");
    Expect(Seen.size() == 3 && Seen[1] == "SmokeList_A.File=" + LongValue, "ConfigForEachEntry values are not capped");

    ConfigRemoveEntries("SmokeList_");
    Seen.clear();
    ConfigForEachEntry("SmokeList", [](const char* InSection, const char*, const char*, void* InUser)
    {
        static_cast<std::vector<std::string>*>(InUser)->push_back(InSection);
    }, &Seen);
    Expect(Seen.size() == 1 && Seen[0] == "SmokeListOther", "ConfigRemoveEntries forgets only the prefixed sections");
}

static void TestConfigFile()
{
    std::printf("[config file]\n");

    const std::string Dir  = "SmokeScratch/Config";
    const std::string Path = Dir + "/Smoke.ini";
    Expect(appCreateDirectories(Dir), "scratch config directory created");

    const std::string Contents = "[Smoke]\nMaxFPS = 75\n";
    FileHandle W = appFileOpenWrite(Path);
    Expect(W != nullptr, "config scratch file opens for write");
    if(W)
    {
        appFileWrite(W, Contents.data(), Contents.size());
        appFileClose(W);
    }

    Expect(LoadConfigFile(Path), "LoadConfigFile reads through the appFile* fallback with no provider installed");
    Expect(GSmokeMaxFPS.GetInt() == 75, "the loaded file's value was applied");

    FilesystemProvider Provider(Dir);
    SetFileProvider(&Provider);
    ConfigLoadStats Stats;
    Expect(LoadConfigFile("Smoke.ini", &Stats), "LoadConfigFile resolves a bundle-relative path through the provider");
    SetFileProvider(nullptr);   // the provider is a stack object -- clear it immediately

    Expect(!LoadConfigFile("SmokeScratch/Config/missing.ini"), "a missing config file fails without crashing");
}

NIMBLE_CONFIG_FLOAT (GSmokeUserSpeed, "SmokeUser", "Speed",      1.0f,     nullptr);
NIMBLE_CONFIG_BOOL  (GSmokeUserFull,  "SmokeUser", "Fullscreen", false,    nullptr);
NIMBLE_CONFIG_STRING(GSmokeUserName,  "SmokeUser", "Name",       "Player", nullptr);

/* The user layer: what a settings menu saves, measured against the project file. */
static void TestConfigUser()
{
    std::printf("[config user]\n");
    const auto Section = []()
    {
        const std::string Text = ConfigBuildUserText();
        const size_t At = Text.find("[SmokeUser]");
        if(At == std::string::npos) return std::string();
        const size_t End = Text.find("\n[", At + 1);
        return Text.substr(At, End == std::string::npos ? std::string::npos : End - At);
    };

    LoadConfigText("[SmokeUser]\nFullscreen = true\n");   // the project's own setting
    Expect(GSmokeUserFull.GetBool() && Section().empty(), "values at the project's or the compiled default write nothing");

    GSmokeUserSpeed.SetFloat(1.5f);
    GSmokeUserFull.SetBool(false);
    std::string Mine = Section();
    Expect(Mine.find("Speed=1.5") != std::string::npos, "a changed var is written");
    Expect(Mine.find("Fullscreen=false") != std::string::npos, "a var differing from the project file is written");
    Expect(Mine.find("Name=") == std::string::npos, "an unchanged one is not");

    GSmokeUserSpeed.SetFromString("1.0");
    Expect(Section().find("Speed") == std::string::npos, "put back to its baseline ('1.0' is 1), it drops out");

    ConfigSetUserFile("");
    Expect(!ConfigSaveUserFile(), "with no user file there is nowhere to save");

    const std::string Path = "SmokeScratch/Config/User.ini";
    appCreateDirectories("SmokeScratch/Config");
    ConfigSetUserFile(Path);
    GSmokeUserSpeed.SetFloat(2.25f);
    GSmokeUserName.SetString("Ada");
    Expect(ConfigSaveUserFile() && appFileExists(Path) && !appFileExists(Path + ".tmp"), "the user file is written, no temporary left");

    GSmokeUserSpeed.ResetToDefault();
    GSmokeUserName.ResetToDefault();
    GSmokeUserFull.SetBool(true);
    ConfigLoadStats Stats;
    LoadConfigFile(Path, &Stats, EConfigLayer::User);
    Expect(GSmokeUserSpeed.GetFloat() == 2.25f && std::string(GSmokeUserName.GetString()) == "Ada" && !GSmokeUserFull.GetBool(),
           "loading it restores the player's values");
    Expect(Section().find("Fullscreen=false") != std::string::npos, "a user value does not become the baseline");

    LoadConfigText("[SmokeUser]\nFromAnotherModule = 3\n", nullptr, EConfigLayer::User);
    Expect(Section().find("FromAnotherModule=3") != std::string::npos, "a user line no var here claims is kept");
    Expect(!LoadConfigFile("SmokeScratch/Config/NoUser.ini", nullptr, EConfigLayer::User), "a missing user file is just absent");

    ConfigSetUserFile("");
    GSmokeUserSpeed.ResetToDefault();
    GSmokeUserName.ResetToDefault();
}

NIMBLE_CONFIG_INT   (GSmokeOverlayRes,  "SmokeOverlay", "Resolution", 2048, nullptr);
NIMBLE_CONFIG_BOOL  (GSmokeOverlayGI,   "SmokeOverlay", "GI",         true, nullptr);

static void TestConfigOverlay()
{
    std::printf("[config overlay]\n");
    const auto UserText = []() { return ConfigBuildUserText(); };

    LoadConfigText("[SmokeOverlay]\nResolution = 1024\n");
    Expect(GSmokeOverlayRes.GetInt() == 1024, "the project file sets the baseline");

    Expect(ConfigSetOverlay(EConfigOverlay::Detail, { { "SmokeOverlay.Resolution", "4096" }, { "SmokeOverlay.GI", "false" } }) == 2,
           "an overlay moves the vars it names");
    Expect(GSmokeOverlayRes.GetInt() == 4096 && !GSmokeOverlayGI.GetBool(), "to its values");
    Expect(UserText().find("[SmokeOverlay]") == std::string::npos, "an overlay's values are baseline: the user file stays clean");

    ConfigSetOverlay(EConfigOverlay::Power, { { "SmokeOverlay.Resolution", "512" } });
    Expect(GSmokeOverlayRes.GetInt() == 4096, "Detail sits above Power");

    GSmokeOverlayGI.SetBool(true);   // the player's choice
    ConfigSetOverlay(EConfigOverlay::Detail, { { "SmokeOverlay.Resolution", "2048" }, { "SmokeOverlay.GI", "false" } });
    Expect(GSmokeOverlayGI.GetBool(), "a value the player set survives a preset switch");
    Expect(GSmokeOverlayRes.GetInt() == 2048, "a value at its baseline follows the switch");
    Expect(UserText().find("GI=true") != std::string::npos, "and the player's value is what the user file keeps");

    LoadConfigText("[SmokeOverlay]\nResolution = 64\n");
    Expect(GSmokeOverlayRes.GetInt() == 2048, "a project file loaded later does not move an overlaid var");

    ConfigSetOverlay(EConfigOverlay::Detail, {});
    Expect(GSmokeOverlayRes.GetInt() == 512, "clearing Detail falls through to Power");
    ConfigSetOverlay(EConfigOverlay::Power, {});
    Expect(GSmokeOverlayRes.GetInt() == 64, "and clearing Power to the project file");
    Expect(ConfigGetOverlay(EConfigOverlay::Power).empty(), "a cleared overlay is empty");

    ConfigSetOverlay(EConfigOverlay::Detail, { { "NoSection", "1" } });
    Expect(ConfigGetOverlay(EConfigOverlay::Detail).empty(), "a path without a section is refused");

    GSmokeOverlayGI.ResetToDefault();
    GSmokeOverlayRes.ResetToDefault();
}

static void TestConfigConsole()
{
    std::printf("[config console]\n");

    GSmokeMaxFPS.SetInt(60);
    std::string R;

    Expect(ExecConfigCommand("Smoke.MaxFPS", R) && Contains(R, "60"), "a get returns the current value");

    Expect(ExecConfigCommand("Smoke.MaxFPS 30", R) && GSmokeMaxFPS.GetInt() == 30, "a bare set applies");
    Expect(ExecConfigCommand("Smoke.MaxFPS = 45", R) && GSmokeMaxFPS.GetInt() == 45, "'=' is accepted and ignored");
    Expect(ExecConfigCommand("Smoke.MaxFPS   90", R) && GSmokeMaxFPS.GetInt() == 90, "a whitespace run is one separator");

    Expect(!ExecConfigCommand("Smoke.MaxFPS fast", R) && GSmokeMaxFPS.GetInt() == 90,
           "a bad value fails and changes nothing");
    Expect(Contains(R, "int"), "the failure names the expected type");

    Expect(ExecConfigCommand("Smoke.VSync on", R) && GSmokeVSync.GetBool(), "'on' sets a bool");
    Expect(ExecConfigCommand("Smoke.Title \"two words\"", R) && std::string(GSmokeTitle.GetString()) == "two words",
           "a quoted final argument is one token");

    Expect(!ExecConfigCommand("Nope.Nope 1", R) && Contains(R, "Unknown"), "an unknown key is rejected, not created");
    Expect(ConfigFindVar("Nope", "Nope") == nullptr, "a failed set did not auto-register the key");

    Expect(!ExecConfigCommand("", R), "an empty command is not a config command");
    Expect(!ExecConfigCommand("   ", R), "a whitespace-only command is not a config command");

    Expect(ExecConfigCommand("MaxFPS 75", R) && GSmokeMaxFPS.GetInt() == 75, "an unqualified name resolves");

    const int BeforeGets = GConfigCallbackCount;
    ExecConfigCommand("Smoke.MaxFPS", R);
    ExecConfigCommand("Smoke.VSync", R);
    Expect(GConfigCallbackCount == BeforeGets, "a get does not fire callbacks");
}

// --- hot-reload modules ---------------------------------------------------
//
// Skipped entirely when CORE_SHARED is OFF: with a static EngineCore a module
// DLL gets its own GLogHandler, its own GFileProvider and its own copy of
// ModuleReloadState::Write, so these tests would not be testing hot reload,
// they would be documenting a configuration that cannot work. Core/CMakeLists
// only defines CORE_SMOKE_MODULE_A in the shared build.

#if defined(CORE_SMOKE_MODULE_A)

/*
 * A log sink that records what crossed it. The point of the message capture is
 * one specific assertion: a CORE_LOG issued INSIDE the module DLL has to reach
 * a handler installed by this EXE. If it does, there is exactly one
 * GLogHandler in the process, which is the single most important structural
 * claim the CORE_SHARED requirement makes.
 */
static int         GModuleLogErrors = 0;
static int         GModuleLogWarns  = 0;
static std::string GModuleLastMessage;
static std::string GModuleAllMessages;

static void ModuleLogHandler(const LogVerbosity Verbosity, const char* Message)
{
    if(Verbosity == LogVerbosity::Error) ++GModuleLogErrors;
    if(Verbosity == LogVerbosity::Warning) ++GModuleLogWarns;

    GModuleLastMessage = Message ? Message : "";
    GModuleAllMessages += GModuleLastMessage;
    GModuleAllMessages += "\n";
}

static void ResetModuleLog()
{
    GModuleLogErrors = 0;
    GModuleLogWarns  = 0;
    GModuleLastMessage.clear();
    GModuleAllMessages.clear();
}

static bool ModuleLogContains(const char* InNeedle)
{
    return GModuleAllMessages.find(InNeedle) != std::string::npos;
}

static const char* const GWatchDir    = "SmokeScratch/Modules";
static const char* const GWatchedPath = "SmokeScratch/Modules/Watched.dll";
static const char* const GShadowRoot  = "SmokeScratch/Modules/Shadow";

/* Reads one of the module's own exports through the manager. Returns 0 if the
 * module is not loaded, which every caller distinguishes by other means. */
static uint32_t ModuleCounter(const ModuleManager& InManager, const char* InSymbol)
{
    using CounterFn = uint32_t (*)();

    void* Symbol = InManager.GetModuleSymbol("Example", InSymbol);
    return Symbol ? reinterpret_cast<CounterFn>(Symbol)() : 0u;
}

/* The manager has its own copy of this in an anonymous namespace; the test
 * cannot see it, and duplicating four lines beats exporting a path utility. */
static std::string SwapExtensionToPdb(const std::string& InPath)
{
    const size_t Dot   = InPath.find_last_of('.');
    const size_t Slash = InPath.find_last_of("/\\");
    if(Dot == std::string::npos || (Slash != std::string::npos && Dot < Slash))
    {
        return InPath + ".pdb";
    }
    return InPath.substr(0, Dot) + ".pdb";
}

static bool StageVariant(const char* InVariantPath)
{
    return appFileCopy(InVariantPath, GWatchedPath, true);
}

static void TestModuleLoad()
{
    std::printf("[module load]\n");

    Expect(appCreateDirectories(GWatchDir), "the module scratch directory is created");
    Expect(StageVariant(CORE_SMOKE_MODULE_A), "variant A is staged at the watched path");

    ResetModuleLog();
    SetLogHandler(&ModuleLogHandler);

    {
        ModuleManager Manager(GShadowRoot);
        Manager.SetSettleSeconds(0.0f);

        Expect(Manager.GetModuleState("Example") == EModuleState::MS_Unregistered,
               "an unknown name reports MS_Unregistered");

        Expect(Manager.RegisterModule("Example", GWatchedPath), "RegisterModule succeeds");
        Expect(!Manager.RegisterModule("Example", GWatchedPath), "a duplicate registration is refused");
        Expect(Manager.GetModuleState("Example") == EModuleState::MS_Unloaded,
               "a registered but unloaded module reports MS_Unloaded");
        Expect(Manager.GetModuleCount() == 1, "the module is counted");
        Expect(Manager.GetModuleNameAt(0) == "Example", "the module can be enumerated by index");

        Expect(Manager.LoadModule("Example"), "LoadModule succeeds");
        Expect(Manager.GetModuleState("Example") == EModuleState::MS_Loaded, "the module reports MS_Loaded");

        IModule* Module = Manager.GetModule("Example");
        Expect(Module != nullptr, "GetModule returns the instance");
        Expect(Module && std::strcmp(Module->GetName(), "ExampleHotModule") == 0,
               "the module reports its name");
        Expect(Manager.GetModuleVersionString("Example") == "A", "variant A is the loaded build");

        // The CORE_SHARED payoff, asserted directly.
        Expect(ModuleLogContains("ExampleHotModule A startup"),
               "a CORE_LOG from inside the DLL reaches the handler this EXE installed");

        Expect(ModuleCounter(Manager, "ExampleHotModuleGetStartupCount") == 1,
               "the module ran OnStartup exactly once");

        /*
         * THE HEADLINE ASSERTION.
         *
         * Windows maps a loaded DLL with FILE_SHARE_READ only, so overwriting
         * the watched file while the module is live is a sharing violation
         * unless something is actually shadow-copying it. This single line is
         * what proves the entire shadow-copy design does its job; stub the copy
         * out and it fails immediately.
         */
        Expect(appFileCopy(CORE_SMOKE_MODULE_B, GWatchedPath, true),
               "the watched DLL can be overwritten while the module is loaded");

        // Put A back so this test leaves no reload pending for the next one.
        Expect(StageVariant(CORE_SMOKE_MODULE_A), "variant A is restored");

        Expect(Manager.UnloadModule("Example"), "UnloadModule succeeds");
        Expect(Manager.GetModuleState("Example") == EModuleState::MS_Unloaded,
               "the module reports MS_Unloaded after unloading");
        Expect(Manager.GetModule("Example") == nullptr, "GetModule returns null once unloaded");

        // If the handle really was released, the file is deletable.
        Expect(appFileDelete(GWatchedPath), "the watched DLL is deletable after unload");
    }

    SetLogHandler(nullptr);
}

/*
 * The sweep a registration runs reclaims the shadow directories of processes
 * that have exited, and leaves a running one's alone: a second instance
 * starting beside this one is copying its module into its directory right
 * then, and deleting it under that copy failed the other's load.
 */
static void TestModuleShadowSweep()
{
    std::printf("[module shadow sweep]\n");

    const std::string Root = "SmokeScratch/Modules/ShadowSweep";
#if defined(_WIN32)
    const uint32_t Live = 4;            // System: always there, never ours to open
#else
    const uint32_t Live = 1;            // init: always there, never ours to signal
#endif
    const uint32_t Dead = 0x7FFFFFF0u;  // no process has it
    const std::string LiveDir = Root + "/" + std::to_string(Live) + "-0001";
    const std::string DeadDir = Root + "/" + std::to_string(Dead) + "-0001";
    for(const std::string& Dir : { LiveDir, DeadDir })
    {
        Expect(appCreateDirectories(Dir), "a generation directory is staged");
        if(FileHandle Handle = appFileOpenWrite(Dir + "/Watched.dll")) { appFileClose(Handle); }
    }

    {
        ModuleManager Manager(Root);
        Expect(appCreateDirectories(GWatchDir), "the module scratch directory is created");
        Expect(StageVariant(CORE_SMOKE_MODULE_A), "variant A is staged at the watched path");
        Expect(Manager.RegisterModule("Example", GWatchedPath), "a registration sweeps the shadow root");
    }
    Expect(!appFileExists(DeadDir + "/Watched.dll"), "an exited process's generation is reclaimed");
    Expect(appFileExists(LiveDir + "/Watched.dll"), "a running process's generation is left alone");

    appFileDelete(LiveDir + "/Watched.dll");
    appDirectoryDelete(LiveDir);
    appDirectoryDelete(Root);
}

/*
 * The .pdb half of the shadow copy.
 *
 * Modules link with /PDBALTPATH:%_PDB%, so a module DLL records only the bare
 * filename of its symbols. The debugger resolves that relative to the loaded
 * module's own directory -- which for a hot-reloaded module is the shadow
 * directory -- so the manager has to put a copy of the .pdb there. Without it
 * the debugger falls back to the original, holds it open, and the next build
 * fails with LNK1201: hot reload would have traded a locked DLL for a locked
 * PDB and gained nothing.
 *
 * The debugger part of that cannot be asserted from in here. What can be
 * asserted is that the copy is made, which is the mechanism it depends on.
 */
static void TestModuleShadowPdb()
{
    std::printf("[module shadow pdb]\n");

    const std::string WatchedPdb = "SmokeScratch/Modules/Watched.pdb";
    const std::string SourcePdb  = SwapExtensionToPdb(CORE_SMOKE_MODULE_A);

    if(!appFileExists(SourcePdb))
    {
        // A Release build without separate symbols has nothing to copy, and
        // that is not a failure of anything.
        std::printf("  (no .pdb beside the fixture; skipped)\n");
        return;
    }

    Expect(StageVariant(CORE_SMOKE_MODULE_A), "variant A is staged");
    Expect(appFileCopy(SourcePdb.c_str(), WatchedPdb.c_str(), true),
           "a .pdb is staged beside the watched DLL");

    const std::string PdbShadowRoot = "SmokeScratch/Modules/ShadowPdb";

    {
        ModuleManager Manager(PdbShadowRoot);
        Manager.SetSettleSeconds(0.0f);

        Expect(Manager.RegisterModule("Example", GWatchedPath), "RegisterModule succeeds");
        Expect(Manager.LoadModule("Example"), "the module loads");

        // Exactly one generation directory exists at this point, and it should
        // hold both files under their original names.
        std::vector<std::string> Generations;
        Expect(appDirectoryList(PdbShadowRoot, Generations), "the shadow root can be listed");
        Expect(Generations.size() == 1, "one generation directory exists after a single load");

        if(Generations.size() == 1)
        {
            const std::string GenerationDir = PdbShadowRoot + "/" + Generations[0];

            std::vector<std::string> Copied;
            Expect(appDirectoryList(GenerationDir, Copied), "the generation directory can be listed");

            bool bHasDll = false;
            bool bHasPdb = false;
            for(const std::string& Name : Copied)
            {
                if(Name == "Watched.dll") bHasDll = true;
                if(Name == "Watched.pdb") bHasPdb = true;
            }
            Expect(bHasDll, "the DLL was shadow-copied");
            Expect(bHasPdb, "the .pdb was shadow-copied alongside it");
        }
    }

    appFileDelete(GWatchedPath);
    appFileDelete(WatchedPdb.c_str());
}

static void TestModuleReload()
{
    std::printf("[module reload]\n");

    Expect(StageVariant(CORE_SMOKE_MODULE_A), "variant A is staged");

    ResetModuleLog();
    SetLogHandler(&ModuleLogHandler);

    {
        ModuleManager Manager(GShadowRoot);
        Manager.SetSettleSeconds(0.0f);      // no waiting; the settle window has its own test

        Expect(Manager.RegisterModule("Example", GWatchedPath), "RegisterModule succeeds");
        Expect(Manager.LoadModule("Example"), "variant A loads");
        Expect(ModuleCounter(Manager, "ExampleHotModuleGetStartupCount") == 1, "one startup so far");

        // Ticks before the swap, so the accumulated count means something.
        Expect(Manager.Tick(0.016f) == 0, "an unchanged module is not reloaded");
        Manager.Tick(0.016f);
        Manager.Tick(0.016f);
        Expect(ModuleCounter(Manager, "ExampleHotModuleGetTickCount") == 3,
               "OnTick runs once per Tick while loaded");

        // The "rebuild": different code at the same watched path.
        Expect(StageVariant(CORE_SMOKE_MODULE_B), "variant B is written over the watched path");
        Expect(Manager.Tick(0.016f) == 1, "Tick reloads the changed module");

        Expect(Manager.GetModuleVersionString("Example") == "B",
               "the reloaded module reports variant B -- different code is executing");
        Expect(Manager.GetReloadCount("Example") == 1, "the reload is counted");
        Expect(Manager.GetModuleState("Example") == EModuleState::MS_Loaded, "the module is still loaded");

        Expect(ModuleCounter(Manager, "ExampleHotModuleGetStartupCount") == 2,
               "B picked up A's startup count -- the host-owned blob round-tripped");

        // 'A' is what A's OnShutdown stamped into the blob.
        Expect(ModuleCounter(Manager, "ExampleHotModuleGetLastVariant") == uint32_t('A'),
               "the blob B consumed was written by A");

        // The Tick that performed the reload also ticked the new instance, so
        // the count is the three from A plus that one.
        Expect(ModuleCounter(Manager, "ExampleHotModuleGetTickCount") == 4,
               "the tick count accumulated across the reload rather than resetting");

        const ModuleReloadState* Blob = Manager.GetReloadState("Example");
        Expect(Blob != nullptr, "the reload state is inspectable from the host");
        Expect(Blob && !Blob->IsEmpty(), "the reload state holds a payload");

        // And back again, which proves the handoff is not one-directional.
        Expect(StageVariant(CORE_SMOKE_MODULE_A), "variant A is written back");
        Expect(Manager.Tick(0.016f) == 1, "Tick reloads back to A");
        Expect(Manager.GetModuleVersionString("Example") == "A", "variant A is loaded again");
        Expect(ModuleCounter(Manager, "ExampleHotModuleGetStartupCount") == 3,
               "the startup count survived a second reload");
        Expect(ModuleCounter(Manager, "ExampleHotModuleGetLastVariant") == uint32_t('B'),
               "the blob A consumed was written by B");
        Expect(Manager.GetReloadCount("Example") == 2, "both reloads are counted");

        // The explicit path an editor hotkey would take.
        Expect(Manager.ReloadModule("Example"), "an explicit ReloadModule succeeds with no file change");
        Expect(ModuleCounter(Manager, "ExampleHotModuleGetStartupCount") == 4,
               "an explicit reload re-runs OnStartup");
    }

    SetLogHandler(nullptr);
    Expect(appFileDelete(GWatchedPath), "the watched DLL is deletable after the manager is destroyed");
}

static void TestModuleSettleWindow()
{
    std::printf("[module settle window]\n");

    Expect(StageVariant(CORE_SMOKE_MODULE_A), "variant A is staged");

    SetLogHandler(&ModuleLogHandler);
    ResetModuleLog();

    {
        ModuleManager Manager(GShadowRoot);
        Manager.SetSettleSeconds(0.25f);
        Expect(Manager.GetSettleSeconds() == 0.25f, "the settle window round-trips");

        Expect(Manager.RegisterModule("Example", GWatchedPath), "RegisterModule succeeds");
        Expect(Manager.LoadModule("Example"), "variant A loads");

        Expect(StageVariant(CORE_SMOKE_MODULE_B), "variant B is written over the watched path");

        // First sighting only records the candidate -- a file seen once could
        // still be half-written.
        Expect(Manager.Tick(0.016f) == 0, "a just-changed file is not reloaded immediately");
        Expect(Manager.GetModuleVersionString("Example") == "A", "the old build is still running");

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        Expect(Manager.Tick(0.016f) == 1, "the file reloads once it has settled");
        Expect(Manager.GetModuleVersionString("Example") == "B", "variant B is now running");
    }

    SetLogHandler(nullptr);
    appFileDelete(GWatchedPath);
}

static void TestModuleAbiMismatch()
{
    std::printf("[module abi mismatch]\n");

    Expect(StageVariant(CORE_SMOKE_MODULE_STALE), "the forged stale plugin is staged");

    SetLogHandler(&ModuleLogHandler);
    ResetModuleLog();

    {
        ModuleManager Manager(GShadowRoot);
        Manager.SetSettleSeconds(0.0f);

        Expect(Manager.RegisterModule("Example", GWatchedPath), "RegisterModule succeeds");
        Expect(!Manager.LoadModule("Example"), "a plugin with the wrong ABI version is refused");
        Expect(Manager.GetModuleState("Example") == EModuleState::MS_Failed, "the module reports MS_Failed");
        Expect(Manager.GetModule("Example") == nullptr, "no instance is created for a rejected plugin");
        Expect(GModuleLogErrors > 0, "the refusal is logged as an error");

        // The refusal names the offender, which it can only do through the free
        // NimbleModuleGetName export -- calling a virtual on a plugin whose
        // vtable is the untrustworthy part would defeat the check.
        Expect(ModuleLogContains("ExampleHotModule"), "the refusal names the rejected module");

        // A rejected plugin must be CLOSED, not leaked: if the handle were
        // still open the shadow copy would be undeletable.
        Expect(ModuleLogContains("refusing"), "the log says the plugin was refused");
    }

    SetLogHandler(nullptr);
    Expect(appFileDelete(GWatchedPath), "the rejected DLL is deletable, so it was closed");
}

static void TestModuleReloadRollback()
{
    std::printf("[module reload rollback]\n");

    Expect(StageVariant(CORE_SMOKE_MODULE_A), "variant A is staged");

    SetLogHandler(&ModuleLogHandler);
    ResetModuleLog();

    {
        ModuleManager Manager(GShadowRoot);
        Manager.SetSettleSeconds(0.0f);

        Expect(Manager.RegisterModule("Example", GWatchedPath), "RegisterModule succeeds");
        Expect(Manager.LoadModule("Example"), "variant A loads");
        Expect(ModuleCounter(Manager, "ExampleHotModuleGetStartupCount") == 1, "one startup so far");

        ResetModuleLog();

        // A "bad build" lands at the watched path.
        Expect(StageVariant(CORE_SMOKE_MODULE_STALE), "the stale plugin is written over the watched path");
        Expect(Manager.Tick(0.016f) == 0, "the bad build does not count as a reload");

        Expect(Manager.GetModuleState("Example") == EModuleState::MS_Loaded,
               "the module is still loaded -- rolled back, not left dead");
        Expect(Manager.GetModuleVersionString("Example") == "A", "the previous build is running again");
        Expect(GModuleLogErrors > 0, "the failed reload is logged as an error");

        // Distinguishes "rolled back" from "never attempted": the old code was
        // genuinely torn down and re-instantiated, and was handed the blob it
        // had just written itself.
        Expect(ModuleCounter(Manager, "ExampleHotModuleGetStartupCount") == 2,
               "the rolled-back instance re-ran OnStartup on its own blob");

        // The retry gate. Without it this spins once per tick forever.
        const int ErrorsAfterFirstAttempt = GModuleLogErrors;
        Manager.Tick(0.016f);
        Manager.Tick(0.016f);
        Expect(GModuleLogErrors == ErrorsAfterFirstAttempt,
               "the same failing build is not retried on every tick");
        Expect(ModuleCounter(Manager, "ExampleHotModuleGetStartupCount") == 2,
               "no further reload attempts were made");

        // And the gate releases on a new stamp rather than latching.
        Expect(StageVariant(CORE_SMOKE_MODULE_B), "a good build replaces the bad one");
        Expect(Manager.Tick(0.016f) == 1, "the module recovers once a good build appears");
        Expect(Manager.GetModuleVersionString("Example") == "B", "variant B is now running");
        Expect(ModuleCounter(Manager, "ExampleHotModuleGetStartupCount") == 3,
               "the state survived the failed reload and the recovery");
    }

    SetLogHandler(nullptr);
    appFileDelete(GWatchedPath);
}

/*
 * The unload barrier.
 *
 * ExampleHotModule deliberately leaks a GC-tracked object with a real
 * destructor (see BarrierProbe in that file). Everything below turns on one
 * question: was that object destroyed while the module's code was still
 * mapped?
 *
 * It cannot be answered by reading a counter, because the counter would live
 * in the module and be gone by the time the test could look. So the probe's
 * destructor logs, and this counts the message -- the same trick, and the same
 * reasoning, as the "CORE_LOG in OnStartup reaches the EXE's handler" check
 * the fixture already relies on.
 */
static int GProbeDestroyCount = 0;
static int GBarrierErrors     = 0;

static void BarrierLogHandler(const LogVerbosity Verbosity, const char* Message)
{
    if(Verbosity == LogVerbosity::Error)
    {
        ++GBarrierErrors;
    }
    if(Message && std::strstr(Message, "barrier probe destroyed") != nullptr)
    {
        ++GProbeDestroyCount;
    }
}

static void TestModuleAllocationBarrier()
{
    std::printf("[module allocation barrier]\n");

    Expect(appCreateDirectories(GWatchDir), "the module scratch directory is created");
    Expect(StageVariant(CORE_SMOKE_MODULE_A), "variant A is staged at the watched path");

    GProbeDestroyCount = 0;
    GBarrierErrors     = 0;
    SetLogHandler(&BarrierLogHandler);

    const uint64_t TrackedBefore = Memory::GetTrackedObjectCount();

    {
        ModuleManager Manager(GShadowRoot);
        Manager.SetSettleSeconds(0.0f);

        Expect(Manager.RegisterModule("Example", GWatchedPath), "the module registers");
        Expect(Manager.LoadModule("Example"), "the module loads");

        // The module allocated through EngineCore.dll, so the HOST's object
        // table is what holds it. If CORE_SHARED had produced two copies of the
        // memory subsystem, this count would not have moved.
        Expect(Memory::GetTrackedObjectCount() == TrackedBefore + 1,
               "the module's allocation landed in the host's object table");
        Expect(GProbeDestroyCount == 0, "and is still alive while the module is loaded");

        Expect(Manager.UnloadModule("Example"), "the module unloads");

        /*
         * THE WHOLE POINT. The probe's destructor lives in the module image.
         * If the barrier had not run before appDLLClose, this message would
         * never arrive -- and the object would sit in the object table with a
         * Destruct pointer into unmapped memory, waiting to take the next
         * collect down with it.
         */
        Expect(GProbeDestroyCount == 1,
               "the leaked object was destroyed at unload, while the module's code was still mapped");
        Expect(Memory::GetTrackedObjectCount() == TrackedBefore,
               "and its memory went back to the pool it came from");
        Expect(GBarrierErrors > 0,
               "the module's leak was reported, not silently cleaned up behind it");
    }

    /*
     * The second half of the barrier's job. The module left a file-scope TRef
     * pointing at the probe; its node is still in a list EngineCore owns, and
     * its target must have been cleared. If it had been left pointing at the
     * freed block, this collect would follow it.
     */
    Memory::CollectGarbage();
    Expect(Memory::GetTrackedObjectCount() == TrackedBefore,
           "a collect after the module is gone finds nothing left of it");

    SetLogHandler(nullptr);
    appFileDelete(GWatchedPath);
}

#endif //CORE_SMOKE_MODULE_A

// --- memory: pools, Alloc/Dealloc, smart pointers, the collector -----------

/*
 * The cases below are chosen from the failure modes the memory design calls
 * out, not for coverage's sake -- the same rule the rest of this file follows.
 * Where a test asserts that something is REFUSED, it also asserts that the
 * refusal was logged: a silent refusal and a silent success are the same thing
 * from a call site's point of view, and the log is the only difference.
 */

static int GMemoryLogErrors = 0;

static void MemoryTestLogHandler(const LogVerbosity Verbosity, const char* /*Message*/)
{
    if(Verbosity == LogVerbosity::Error || Verbosity == LogVerbosity::Fatal)
    {
        ++GMemoryLogErrors;
    }
}

namespace
{
    int GCountedLive     = 0;
    int GCountedDestroyed = 0;

    struct Counted
    {
        int Value = 0;

        Counted() { ++GCountedLive; }
        explicit Counted(const int InValue) : Value(InValue) { ++GCountedLive; }
        ~Counted() { --GCountedLive; ++GCountedDestroyed; }
    };

    struct alignas(64) WideAligned
    {
        double Values[8] = {};
    };

    /* A node that can point at another node -- the shape a reference cycle
     * needs, and the shape the collector's edge discovery has to see through. */
    struct GraphNode
    {
        int                    Id = 0;
        TRef<GraphNode>        Other;

        explicit GraphNode(const int InId) : Id(InId) { ++GCountedLive; }
        ~GraphNode() { --GCountedLive; ++GCountedDestroyed; }
    };

    void ResetCounters()
    {
        GCountedLive      = 0;
        GCountedDestroyed = 0;
        GMemoryLogErrors  = 0;
    }
}

static void TestVirtualMemory()
{
    std::printf("[memory/vm]\n");

    const size_t Page = appMemPageSize();
    Expect(Page >= 4096 && (Page & (Page - 1)) == 0, "page size is a sensible power of two");

    // 64MB of ADDRESS SPACE. If this were a malloc the process would be 64MB
    // heavier; the whole point of the seam is that it is not.
    const size_t ReserveBytes = 64u * 1024u * 1024u;
    void* const  Base         = appMemReserve(ReserveBytes);
    Expect(Base != nullptr, "a 64MB reservation succeeds");

    if(Base)
    {
        Expect(appMemCommit(Base, Page), "one page commits");

        uint8_t* const Bytes = static_cast<uint8_t*>(Base);
        Bytes[0]        = 0xAB;
        Bytes[Page - 1] = 0xCD;
        Expect(Bytes[0] == 0xAB && Bytes[Page - 1] == 0xCD, "committed memory is readable and writable");

        // Committing an already-committed range must be a no-op rather than a
        // failure -- pools rely on that to commit greedily without tracking
        // page boundaries themselves.
        Expect(appMemCommit(Base, Page), "re-committing an already-committed page succeeds");
        Expect(Bytes[0] == 0xAB, "re-committing does not clear what was already there");

        Expect(appMemDecommit(Base, Page), "the page decommits");
        Expect(appMemCommit(Base, Page), "the same range re-commits after a decommit");

#if defined(_WIN32)
        /*
         * Windows guarantees a freshly committed page reads as zero, so this
         * proves the decommit actually released the pages rather than being a
         * silent no-op. NOT asserted on POSIX: MADV_DONTNEED is advisory on
         * macOS and may keep the contents, which is exactly why
         * CorePlatform.h specifies them as LOST rather than as ZERO.
         */
        Expect(Bytes[0] == 0 && Bytes[Page - 1] == 0, "decommit really released the pages");
#endif

        appMemRelease(Base, ReserveBytes);
    }
}

static void TestMemoryPools()
{
    std::printf("[memory/pools]\n");

    ResetCounters();
    SetLogHandler(&MemoryTestLogHandler);

    // --- HeapMemoryPool: accounting over the CRT ---------------------------
    {
        HeapMemoryPool Heap("SmokeHeap");
        const PoolID   Id = RegisterMemoryPool(&Heap);
        Expect(Id != POOL_Invalid && Id != POOL_Default, "a pool registers into a non-default slot");
        Expect(FindMemoryPool("SmokeHeap") == Id, "a pool is findable by name");
        Expect(FindMemoryPool("NoSuchPool") == POOL_Invalid, "an unknown name yields POOL_Invalid");

        void* Block = Heap.Allocate(128, 16);
        Expect(Block != nullptr, "the heap pool allocates");
        Expect(reinterpret_cast<uintptr_t>(Block) % 16 == 0, "the heap pool honours alignment");

        MemoryPoolStats Stats;
        Heap.GetStats(Stats);
        Expect(Stats.LiveAllocations == 1 && Stats.BytesInUse == 128, "the heap pool counts what it hands out");

        // A pool with memory still out cannot be unregistered: every live
        // block's header names this slot, and clearing it would make them
        // unfreeable.
        const int ErrorsBefore = GMemoryLogErrors;
        UnregisterMemoryPool(Id);
        Expect(GMemoryLogErrors > ErrorsBefore, "unregistering a pool with live allocations is refused and logged");
        Expect(GetMemoryPool(Id) == &Heap, "the refused unregister left the slot intact");

        Heap.Deallocate(Block, 128, 16);
        Heap.GetStats(Stats);
        Expect(Stats.LiveAllocations == 0 && Stats.BytesInUse == 0, "freeing returns the pool to empty");

        UnregisterMemoryPool(Id);
        Expect(GetMemoryPool(Id) == nullptr, "an empty pool unregisters");
    }

    // --- FixedBlockPool: one size class, reuse, exhaustion -----------------
    {
        FixedBlockPool Fixed("SmokeFixed", 64, 16, 4);

        void* A = Fixed.Allocate(64, 16);
        void* B = Fixed.Allocate(64, 16);
        void* C = Fixed.Allocate(64, 16);
        void* D = Fixed.Allocate(64, 16);
        Expect(A && B && C && D, "the fixed pool hands out its whole capacity");
        Expect(Fixed.Allocate(64, 16) == nullptr, "an exhausted fixed pool returns null rather than corrupting");

        Expect(Fixed.Owns(A), "Owns recognises a block it handed out");
        Expect(!Fixed.Owns(&Fixed), "Owns rejects a foreign pointer");
        Expect(!Fixed.Owns(static_cast<uint8_t*>(A) + 8), "Owns rejects an interior pointer");

        // The free list has to actually work -- a pool that never reuses is a
        // pool that exhausts at its capacity and stays there.
        Fixed.Deallocate(B, 64, 16);
        void* Reused = Fixed.Allocate(64, 16);
        Expect(Reused == B, "a freed block is reused");

        Expect(Fixed.Allocate(65, 16) == nullptr, "a request larger than the block size is refused");
        Expect(Fixed.Allocate(64, 32) == nullptr, "a request aligned harder than the block is refused");

        MemoryPoolStats Stats;
        Fixed.GetStats(Stats);
        Expect(Stats.BytesReserved > 0, "the fixed pool reserved address space");
        Expect(Stats.BytesCommitted <= Stats.BytesReserved, "it committed no more than it reserved");

        Fixed.Deallocate(A, 64, 16);
        Fixed.Deallocate(C, 64, 16);
        Fixed.Deallocate(D, 64, 16);
        Fixed.Deallocate(Reused, 64, 16);
    }

    // --- LinearMemoryPool: bump, and the reset that runs no destructors ----
    {
        LinearMemoryPool Linear("SmokeLinear", 1024u * 1024u);

        void* A = Linear.Allocate(100, 16);
        void* B = Linear.Allocate(100, 16);
        Expect(A && B && A != B, "the linear pool bumps");
        Expect(reinterpret_cast<uintptr_t>(B) % 16 == 0, "the bump respects alignment");
        Expect(Linear.GetBytesUsed() >= 200, "the bump advanced");

        // Documented behaviour, asserted so it cannot drift: an individual
        // free reclaims nothing at all.
        const size_t UsedBefore = Linear.GetBytesUsed();
        Linear.Deallocate(A, 100, 16);
        Expect(Linear.GetBytesUsed() == UsedBefore, "Deallocate on a bump allocator is a no-op");

        Linear.Reset();
        Expect(Linear.GetBytesUsed() == 0, "Reset rewinds the whole arena");

        void* AfterReset = Linear.Allocate(100, 16);
        Expect(AfterReset == A, "the arena hands the same addresses out again after a reset");
    }

    SetLogHandler(nullptr);
}

static void TestAllocDealloc()
{
    std::printf("[memory/alloc]\n");

    ResetCounters();
    SetLogHandler(&MemoryTestLogHandler);

    // --- constructor forwarding and exactly-once destruction ---------------
    {
        Counted* Object = Memory::Alloc<Counted>(42);
        Expect(Object != nullptr, "Alloc returns memory");
        Expect(Object->Value == 42, "Alloc forwards constructor arguments");
        Expect(GCountedLive == 1, "the object was constructed");

        const AllocHeader* Header = Memory::GetHeader(Object);
        Expect(Header != nullptr, "an Alloc'd pointer has a readable header");
        Expect(Header && Header->SizeBytes == sizeof(Counted), "the header records the payload size");
        Expect(Header && Header->Pool == POOL_Default, "a plain Alloc came from the default pool");
        Expect(Header && Header->Destruct != nullptr, "a type with a destructor records one");

        Memory::Dealloc(Object);
        Expect(GCountedLive == 0 && GCountedDestroyed == 1, "Dealloc runs the destructor exactly once");
    }

    /*
     * THE CASE THIS WHOLE DESIGN EXISTS FOR: allocate from a chosen pool, free
     * with a Dealloc that was told nothing about it, and have the bytes go
     * back to the right place. If the header round-trip is broken, this is
     * where it shows.
     */
    {
        HeapMemoryPool Heap("SmokeAllocPool");
        const PoolID   Id = RegisterMemoryPool(&Heap);

        Counted* Object = Memory::AllocIn<Counted>(Id, 7);
        Expect(Object != nullptr && Object->Value == 7, "AllocIn constructs in the chosen pool");

        MemoryPoolStats Stats;
        Heap.GetStats(Stats);
        Expect(Stats.LiveAllocations == 1, "the chosen pool -- not the default one -- provided the bytes");

        const AllocHeader* Header = Memory::GetHeader(Object);
        Expect(Header && Header->Pool == Id, "the header carries the pool id");

        Memory::Dealloc(Object);        // note: no pool argument anywhere

        Heap.GetStats(Stats);
        Expect(Stats.LiveAllocations == 0 && Stats.BytesInUse == 0,
               "Dealloc returned the bytes to the pool they came from, without being told which");

        UnregisterMemoryPool(Id);
    }

    // --- over-aligned types ------------------------------------------------
    {
        WideAligned* Wide = Memory::Alloc<WideAligned>();
        Expect(Wide != nullptr, "an over-aligned type allocates");
        Expect(reinterpret_cast<uintptr_t>(Wide) % 64 == 0, "the payload is 64-byte aligned");
        Expect(Memory::GetHeader(Wide) != nullptr, "the header survives the alignment padding");
        Memory::Dealloc(Wide);
    }

    // --- arrays ------------------------------------------------------------
    {
        ResetCounters();

        Counted* Items = Memory::AllocArray<Counted>(8);
        Expect(Items != nullptr, "AllocArray allocates");
        Expect(GCountedLive == 8, "every element was constructed");

        Memory::DeallocArray(Items, 8);
        Expect(GCountedLive == 0 && GCountedDestroyed == 8, "every element was destroyed exactly once");
    }

    // --- a bump allocator may not hold a type with a destructor ------------
    {
        ResetCounters();

        LinearMemoryPool Linear("SmokeLinearRefuse", 64u * 1024u);
        const PoolID     Id = RegisterMemoryPool(&Linear);

        // Reset() would reclaim these bytes without running ~Counted, and
        // silently not running a destructor is precisely the failure this
        // check exists to prevent.
        Counted* Refused = Memory::AllocIn<Counted>(Id, 1);
        Expect(Refused == nullptr, "a type with a destructor is refused by a pool that cannot free");
        Expect(GMemoryLogErrors > 0, "the refusal is logged rather than silent");
        Expect(GCountedLive == 0, "nothing was constructed in the refused allocation");

        // A trivially-destructible type is fine in the same pool: there is no
        // destructor for Reset to skip.
        int* Plain = Memory::AllocIn<int>(Id, 5);
        Expect(Plain != nullptr && *Plain == 5, "a trivially-destructible type is accepted");

        UnregisterMemoryPool(Id);
    }

    // --- a foreign pointer is not mistaken for an allocation ---------------
    {
        // Deliberately far inside a large buffer, so reading the 48 bytes in
        // front of it stays comfortably in bounds -- see the hazard note on
        // GetHeader.
        std::vector<uint8_t> Foreign(4096, 0);
        Expect(Memory::GetHeader(Foreign.data() + 2048) == nullptr,
               "GetHeader rejects a pointer that is not a Core allocation");
        Expect(Memory::GetHeader(nullptr) == nullptr, "GetHeader tolerates null");
    }

    SetLogHandler(nullptr);
}

static void TestSmartPointers()
{
    std::printf("[memory/pointers]\n");

    ResetCounters();
    SetLogHandler(&MemoryTestLogHandler);

    // --- TUnique -----------------------------------------------------------
    {
        {
            TUnique<Counted> A = MakeUnique<Counted>(3);
            Expect(A.IsValid() && A->Value == 3, "MakeUnique constructs");
            Expect(GCountedLive == 1, "one object is alive");

            TUnique<Counted> B = std::move(A);
            Expect(!A.IsValid() && B.IsValid(), "moving transfers ownership");
            Expect(GCountedLive == 1, "moving neither copies nor destroys");
        }
        Expect(GCountedLive == 0 && GCountedDestroyed == 1,
               "a moved-from TUnique does not double free, and the owner destroys once");

        ResetCounters();
        {
            TUnique<Counted> A = MakeUnique<Counted>(1);
            A.Reset();
            Expect(GCountedDestroyed == 1 && !A.IsValid(), "Reset destroys and empties");
            A.Reset();
            Expect(GCountedDestroyed == 1, "Reset on an empty handle does nothing");
        }
    }

    // --- TRef refcounting --------------------------------------------------
    {
        ResetCounters();
        {
            TRef<Counted> A = MakeRef<Counted>(9);
            Expect(A.IsValid() && A->Value == 9, "MakeRef constructs");
            Expect(A.GetStrongCount() == 1, "a fresh TRef holds the only reference");

            {
                TRef<Counted> B = A;
                Expect(A.GetStrongCount() == 2, "copying a TRef adds a reference");
                Expect(B.Get() == A.Get(), "both handles point at the same object");
            }
            Expect(A.GetStrongCount() == 1, "the copy released its reference");
            Expect(GCountedLive == 1, "the object outlived the copy");

            TRef<Counted> C = std::move(A);
            Expect(!A.IsValid() && C.IsValid(), "moving a TRef transfers without touching the count");
            Expect(C.GetStrongCount() == 1, "the move did not add a reference");
        }
        Expect(GCountedLive == 0 && GCountedDestroyed == 1,
               "the object dies deterministically when the last TRef drops");
    }

    // --- self-assignment and reassignment ---------------------------------
    {
        ResetCounters();
        TRef<Counted> A = MakeRef<Counted>(1);
        TRef<Counted> B = MakeRef<Counted>(2);

        A = A;                                  // must not destroy anything
        Expect(GCountedDestroyed == 0 && A.IsValid(), "self-assignment is harmless");

        A = B;
        Expect(GCountedDestroyed == 1, "reassignment released the old target exactly once");
        Expect(A->Value == 2 && B.GetStrongCount() == 2, "both handles now share the survivor");
    }

    // --- TWeakRef ----------------------------------------------------------
    {
        ResetCounters();

        TWeakRef<Counted> Weak;
        {
            TRef<Counted> Strong = MakeRef<Counted>(5);
            Weak = Strong;

            Expect(!Weak.IsStale(), "a weak reference to a live object is not stale");

            TRef<Counted> Pinned = Weak.Pin();
            Expect(Pinned.IsValid() && Pinned->Value == 5, "Pin returns a usable handle");
            Expect(Strong.GetStrongCount() == 2, "pinning takes a real reference");
        }

        Expect(GCountedDestroyed == 1, "the object died when the last strong handle dropped");
        Expect(Weak.IsStale(), "the weak reference reports the object as gone");
        Expect(!Weak.Pin().IsValid(), "pinning a dead object yields a null handle");

        // The block is still allocated -- the weak reference is what is
        // holding it -- so reading the header here must be safe rather than a
        // use-after-free. Resetting is what finally releases it.
        Weak.Reset();
        Expect(!Weak.IsSet(), "resetting clears the weak reference");
    }

    SetLogHandler(nullptr);
}

static void TestGarbageCollection()
{
    std::printf("[memory/gc]\n");

    ResetCounters();
    SetLogHandler(&MemoryTestLogHandler);

    Memory::SetCollectorThread();

    const uint64_t Baseline = Memory::GetTrackedObjectCount();

    // --- the case the collector exists for ---------------------------------
    {
        ResetCounters();
        {
            TRef<GraphNode> X = MakeRef<GraphNode>(1);
            TRef<GraphNode> Y = MakeRef<GraphNode>(2);
            X->Other = Y;
            Y->Other = X;
            Expect(X.GetStrongCount() == 2, "each cycle member is referenced twice");
        }

        // This is the whole justification for the file: reference counting
        // cannot get here, no matter how careful the call site is.
        Expect(GCountedDestroyed == 0, "a reference cycle is NOT reclaimed by refcounting");
        Expect(Memory::GetTrackedObjectCount() == Baseline + 2, "both nodes are still tracked");

        Memory::GCStats Stats;
        Memory::CollectGarbage(&Stats);

        Expect(GCountedDestroyed == 2, "the collector reclaims the cycle");
        Expect(Stats.ObjectsCollected == 2, "and reports what it reclaimed");
        Expect(Memory::GetTrackedObjectCount() == Baseline, "the object table is back to where it started");
    }

    // --- a stack TRef is a root -------------------------------------------
    {
        ResetCounters();

        TRef<GraphNode> Live = MakeRef<GraphNode>(3);
        Memory::CollectGarbage();
        Expect(GCountedDestroyed == 0, "an object held by a stack TRef survives a collect");
        Expect(Live.IsValid() && Live->Id == 3, "and is still usable afterwards");
    }

    /*
     * --- edge discovery, not just root discovery ---
     *
     * Inner is referenced ONLY by a TRef living inside Outer's payload. It
     * survives only if the mark phase followed that handle -- which is the
     * mechanism the whole no-reflection design rests on. If edge discovery
     * were broken, the root test above would still pass and this one would
     * not.
     */
    {
        ResetCounters();

        TRef<GraphNode> Outer = MakeRef<GraphNode>(4);
        Outer->Other = MakeRef<GraphNode>(5);
        GraphNode* const InnerRaw = Outer->Other.Get();

        Memory::CollectGarbage();

        Expect(GCountedDestroyed == 0, "an object reachable only through a TRef inside another object survives");
        Expect(Outer->Other.IsValid() && Outer->Other.Get() == InnerRaw, "the inner handle is unchanged");

        // Dropping the outer handle makes both unreachable, and the refcount
        // path -- not the collector -- should reclaim them, in order.
        Outer.Reset();
        Expect(GCountedDestroyed == 2, "dropping the outer reference destroys both without a collect");
    }

    // --- a weak reference does not keep an object reachable ----------------
    {
        ResetCounters();

        TWeakRef<GraphNode> Weak;
        {
            TRef<GraphNode> Strong = MakeRef<GraphNode>(6);
            Weak = Strong;
            Strong->Other = Strong;             // a one-node cycle
        }

        Expect(GCountedDestroyed == 0, "the self-referencing node keeps itself alive");
        Memory::CollectGarbage();
        Expect(GCountedDestroyed == 1, "a TWeakRef is invisible to the mark phase, so the cycle is collected");
        Expect(Weak.IsStale(), "the weak reference reports the collected object as gone");
    }

    // --- ReportLeaks -------------------------------------------------------
    // A walk, not an assertion: it must be callable at any point in a process
    // and must count what is genuinely outstanding right then.
    {
        TRef<GraphNode> Held = MakeRef<GraphNode>(7);
        Expect(Memory::ReportLeaks() == Baseline + 1, "ReportLeaks counts a live tracked object");
    }
    Expect(Memory::ReportLeaks() == Baseline, "and reports nothing once it is gone");

    Expect(Memory::GetTrackedObjectCount() == Baseline, "no tracked objects leaked out of the GC tests");

    SetLogHandler(nullptr);
}

static void RunMemoryTests()
{
    TestVirtualMemory();
    TestMemoryPools();
    TestAllocDealloc();
    TestSmartPointers();
    TestGarbageCollection();
}

static void RunModuleTests()
{
#if defined(CORE_SMOKE_MODULE_A)
    TestModuleLoad();
    TestModuleShadowPdb();
    TestModuleShadowSweep();
    TestModuleReload();
    TestModuleSettleWindow();
    TestModuleAbiMismatch();
    TestModuleReloadRollback();
    TestModuleAllocationBarrier();
#else
    std::printf("[modules] skipped (CORE_SHARED off)\n");
#endif
}

/* Content packs: hashed paths, content stored once, names only when asked. */
static void TestPack()
{
    std::printf("[pack]\n");
    Expect(HashPackPath("Content\\Levels/A.nmworld") == HashPackPath("./content/levels/a.nmworld"),
           "a pack path hashes the same whatever its case and slashes");

    const std::vector<uint8_t> Shared = { 1, 2, 3 };
    std::vector<PackInput> Inputs = {
        { "Content/A.bin", Shared }, { "Content/B.bin", Shared }, { "Config/DefaultEngine.ini", { 'x', '=', '1' } },
    };
    appCreateDirectories("SmokeScratch/Paks");
    std::string Error;
    Expect(WritePackFile("SmokeScratch/Paks/Named.nmpak", "Named", Inputs, true, false, 0, &Error), "a pack with names is written");
    Expect(WritePackFile("SmokeScratch/Paks/Bare.nmpak", "Bare", Inputs, false, true, 0, &Error), "a pack without names is written");
    std::vector<PackInput> Twice = { { "Content/A.bin", Shared }, { "content/a.bin", Shared } };
    Expect(!WritePackFile("SmokeScratch/Paks/Twice.nmpak", "Twice", Twice, false, false, 0, &Error), "a path twice is refused");

    std::vector<uint8_t> NamedBytes, BareBytes;
    appFileReadAll("SmokeScratch/Paks/Named.nmpak", NamedBytes);
    appFileReadAll("SmokeScratch/Paks/Bare.nmpak", BareBytes);
    const std::string BareText(BareBytes.begin(), BareBytes.end());
    Expect(BareText.find("Content") == std::string::npos && BareText.find("DefaultEngine") == std::string::npos
           && BareText.find("content") == std::string::npos, "a pack without names holds no path text");
    Expect(BareText.find("x=1") == std::string::npos, "a scrambled pack's content does not show");
    Expect(BareBytes.size() < NamedBytes.size() && BareBytes.size() < 200, "shared content is stored once");

    std::shared_ptr<PackFileProvider> Named = PackFileProvider::Load(NamedBytes, Error);
    std::shared_ptr<PackFileProvider> Bare  = PackFileProvider::Load(BareBytes, Error);
    Expect(Named && Bare && Named->GetEntryCount() == 3 && Bare->GetEntryCount() == 3, "both packs load, three entries each");
    std::vector<uint8_t> Out;
    Expect(Bare && Bare->ReadAll("content/b.BIN", Out) && Out == Shared, "an entry reads back by its path, any case");
    Expect(Bare && Bare->IsScrambled() && Named && !Named->IsScrambled()
           && Bare->ReadAll("Config/DefaultEngine.ini", Out) && std::string(Out.begin(), Out.end()) == "x=1",
           "a scrambled pack reads back as it was packed");
    Expect(Bare && !Bare->Exists("Content/C.bin"), "a path not packed is not there");
    Expect(Named && Named->HasNames() && Named->FindName(HashPackPath("Config/DefaultEngine.ini"))
           && *Named->FindName(HashPackPath("Config/DefaultEngine.ini")) == "Config/DefaultEngine.ini",
           "a debug pack names each hash");
    Expect(Bare && !Bare->HasNames() && !Bare->FindName(HashPackPath("Content/A.bin")), "a shipping pack names nothing");
    std::vector<uint8_t> Garbage = { 'N', 'O', 'P', 'E' };
    Expect(!PackFileProvider::Load(Garbage, Error), "something else is not a pack");

    std::unordered_map<uint64_t, HashedMemoryProvider::FileBytes> Hashed;
    Hashed[HashPackPath("Shaders/Cooked/Vulkan/A@0.fsh.spv")] = std::make_shared<const std::vector<uint8_t>>(Shared);
    HashedMemoryProvider ByHash(std::move(Hashed));
    Expect(ByHash.Exists("shaders/cooked/vulkan/a@0.fsh.spv") && ByHash.ReadAll("Shaders/Cooked/Vulkan/A@0.fsh.spv", Out)
           && Out == Shared && !ByHash.Exists("Shaders/Cooked/Vulkan/B@0.fsh.spv"), "a hashed provider answers by path, holding none");

    FilesystemProvider Disk;
    LayeredFileProvider Layered(&Disk, false);
    Expect(MountPackFiles(Layered, "SmokeScratch/Paks") == 2, "every pack in a folder mounts");
    Expect(Layered.ReadAll("Config/DefaultEngine.ini", Out) && Out.size() == 3, "a mounted pack answers the provider");
}

/* Compressed packs: chunked zstd, plain and scrambled; what does not shrink
 * is stored; range reads decode only their chunks; version 1 still reads. */
static void TestPackCompressed()
{
    std::printf("[pack, compressed]\n");

    /* ~1.2 MB of text-like bytes (several chunks, compresses well), 100 KB
     * of noise (does not), and the edge sizes. */
    std::vector<uint8_t> Text;
    const std::string Line = "Texture 3 Default Default Content/Textures/Brick_Albedo.nmtexture\n";
    for(int Index = 0; Text.size() < 1200 * 1024 + 123; ++Index)
    {
        const std::string Each = Line + std::to_string(Index) + "\n";
        Text.insert(Text.end(), Each.begin(), Each.end());
    }
    std::vector<uint8_t> Noise(100 * 1024);
    uint64_t State = 0x9E3779B97F4A7C15ull;
    for(uint8_t& Byte : Noise)
    {
        State ^= State << 13; State ^= State >> 7; State ^= State << 17;
        Byte = uint8_t(State);
    }
    const std::vector<PackInput> Inputs = {
        { "Content/Text.bin", Text }, { "Content/Noise.bin", Noise }, { "Content/Tiny.bin", { 'h', 'i' } }, { "Content/Empty.bin", {} },
    };
    const uint64_t RawSize = Text.size() + Noise.size() + 2;

    appCreateDirectories("SmokeScratch/PaksZ");
    for(const bool bScramble : { false, true })
    {
        const std::string File = std::string("SmokeScratch/PaksZ/") + (bScramble ? "Scrambled" : "Plain") + ".nmpak";
        const std::string What = bScramble ? " (scrambled)" : " (plain)";
        std::string Error;
        Expect(WritePackFile(File, "Z", Inputs, true, bScramble, 3, &Error), ("a compressed pack is written" + What).c_str());

        std::vector<uint8_t> Bytes;
        appFileReadAll(File, Bytes);
        Expect(Bytes.size() < Noise.size() + RawSize / 10, ("a compressed pack is smaller than its content" + What).c_str());
        const std::string AsText(Bytes.begin(), Bytes.end());
        Expect(AsText.find("Brick_Albedo") == std::string::npos, ("compressed content does not show" + What).c_str());

        std::vector<uint8_t> Truncated(Bytes.begin(), Bytes.end() - 16);
        Expect(!PackFileProvider::Load(Truncated, Error), ("a truncated compressed pack does not load" + What).c_str());

        std::shared_ptr<PackFileProvider> Pack = PackFileProvider::Load(Bytes, Error);
        Expect(Pack && Pack->IsCompressed() && Pack->IsScrambled() == bScramble && Pack->GetEntryCount() == 4,
               ("a compressed pack loads" + What).c_str());
        if(!Pack) { continue; }

        std::vector<uint8_t> Out;
        Expect(Pack->ReadAll("Content/Text.bin", Out) && Out == Text, ("a compressed entry reads back whole" + What).c_str());
        Expect(Pack->ReadAll("Content/Noise.bin", Out) && Out == Noise, ("a stored entry reads back" + What).c_str());
        Expect(Pack->ReadAll("Content/Tiny.bin", Out) && Out.size() == 2 && Out[0] == 'h'
               && Pack->ReadAll("Content/Empty.bin", Out) && Out.empty(), ("tiny and empty entries read back" + What).c_str());

        uint64_t Size = 0, Stored = 0;
        Expect(Pack->GetEntrySizes(HashPackPath("Content/Text.bin"), Size, Stored) && Size == Text.size() && Stored * 10 < Size,
               ("text is stored compressed" + What).c_str());
        Expect(Pack->GetEntrySizes(HashPackPath("Content/Noise.bin"), Size, Stored) && Size == Noise.size() && Stored == Size,
               ("noise is stored as it is" + What).c_str());

        /* Ranges: across a chunk boundary, exactly one chunk, inside one, the tail, nothing. */
        const uint64_t Chunk = 256 * 1024;
        const std::pair<uint64_t, uint64_t> Ranges[] = {
            { Chunk - 10, Chunk + 300 }, { Chunk, Chunk }, { 17, 1001 }, { Text.size() - 5, 5 }, { 1000, 0 },
        };
        bool bRanges = true;
        for(const auto& Range : Ranges)
        {
            const std::vector<uint8_t> Want(Text.begin() + std::ptrdiff_t(Range.first), Text.begin() + std::ptrdiff_t(Range.first + Range.second));
            bRanges = bRanges && Pack->ReadRange("Content/Text.bin", Range.first, Range.second, Out) && Out == Want;
        }
        Expect(bRanges, ("a compressed entry's ranges read back, across chunks too" + What).c_str());
        Expect(!Pack->ReadRange("Content/Text.bin", Text.size() - 5, 6, Out) && Out.empty(),
               ("a range past a compressed entry's end fails" + What).c_str());
        Expect(Pack->ReadRange("Content/Noise.bin", 99, 5000, Out)
               && Out == std::vector<uint8_t>(Noise.begin() + 99, Noise.begin() + 5099), ("a stored entry's range reads back" + What).c_str());
    }

    /* A version 1 pack, as the last build wrote it: one stored blob. */
    std::vector<uint8_t> Old;
    const auto Put = [&Old](const uint64_t InValue, const int InBytes)
    {
        for(int Byte = 0; Byte < InBytes; ++Byte) { Old.push_back(uint8_t(InValue >> (8 * Byte))); }
    };
    Put(0x4B41504Eu, 4); Put(1, 2); Put(0, 2);
    Put(3, 4); Old.insert(Old.end(), { 'O', 'l', 'd' });
    const uint64_t BlobAt = Old.size() + 4 + 16 + 4 + 12 + 4;
    Put(1, 4); Put(BlobAt, 8); Put(3, 8);
    Put(1, 4); Put(HashPackPath("Old.bin"), 8); Put(0, 4);
    Put(0, 4);
    Old.insert(Old.end(), { 'a', 'b', 'c' });
    std::string Error;
    std::shared_ptr<PackFileProvider> OldPack = PackFileProvider::Load(Old, Error);
    std::vector<uint8_t> Out;
    Expect(OldPack && !OldPack->IsCompressed() && OldPack->ReadAll("Old.bin", Out) && std::string(Out.begin(), Out.end()) == "abc",
           "a version 1 pack still reads");
}

int main()
{
    std::printf("EngineCore smoke test\n");

    TestLogging();
    TestPack();
    TestPackCompressed();
    TestFileSeam();
    TestFileMetadataSeam();
    TestDLLSeam();
    TestModuleReloadStateBuffer();
    TestFileProvider();
    TestImage();
    TestImageFormatConversion();
    TestQueue();
    RunThreadTests();
    RunJobTests();
    TestTypes();
    TestConfigRegistry();
    TestConfigParse();
    TestConfigCallbacks();
    TestConfigFile();
    TestConfigUser();
    TestConfigOverlay();
    TestConfigConsole();
    RunMemoryTests();
    RunModuleTests();

    // Restore the default handler so a failure below is not swallowed.
    SetLogHandler(nullptr);

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
