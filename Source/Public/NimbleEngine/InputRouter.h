#ifndef NIMBLEENGINE_INPUTROUTER_H
#define NIMBLEENGINE_INPUTROUTER_H

#include <cstdint>

#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/NimbleEngineAPI.h"

class GameWindow;

/*
 * HOST SIDE. Turns a GameWindow's SDL input into one InputState per frame:
 * events accumulate as they arrive (inside GameWindowSystem::PumpEvents), and
 * Latch() publishes them as this frame's snapshot. The launcher and the
 * editor's Play-in-Editor each own one; a game reads the snapshot through
 * GameSession::GetInput(), never this.
 *
 * Attach takes over the window's delegate table. A host that needs other
 * delegates (close, resize) sets them on the table GetDelegatesFor fills.
 * Focus loss releases every key, so nothing sticks down across an alt-tab.
 */
namespace NimbleEngine
{
    class NE_API InputRouter
    {
    public:
        InputRouter() = default;
        ~InputRouter();

        InputRouter(const InputRouter&)            = delete;
        InputRouter& operator=(const InputRouter&) = delete;

        /* Routes InWindow's input here (its delegates are replaced). */
        void Attach(GameWindow* InWindow);

        /* Restores the window's delegates to empty. Safe if never attached. */
        void Detach();

        /* Publishes what arrived since the last Latch as this frame's state,
         * and starts accumulating the next. Once per frame, after the pump. */
        void Latch(uint64_t InFrameNumber);

        /* This frame's snapshot. */
        const InputState& GetState() const { return Current; }

        /* What is accumulating: for a host that feeds events itself (the
         * editor's Qt fallback) and for tests. */
        InputState& GetPending() { return Pending; }

        /* The window this routes, if it still exists. */
        GameWindow* GetWindow() const;

    private:
        /* By id: PumpEvents reaps a closed window, so a pointer could dangle. */
        uint32_t    WindowID = 0;
        InputState  Pending;
        InputState  Current;
    };
}

#endif //NIMBLEENGINE_INPUTROUTER_H
