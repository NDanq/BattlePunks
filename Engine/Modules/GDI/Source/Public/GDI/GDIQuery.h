#ifndef CORECLIENT_GDIQUERY_H
#define CORECLIENT_GDIQUERY_H

#include <cstdint>

/*
 * OCCLUSION QUERIES (GDICreateOcclusionQueries): a set of Capacity queries,
 * each asking whether any sample of the draws between its Begin and End
 * passed the depth test. Used a round at a time, at most one round per
 * GDIBeginRendering block:
 *
 *   GDIBeginOcclusionRound(Set, Count)   queries [0, Count) are this round's
 *   GDIBeginOcclusionQuery / End...      around draws, inside a pass; one open at a time
 *   GDIResolveOcclusionQueries(Set)      after the last End, before a draw they predicate
 *
 * Then GDISetOcclusionPredicate(Set, i) skips the draws after it, on the GPU,
 * if query i found nothing -- where the backend can predicate. The CPU sees a
 * round's answers once its block has finished on the GPU, from the next
 * block on (GDIReadOcclusionResults), without waiting for it.
 *
 * Only where GDISupportsOcclusionQueries(). Created on the render thread;
 * Release defers the free until the GPU is done with it, as a GDIBuffer's.
 */
class GDIOcclusionQueries
{
public:
    explicit GDIOcclusionQueries(const uint32_t InCapacity) : Capacity(InCapacity) {}
    virtual ~GDIOcclusionQueries() = default;

    uint32_t GetCapacity() const { return Capacity; }

    /* Frees it once the GPU no longer uses it; the object is then deleted. */
    virtual void Release() = 0;

protected:
    uint32_t Capacity = 0;
};

#endif //CORECLIENT_GDIQUERY_H
