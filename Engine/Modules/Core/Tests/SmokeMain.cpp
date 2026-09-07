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

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <utility>

#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreImage.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreModule.h"
#include "Public/Core/CoreModuleManager.h"
#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreQueue.h"
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

#endif //CORE_SMOKE_MODULE_A

static void RunModuleTests()
{
#if defined(CORE_SMOKE_MODULE_A)
    TestModuleLoad();
    TestModuleReload();
    TestModuleSettleWindow();
    TestModuleAbiMismatch();
    TestModuleReloadRollback();
#else
    std::printf("[modules] skipped (CORE_SHARED off)\n");
#endif
}

int main()
{
    std::printf("EngineCore smoke test\n");

    TestLogging();
    TestFileSeam();
    TestFileMetadataSeam();
    TestDLLSeam();
    TestModuleReloadStateBuffer();
    TestFileProvider();
    TestImage();
    TestImageFormatConversion();
    TestQueue();
    TestTypes();
    RunModuleTests();

    // Restore the default handler so a failure below is not swallowed.
    SetLogHandler(nullptr);

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
