#ifndef ENGINECORE_COREMEMORYINTERNAL_H
#define ENGINECORE_COREMEMORYINTERNAL_H

#include "Public/Core/CoreGarbageCollector.h"      // GCHandleNode; pulls in CoreMemory.h

/*
 * The contract between CoreMemory.cpp and CoreGC.cpp. NOT a public header --
 * it lives under Private/ and nothing outside EngineCore includes it.
 *
 * It exists because the object table is genuinely shared: the ALLOCATOR is
 * what links a tracked object in and unlinks it on free, but the COLLECTOR is
 * what the table is for and what walks it. Putting the table in CoreMemory.cpp
 * would drag the mark and sweep in with it; putting the link calls in
 * CoreGC.h would put an implementation detail on the public surface that no
 * consumer may call and every consumer would then see.
 */

namespace EngineCore
{
namespace Memory
{
namespace Internal
{
    /*
     * Links/unlinks a header in the collector's object table, which lives in
     * CoreGC.cpp. Called only from Memory::Allocate and Memory::Free, and only
     * for AF_GCTracked blocks -- this is the one direction the allocator
     * reaches into the collector.
     */
    void TrackObject  (AllocHeader* InHeader);
    void UntrackObject(AllocHeader* InHeader);

    /*
     * Per-image live allocation counts, kept by the allocator and read by the
     * image barrier.
     *
     * These exist because the object table only holds GC-TRACKED objects. A
     * module's plain Alloc<T> or TUnique is not enumerable by anything -- and
     * making it enumerable would cost a locked list insert on every allocation
     * in the engine, to catch a case a counter diagnoses just as well. So the
     * barrier destroys what it can find and REPORTS what it cannot.
     */
    void     NoteImageAllocation(ImageID InImage);
    void     NoteImageFree      (ImageID InImage);
    uint64_t ImageLiveAllocations(ImageID InImage);

    enum : ImageID { MAX_IMAGES = 64 };
}
}
}

#endif //ENGINECORE_COREMEMORYINTERNAL_H
