#include "Public/Core/CoreGarbageCollector.h"

#include "Private/Core/CoreMemoryInternal.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

/*
 * The object table, the TRef handle list, and the mark/sweep over both.
 *
 * Read the long comment at the top of CoreGC.h first -- particularly the part
 * about this being a BACKSTOP for reference cycles rather than the mechanism
 * by which objects normally die.
 */

namespace EngineCore
{
namespace Memory
{
namespace
{
    /*
     * Both lists are raw pointers at file scope, CONSTANT-INITIALISED into
     * .bss, for the reason CoreConfig.h:31 sets out at length: a tracked
     * object may be allocated by a file-scope constructor in any image, before
     * this translation unit's own dynamic initialisers have run. A
     * std::vector or a Meyers singleton here would be pushed into before it
     * existed.
     */
    AllocHeader*  GObjectHead  = nullptr;
    uint64_t      GObjectCount = 0;
    GCHandleNode* GHandleHead  = nullptr;

    /* std::mutex is constexpr-default-constructible, so these are
     * constant-initialised too and do not reintroduce the ordering problem the
     * raw heads above exist to avoid. */
    std::mutex GObjectMutex;
    std::mutex GHandleMutex;

    bool     GCollecting      = false;
    bool     GCollectorThreadSet = false;
    ThreadID GCollectorThread = 0;

    uint8_t* PayloadOf(AllocHeader* InHeader)
    {
        // The header sits immediately below the payload -- see Memory::Allocate.
        return reinterpret_cast<uint8_t*>(InHeader + 1);
    }

    AllocHeader* HeaderOf(void* InPayload)
    {
        if(!InPayload)
        {
            return nullptr;
        }
        AllocHeader* Header = reinterpret_cast<AllocHeader*>(InPayload) - 1;
        return (Header->Magic == ALLOC_HEADER_MAGIC) ? Header : nullptr;
    }

    /* A tracked object's payload extent, for the address-range tests below. */
    struct ObjectRange
    {
        const uint8_t* Start  = nullptr;
        const uint8_t* End    = nullptr;
        AllocHeader*   Header = nullptr;
    };

    /* One registered TRef: where it lives, and what it points at. */
    struct HandleEntry
    {
        const uint8_t* Address = nullptr;
        void*          Target  = nullptr;
    };

    /*
     * Which tracked object, if any, contains InAddress.
     *
     * This is the whole root/edge discrimination, and it is why the ranges are
     * sorted first: the alternative is a scan of every object for every
     * handle, which is O(objects x handles) and turns a collect on a large
     * scene into a visible stall.
     */
    const ObjectRange* FindContaining(const std::vector<ObjectRange>& InRanges, const uint8_t* InAddress)
    {
        // First range whose Start is > InAddress; the candidate is the one before it.
        auto It = std::upper_bound(InRanges.begin(), InRanges.end(), InAddress,
                                   [](const uint8_t* InAddr, const ObjectRange& InRange)
                                   {
                                       return InAddr < InRange.Start;
                                   });
        if(It == InRanges.begin())
        {
            return nullptr;
        }
        --It;
        return (InAddress < It->End) ? &(*It) : nullptr;
    }
}

namespace Internal
{
    void TrackObject(AllocHeader* InHeader)
    {
        std::lock_guard<std::mutex> Lock(GObjectMutex);

        InHeader->PrevTracked = nullptr;
        InHeader->NextTracked = GObjectHead;
        if(GObjectHead)
        {
            GObjectHead->PrevTracked = InHeader;
        }
        GObjectHead = InHeader;
        ++GObjectCount;
    }

    void UntrackObject(AllocHeader* InHeader)
    {
        std::lock_guard<std::mutex> Lock(GObjectMutex);

        if(InHeader->PrevTracked)
        {
            InHeader->PrevTracked->NextTracked = InHeader->NextTracked;
        }
        else if(GObjectHead == InHeader)
        {
            GObjectHead = InHeader->NextTracked;
        }
        else
        {
            return;     // already unlinked; nothing to do and nothing to count
        }

        if(InHeader->NextTracked)
        {
            InHeader->NextTracked->PrevTracked = InHeader->PrevTracked;
        }

        InHeader->NextTracked = nullptr;
        InHeader->PrevTracked = nullptr;
        InHeader->Flags      &= ~uint8_t(AF_GCTracked);

        if(GObjectCount > 0)
        {
            --GObjectCount;
        }
    }
}

// === Handle registration ====================================================

void RegisterHandle(GCHandleNode* InNode)
{
    std::lock_guard<std::mutex> Lock(GHandleMutex);

    InNode->Prev = nullptr;
    InNode->Next = GHandleHead;
    if(GHandleHead)
    {
        GHandleHead->Prev = InNode;
    }
    GHandleHead = InNode;
}

void UnregisterHandle(GCHandleNode* InNode)
{
    std::lock_guard<std::mutex> Lock(GHandleMutex);

    if(InNode->Prev)
    {
        InNode->Prev->Next = InNode->Next;
    }
    else if(GHandleHead == InNode)
    {
        GHandleHead = InNode->Next;
    }
    else
    {
        return;     // not linked
    }

    if(InNode->Next)
    {
        InNode->Next->Prev = InNode->Prev;
    }

    InNode->Next = nullptr;
    InNode->Prev = nullptr;
}

// === Collection =============================================================

void SetCollectorThread()
{
    GCollectorThread    = appThreadID();
    GCollectorThreadSet = true;
}

uint64_t GetTrackedObjectCount()
{
    std::lock_guard<std::mutex> Lock(GObjectMutex);
    return GObjectCount;
}

void CollectGarbage(GCStats* OutStats)
{
    if(GCollecting)
    {
        CORE_LOG_ERROR("Memory: CollectGarbage re-entered -- a destructor ran during a sweep and "
                       "called it again; ignoring the inner call")
        return;
    }

    if(!GCollectorThreadSet)
    {
        SetCollectorThread();
    }
    else if(appThreadID() != GCollectorThread)
    {
        CORE_LOG_ERROR("Memory: CollectGarbage called from a thread other than the collector "
                       "thread; refusing (see the threading note in CoreGC.h)")
        return;
    }

    GCollecting = true;

    GCStats Stats;

    // --- Gather -------------------------------------------------------------
    // Snapshot both lists under their locks, then let them go. Everything after
    // this point relies on the world being stopped, which is the contract; the
    // locks are held only here because the sweep's own destructors will call
    // back into TrackObject/UntrackObject and would deadlock against them.
    std::vector<ObjectRange> Objects;
    {
        std::lock_guard<std::mutex> Lock(GObjectMutex);
        Objects.reserve(size_t(GObjectCount));

        for(AllocHeader* Header = GObjectHead; Header; Header = Header->NextTracked)
        {
            Header->Flags &= ~uint8_t(AF_Marked);

            uint8_t* const Payload = PayloadOf(Header);
            Objects.push_back(ObjectRange{ Payload, Payload + Header->SizeBytes, Header });
        }
    }

    std::vector<HandleEntry> Handles;
    {
        std::lock_guard<std::mutex> Lock(GHandleMutex);
        for(GCHandleNode* Node = GHandleHead; Node; Node = Node->Next)
        {
            if(Node->Target)
            {
                Handles.push_back(HandleEntry{ reinterpret_cast<const uint8_t*>(Node), Node->Target });
            }
        }
    }

    std::sort(Objects.begin(), Objects.end(),
              [](const ObjectRange& L, const ObjectRange& R) { return L.Start < R.Start; });
    std::sort(Handles.begin(), Handles.end(),
              [](const HandleEntry& L, const HandleEntry& R) { return L.Address < R.Address; });

    Stats.ObjectsConsidered = Objects.size();
    Stats.HandlesScanned    = Handles.size();

    // --- Mark ---------------------------------------------------------------
    // An explicit worklist rather than recursion: a long chain of objects is a
    // long chain of stack frames, and a scene graph is exactly that shape.
    std::vector<void*> Worklist;

    for(const HandleEntry& Entry : Handles)
    {
        // A handle INSIDE a tracked object is that object's edge, and is only
        // followed if the object turns out to be reachable. A handle anywhere
        // else -- stack, global, plain struct member, std::vector buffer -- is
        // a root.
        if(FindContaining(Objects, Entry.Address) == nullptr)
        {
            Worklist.push_back(Entry.Target);
            ++Stats.RootsFound;
        }
    }

    while(!Worklist.empty())
    {
        void* const Target = Worklist.back();
        Worklist.pop_back();

        AllocHeader* Header = HeaderOf(Target);
        if(!Header || (Header->Flags & AF_Marked))
        {
            continue;
        }

        Header->Flags |= AF_Marked;
        ++Stats.ObjectsMarked;

        // Follow this object's outgoing edges: every registered handle whose
        // address falls inside its payload.
        const uint8_t* const Start = static_cast<const uint8_t*>(Target);
        const uint8_t* const End   = Start + Header->SizeBytes;

        auto It = std::lower_bound(Handles.begin(), Handles.end(), Start,
                                   [](const HandleEntry& InEntry, const uint8_t* InAddr)
                                   {
                                       return InEntry.Address < InAddr;
                                   });
        for(; It != Handles.end() && It->Address < End; ++It)
        {
            Worklist.push_back(It->Target);
        }
    }

    // --- Sweep --------------------------------------------------------------
    std::vector<AllocHeader*> Doomed;
    for(const ObjectRange& Range : Objects)
    {
        const uint8_t Flags = Range.Header->Flags;
        if((Flags & AF_Marked) == 0 && (Flags & AF_Pinned) == 0)
        {
            Doomed.push_back(Range.Header);
        }
    }

    /*
     * THREE PHASES, AND THE ORDER IS THE WHOLE POINT.
     *
     * Destroying a cycle member drops its TRefs to the other members, whose
     * counts reach zero, whose ReleaseStrongRef would free blocks this loop is
     * still holding pointers to. Flagging the entire set BEFORE any destructor
     * runs makes that path decrement and stop; the release below is then the
     * only one, and it happens after every destructor is done.
     */
    for(AllocHeader* Header : Doomed)
    {
        Header->Flags |= AF_Sweeping;
    }

    for(AllocHeader* Header : Doomed)
    {
        DestroyObject(PayloadOf(Header));
    }

    for(AllocHeader* Header : Doomed)
    {
        Stats.BytesCollected += Header->SizeBytes;
        ++Stats.ObjectsCollected;

        /*
         * Force the strong count to zero. These objects are unreachable BY
         * DEFINITION -- that is what the mark phase just proved -- so a
         * nonzero count here is the cycle's own self-reference and nothing
         * else.
         */
        Header->StrongCount.store(0, std::memory_order_relaxed);

        // Out of the table before the block can go away, and while the header
        // is still guaranteed readable.
        Internal::UntrackObject(Header);

        Header->Flags &= ~uint8_t(AF_Sweeping);

        /*
         * Drop the implicit weak reference the strong count was holding, which
         * is what actually frees the block -- unless a real TWeakRef still
         * points here, in which case the payload is dead but the bytes stay
         * readable until that reference goes away. Either outcome falls out of
         * the one call; see THE IMPLICIT WEAK REFERENCE in CoreMemory.h.
         */
        ReleaseWeakRef(PayloadOf(Header));
    }

    GCollecting = false;

    if(Stats.ObjectsCollected > 0)
    {
        CORE_LOG("Memory: collected " + std::to_string(Stats.ObjectsCollected) +
                 " unreachable object(s), " + std::to_string(Stats.BytesCollected) + " bytes")
    }

    if(OutStats)
    {
        *OutStats = Stats;
    }
}

uint64_t ReportLeaks()
{
    uint64_t Total = 0;

    std::vector<AllocHeader*> Live;
    {
        std::lock_guard<std::mutex> Lock(GObjectMutex);
        for(AllocHeader* Header = GObjectHead; Header; Header = Header->NextTracked)
        {
            Live.push_back(Header);
        }
    }

    for(AllocHeader* Header : Live)
    {
        ++Total;
        CORE_LOG_WARN("Memory: leaked tracked object -- " + std::to_string(Header->SizeBytes) +
                      " bytes, strong " + std::to_string(Header->StrongCount.load(std::memory_order_relaxed)) +
                      ", weak " + std::to_string(Header->WeakCount.load(std::memory_order_relaxed)) +
                      ", from image '" + GetImageName(Header->OwningImage) + "'")
    }

    // Untracked allocations are not enumerable -- see CoreMemoryInternal.h --
    // so they are reported as a count per image rather than individually.
    for(ImageID Image = 0; Image < Internal::MAX_IMAGES; ++Image)
    {
        const uint64_t Count = Internal::ImageLiveAllocations(Image);
        if(Count > 0)
        {
            CORE_LOG_WARN(std::string("Memory: image '") + GetImageName(Image) + "' still holds " +
                          std::to_string(Count) + " live allocation(s)")
        }
    }

    return Total;
}

// === The hot-reload barrier =================================================

uint64_t OnImageUnloading(const ImageID InImage)
{
    if(InImage == IMAGE_Host)
    {
        // The host's image is never unmapped while the process runs, so there
        // is nothing to protect against and destroying everything it owns
        // would be catastrophic rather than merely wrong.
        return 0;
    }

    std::vector<AllocHeader*> Owned;
    {
        std::lock_guard<std::mutex> Lock(GObjectMutex);
        for(AllocHeader* Header = GObjectHead; Header; Header = Header->NextTracked)
        {
            /*
             * A null Destruct means a trivially-destructible type: there is no
             * code in the departing image for anything to jump into later, so
             * the object is safe to leave alive. This is the one exemption.
             */
            if(Header->OwningImage == InImage && Header->Destruct != nullptr)
            {
                Owned.push_back(Header);
            }
        }
    }

    /*
     * NEUTER EVERY HANDLE POINTING AT WHAT IS ABOUT TO BE DESTROYED, FIRST.
     *
     * This is not tidiness, it is the difference between a clean unload and a
     * crash minutes later. A TRef living in the departing module -- a file-
     * scope one, or a member of something the module allocated -- is a node in
     * a list EngineCore owns. Its destructor runs at DLL detach, INSIDE
     * appDLLClose, after the loop below has already freed what it points at,
     * and it would then release a reference through a header that no longer
     * exists.
     *
     * Nulling the target makes that destructor a no-op. It cannot save the
     * node itself: a TRef in memory the module leaks outright is never
     * destroyed, so its node stays in the list with its Next/Prev threading
     * through unmapped pages, and the next collect walks into it. Nothing in
     * this design can fix that from here -- it is the case CoreModule.h's rule
     * exists to forbid -- so it is counted and reported instead.
     */
    uint64_t HandlesNeutered = 0;
    {
        std::lock_guard<std::mutex> Lock(GHandleMutex);
        for(GCHandleNode* Node = GHandleHead; Node; Node = Node->Next)
        {
            AllocHeader* Target = HeaderOf(Node->Target);
            if(Target && Target->OwningImage == InImage && Target->Destruct != nullptr)
            {
                Node->Target = nullptr;
                ++HandlesNeutered;
            }
        }
    }

    if(HandlesNeutered > 0)
    {
        CORE_LOG_WARN(std::string("Memory: cleared ") + std::to_string(HandlesNeutered) +
                      " handle(s) pointing into image '" + GetImageName(InImage) +
                      "' so their destructors cannot reach freed memory");
    }

    uint64_t Destroyed = 0;

    for(AllocHeader* Header : Owned)
    {
        if(Header->Flags & AF_Pinned)
        {
            CORE_LOG_ERROR(std::string("Memory: pinned object from image '") + GetImageName(InImage) +
                           "' cannot be destroyed at unload; its destructor is about to be unmapped")
            continue;
        }

        const uint32_t Strong = Header->StrongCount.load(std::memory_order_relaxed);
        if(Strong > 0)
        {
            /*
             * Somebody outside the module is still holding this. It gets
             * destroyed anyway -- leaving it alive means a dangling Destruct
             * pointer and a crash in the collector with nothing to connect it
             * back here. Destroying it means a dangling TRef, which at least
             * fails at the use site.
             *
             * Either way this is a module bug, and the log is where it stops
             * being invisible. CoreModule.h:37: "Nothing a module allocates
             * outlives the module."
             */
            CORE_LOG_ERROR(std::string("Memory: object from image '") + GetImageName(InImage) +
                           "' is still referenced (" + std::to_string(Strong) +
                           " strong) at unload; destroying it anyway -- the module leaked it")
        }

        // Same three-step shape as the sweep, and for the same reason: flag
        // first so a destructor dropping a reference back into this set
        // decrements and stops, release the bytes only once it has run.
        Header->Flags |= AF_Sweeping;
        DestroyObject(PayloadOf(Header));

        Header->StrongCount.store(0, std::memory_order_relaxed);
        Internal::UntrackObject(Header);
        Header->Flags &= ~uint8_t(AF_Sweeping);

        ReleaseWeakRef(PayloadOf(Header));

        ++Destroyed;
    }

    /*
     * What the object table cannot see: plain Alloc<T> and TUnique from this
     * module. Nothing enumerates those, so all that can be done is say how
     * many there are -- which is enough to point at the right module, which is
     * the whole job of this message.
     */
    const uint64_t Remaining = Internal::ImageLiveAllocations(InImage);
    if(Remaining > 0)
    {
        CORE_LOG_ERROR(std::string("Memory: image '") + GetImageName(InImage) + "' is unloading with " +
                       std::to_string(Remaining) +
                       " untracked allocation(s) outstanding; their memory is leaked and any "
                       "destructor they still owe will be unmapped with the module")
    }

    return Destroyed;
}
}
}
