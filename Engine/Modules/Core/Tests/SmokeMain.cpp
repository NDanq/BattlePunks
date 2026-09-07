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

#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreImage.h"
#include "Public/Core/CoreLog.h"
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

int main()
{
    std::printf("EngineCore smoke test\n");

    TestLogging();
    TestFileSeam();
    TestFileMetadataSeam();
    TestDLLSeam();
    TestFileProvider();
    TestImage();
    TestImageFormatConversion();
    TestQueue();
    TestTypes();

    // Restore the default handler so a failure below is not swallowed.
    SetLogHandler(nullptr);

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
