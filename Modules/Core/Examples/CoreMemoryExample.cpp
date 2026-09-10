/*
 * CoreMemoryExample -- a worked tour of the EngineCore memory subsystem.
 *
 * Build with -DCORE_BUILD_EXAMPLES=ON and run the binary. It prints what it is
 * doing at each step, so the output IS the documentation; read it side by side
 * with this file.
 *
 * It is deliberately shaped like a small engine subsystem rather than a list of
 * API calls -- a particle system with its own pool, a scene graph whose nodes
 * reference each other, and a per-frame scratch arena -- because the questions
 * this subsystem answers ("which pool?", "who owns this?", "what about
 * cycles?") only really come up once there is a shape to hang them on.
 *
 * WHAT TO TAKE AWAY, IF YOU READ NOTHING ELSE
 *
 *   1. Reach for TUnique. Most things have one owner.
 *   2. Reach for TRef when ownership is genuinely shared. It is reference
 *      counted, so objects die on time; the collector is only there for cycles.
 *   3. Dealloc never takes a pool. The allocation remembers where it came from.
 *   4. Size a FixedBlockPool with FixedBlockBytesFor<T>(), never sizeof(T).
 *
 * The headers are the reference material; each section below points at the one
 * it is demonstrating.
 */

#include "Public/Core/CoreGC.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/Core/CoreMemoryPool.h"
#include "Public/Core/CorePointer.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

using namespace EngineCore;

// ===========================================================================
// Types the example allocates
// ===========================================================================

namespace
{
    /*
     * A trivially-destructible POD. Worth noticing: because it has no
     * destructor, Alloc records a null Destruct in its header, which makes it
     * legal in a LinearMemoryPool and exempt from the module unload barrier.
     */
    struct Particle
    {
        float X = 0.0f;
        float Y = 0.0f;
        float VelocityX = 0.0f;
        float VelocityY = 0.0f;
        float LifeSeconds = 0.0f;
    };

    /*
     * A type with a real destructor, so its lifetime is observable in the
     * output. Anything with a destructor is the interesting case throughout:
     * it is what the collector has to call, and what a bump allocator is
     * refused.
     */
    struct Texture
    {
        std::string Name;
        int         Width  = 0;
        int         Height = 0;

        Texture(std::string InName, const int InWidth, const int InHeight)
            : Name(std::move(InName)), Width(InWidth), Height(InHeight)
        {
            std::printf("      + Texture '%s' (%dx%d) constructed\n", Name.c_str(), Width, Height);
        }

        ~Texture()
        {
            std::printf("      - Texture '%s' destroyed\n", Name.c_str());
        }
    };

    /*
     * A scene node holding TRefs to other nodes. This is the shape that makes
     * a collector necessary at all: Parent and Child pointing at each other is
     * a cycle, and no amount of care at the call site lets reference counting
     * reclaim one.
     */
    struct SceneNode
    {
        std::string        Name;
        TRef<SceneNode>    Child;
        TWeakRef<SceneNode> Parent;    // WEAK on purpose -- see Section 5

        explicit SceneNode(std::string InName) : Name(std::move(InName))
        {
            std::printf("      + SceneNode '%s' constructed\n", Name.c_str());
        }

        ~SceneNode()
        {
            std::printf("      - SceneNode '%s' destroyed\n", Name.c_str());
        }
    };

    void Heading(const char* InText)
    {
        const size_t Length = std::strlen(InText);
        const size_t Rule   = (Length < 70) ? (70 - Length) : 3;
        std::printf("\n=== %s %s\n", InText, std::string(Rule, '=').c_str());
    }

    void PrintPoolStats(const char* InLabel, const PoolID InPool)
    {
        MemoryPoolStats Stats;
        if(!Memory::GetPoolStats(InPool, Stats))
        {
            std::printf("      %-16s <not registered>\n", InLabel);
            return;
        }

        std::printf("      %-16s live=%-4llu  inUse=%-8llu  peak=%-8llu  committed=%llu\n",
                    InLabel,
                    (unsigned long long)Stats.LiveAllocations,
                    (unsigned long long)Stats.BytesInUse,
                    (unsigned long long)Stats.PeakBytesInUse,
                    (unsigned long long)Stats.BytesCommitted);
    }

    /* Routes engine log output to stdout so the subsystem's own diagnostics --
     * the refusals and leak reports below -- appear inline with the narration
     * rather than vanishing. A real host installs its own; see CoreLog.h. */
    void ExampleLogHandler(const LogVerbosity InVerbosity, const char* InMessage)
    {
        const char* Tag = "LOG";
        switch(InVerbosity)
        {
            case LogVerbosity::Warning: Tag = "WARN";  break;
            case LogVerbosity::Error:   Tag = "ERROR"; break;
            case LogVerbosity::Fatal:   Tag = "FATAL"; break;
            default: break;
        }
        std::printf("      [%s] %s\n", Tag, InMessage);
    }
}

// ===========================================================================
// 1. Explicit Alloc / Dealloc  (CoreMemory.h)
// ===========================================================================

static void ExplicitAllocation()
{
    Heading("1. Alloc / Dealloc");

    std::printf("  Alloc<T> forwards constructor arguments and returns a live T*.\n");

    Texture* Diffuse = Memory::Alloc<Texture>("diffuse", 1024, 1024);

    /*
     * Every allocation carries a 48-byte header in front of the payload. It is
     * what makes Dealloc work without a pool argument, and it is readable if
     * you want to know where something came from.
     */
    if(const AllocHeader* Header = Memory::GetHeader(Diffuse))
    {
        std::printf("      header: %u payload bytes, pool %u, image '%s'\n",
                    Header->SizeBytes, unsigned(Header->Pool),
                    Memory::GetImageName(Header->OwningImage));
    }

    std::printf("  Dealloc runs the destructor and returns the bytes.\n");
    Memory::Dealloc(Diffuse);

    /*
     * Arrays are a separate pair, and DeallocArray wants the count back. That
     * is deliberate -- see the note in CoreMemory.h. If you find yourself
     * tracking the count by hand at every call site, you want a container.
     */
    std::printf("  AllocArray constructs N elements; DeallocArray destroys them in reverse.\n");
    Particle* Burst = Memory::AllocArray<Particle>(256);
    Burst[0].X = 1.0f;
    Memory::DeallocArray(Burst, 256);

    PrintPoolStats("default pool", POOL_Default);
}

// ===========================================================================
// 2. Sole ownership: TUnique  (CorePointer.h)
// ===========================================================================

static void SoleOwnership()
{
    Heading("2. TUnique -- the default choice");

    std::printf("  Move-only, no refcount, no collector participation.\n");
    std::printf("  It costs nothing beyond the allocation itself.\n");

    {
        TUnique<Texture> Owned = MakeUnique<Texture>("lightmap", 512, 512);

        std::printf("  Moving transfers ownership -- neither copies nor destroys:\n");
        TUnique<Texture> Moved = std::move(Owned);

        std::printf("      original is now %s, new owner holds '%s'\n",
                    Owned.IsValid() ? "valid" : "empty", Moved->Name.c_str());

        std::printf("  Leaving scope destroys it:\n");
    }
}

// ===========================================================================
// 3. Pools, and allocating into a chosen one  (CoreMemoryPool.h)
// ===========================================================================

static void PoolsAndOverrides()
{
    Heading("3. Memory pools");

    /*
     * SIZE A FIXED-BLOCK POOL WITH THE HELPERS, NEVER sizeof(T).
     *
     * Every allocation carries the 48-byte header, so a pool sized sizeof(T)
     * rejects every single request for a T -- and reports it as an
     * out-of-memory, which looks nothing like the sizing mistake it is.
     */
    FixedBlockPool ParticlePool("Particles",
                                Memory::FixedBlockBytesFor<Particle>(),
                                Memory::FixedBlockAlignFor<Particle>(),
                                4096);

    const PoolID Particles = RegisterMemoryPool(&ParticlePool);

    std::printf("  FixedBlockPool 'Particles': %llu-byte blocks (%llu payload + 48 header), 4096 of them.\n",
                (unsigned long long)Memory::FixedBlockBytesFor<Particle>(),
                (unsigned long long)sizeof(Particle));
    std::printf("  It reserved address space up front and has committed nothing yet:\n");
    PrintPoolStats("Particles", Particles);

    /*
     * AllocIn is the pool override. Note what happens at the free site: it is
     * a plain Dealloc, with no mention of the pool anywhere. The header
     * carries it, which is the whole reason a pool override is safe to use.
     */
    std::printf("\n  AllocIn<T>(Pool, ...) allocates from it. 1000 particles:\n");

    Particle* Live[1000] = {};
    for(int Index = 0; Index < 1000; ++Index)
    {
        Live[Index] = Memory::AllocIn<Particle>(Particles);
        Live[Index]->X = float(Index);
    }
    PrintPoolStats("Particles", Particles);

    std::printf("\n  ... and a plain Dealloc puts them back. No pool argument anywhere:\n");
    for(Particle* Each : Live)
    {
        Memory::Dealloc(Each);      // <-- knows nothing about ParticlePool
    }
    PrintPoolStats("Particles", Particles);

    // Pools must be empty before they can be unregistered -- every live block's
    // header names this slot, so clearing it would strand them.
    UnregisterMemoryPool(Particles);
}

// ===========================================================================
// 4. The frame arena  (LinearMemoryPool)
// ===========================================================================

static void FrameArena()
{
    Heading("4. LinearMemoryPool -- per-frame scratch");

    LinearMemoryPool Arena("FrameScratch", 4u * 1024u * 1024u);
    const PoolID     Scratch = RegisterMemoryPool(&Arena);

    std::printf("  Allocation is a pointer bump. Deallocate does NOTHING;\n");
    std::printf("  Reset() reclaims the whole arena at the frame boundary.\n\n");

    for(int Frame = 0; Frame < 3; ++Frame)
    {
        for(int Index = 0; Index < 500; ++Index)
        {
            Memory::AllocIn<Particle>(Scratch);   // never individually freed
        }
        std::printf("      frame %d: %llu bytes used", Frame, (unsigned long long)Arena.GetBytesUsed());

        Arena.Reset();
        std::printf("  ->  Reset  ->  %llu bytes used\n", (unsigned long long)Arena.GetBytesUsed());
    }

    /*
     * The guard rail. Reset() cannot run destructors -- it has no idea what is
     * in the region -- so the pool advertises that it cannot free individual
     * blocks and Allocate refuses any type with a destructor. Texture has one,
     * so this is refused and logged rather than silently leaking a
     * std::string per particle.
     */
    std::printf("\n  A type with a destructor is refused by a bump allocator:\n");
    Texture* Refused = Memory::AllocIn<Texture>(Scratch, "scratch", 4, 4);
    std::printf("      AllocIn<Texture> returned %s\n", Refused ? "a pointer (!)" : "nullptr, as it should");

    UnregisterMemoryPool(Scratch);
}

// ===========================================================================
// 5. Shared ownership: TRef and TWeakRef  (CorePointer.h)
// ===========================================================================

static void SharedOwnership()
{
    Heading("5. TRef / TWeakRef -- shared ownership");

    std::printf("  TRef is atomically reference counted, so objects die the moment\n");
    std::printf("  the last handle drops -- deterministically, right there.\n\n");

    TWeakRef<Texture> Observer;

    {
        TRef<Texture> Shared = MakeRef<Texture>("albedo", 2048, 2048);
        std::printf("      after MakeRef:        strong = %u\n", Shared.GetStrongCount());

        {
            TRef<Texture> SecondHandle = Shared;
            std::printf("      after a copy:         strong = %u\n", Shared.GetStrongCount());
        }
        std::printf("      copy left scope:      strong = %u\n", Shared.GetStrongCount());

        /*
         * A weak reference observes without owning and without keeping the
         * object reachable. It is the right answer for a back-pointer -- see
         * SceneNode::Parent above, which is weak precisely so a parent/child
         * pair is not a cycle in the first place.
         */
        Observer = Shared;
        std::printf("      a TWeakRef does not raise the count: strong = %u\n", Shared.GetStrongCount());

        std::printf("\n  Pin() is the only safe way to use a weak reference:\n");
        if(TRef<Texture> Pinned = Observer.Pin())
        {
            std::printf("      pinned '%s', strong = %u while held\n",
                        Pinned->Name.c_str(), Pinned.GetStrongCount());
        }

        std::printf("\n  Last strong handle leaving scope destroys it:\n");
    }

    std::printf("      the weak reference now reports stale: %s\n", Observer.IsStale() ? "yes" : "no");
    std::printf("      and Pin() yields an empty handle:     %s\n",
                Observer.Pin().IsValid() ? "no (!)" : "yes");
}

// ===========================================================================
// 6. The one thing refcounting cannot do  (CoreGC.h)
// ===========================================================================

static void ReferenceCycles()
{
    Heading("6. Cycles, and the collector");

    Memory::SetCollectorThread();
    const uint64_t Baseline = Memory::GetTrackedObjectCount();

    std::printf("  A parent/child pair using WEAK back-pointers is not a cycle,\n");
    std::printf("  and needs no collector at all:\n\n");
    {
        TRef<SceneNode> Root  = MakeRef<SceneNode>("Root");
        TRef<SceneNode> Leaf  = MakeRef<SceneNode>("Leaf");
        Root->Child  = Leaf;
        Leaf->Parent = Root;      // WEAK -- no reference taken

        std::printf("      leaving scope:\n");
    }
    std::printf("      tracked objects: %llu (back to baseline)\n\n",
                (unsigned long long)Memory::GetTrackedObjectCount());

    /*
     * Now the case the collector exists for. Two nodes each holding a STRONG
     * reference to the other keep each other's count at 1 forever. This is not
     * a mistake you can code your way out of at the call site -- it is what
     * reference counting structurally cannot reclaim.
     */
    std::printf("  Two nodes holding STRONG references to each other cannot be\n");
    std::printf("  reclaimed by refcounting, no matter how careful the call site is:\n\n");
    {
        TRef<SceneNode> A = MakeRef<SceneNode>("CycleA");
        TRef<SceneNode> B = MakeRef<SceneNode>("CycleB");
        A->Child = B;
        B->Child = A;             // STRONG both ways -- a cycle
        std::printf("      leaving scope (note: no destructors run)...\n");
    }
    std::printf("      tracked objects still alive: %llu\n",
                (unsigned long long)(Memory::GetTrackedObjectCount() - Baseline));

    std::printf("\n  CollectGarbage() is the backstop. It is explicitly driven -- the host\n");
    std::printf("  calls it at a frame boundary of its choosing, with the world stopped:\n\n");

    Memory::GCStats Stats;
    Memory::CollectGarbage(&Stats);

    std::printf("      considered %llu, marked %llu, collected %llu (%llu bytes)\n",
                (unsigned long long)Stats.ObjectsConsidered,
                (unsigned long long)Stats.ObjectsMarked,
                (unsigned long long)Stats.ObjectsCollected,
                (unsigned long long)Stats.BytesCollected);

    /*
     * How it found them, with no reflection and no base class: every TRef
     * registers itself, and is classified by its own ADDRESS. A TRef on the
     * stack is a root; a TRef living inside a tracked object's payload is that
     * object's outgoing edge. The two cycle nodes had only the latter, so
     * nothing reachable pointed at them.
     *
     * The corollary matters: a raw T* is invisible to this. Hold a TRef; use
     * Get() at the point of use, not for storage.
     */
    std::printf("\n  A live object held by a stack TRef is a ROOT and survives:\n\n");
    {
        TRef<SceneNode> Held = MakeRef<SceneNode>("Survivor");
        Held->Child = MakeRef<SceneNode>("HeldByEdge");   // reachable only via Held's payload

        Memory::CollectGarbage(&Stats);
        std::printf("      collected %llu -- both survived (one as a root, one as an edge)\n",
                    (unsigned long long)Stats.ObjectsCollected);
    }
}

// ===========================================================================

int main()
{
    // Unbuffered, so the narration and the engine's own log output interleave
    // in the order they actually happened rather than in flush order.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    SetLogHandler(&ExampleLogHandler);

    std::printf("EngineCore memory subsystem -- worked example\n");

    ExplicitAllocation();
    SoleOwnership();
    PoolsAndOverrides();
    FrameArena();
    SharedOwnership();
    ReferenceCycles();

    /*
     * An EXPLICIT call, never a static destructor -- a destructor at process
     * exit runs in an order nothing specifies and would report teardown
     * ordering as leaks. A real host calls this at a point of its choosing,
     * the same way it calls ModuleManager::UnloadAll.
     */
    Heading("7. Leak report");
    std::printf("  Everything above cleaned up after itself, so this should be silent:\n");

    const uint64_t Leaked = Memory::ReportLeaks();
    std::printf("      %llu tracked object(s) outstanding\n", (unsigned long long)Leaked);

    PrintPoolStats("default pool", POOL_Default);

    std::printf("\nDone.\n");

    SetLogHandler(nullptr);
    return Leaked == 0 ? 0 : 1;
}
