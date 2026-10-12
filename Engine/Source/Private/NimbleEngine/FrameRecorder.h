#ifndef NIMBLEENGINE_FRAMERECORDER_H
#define NIMBLEENGINE_FRAMERECORDER_H

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>

namespace RenderCore { class Renderer; }

/*
 * RECORDING A VIDEO of the game as it runs: -record=<file.mp4> on the
 * launcher's command line ([Record] File), at [Record] Fps (30) for
 * [Record] Seconds (0: until the game ends), starting [Record] StartSeconds
 * in. Each frame is the scene's final picture with the UI over it and no
 * "stat" panel (Renderer::RequestSceneCapture), sent raw to an ffmpeg on the
 * PATH, which encodes H.264.
 *
 * Real time: a frame is taken every 1/Fps seconds of the wall clock, and one
 * that came late is repeated to keep the video's clock with the game's, so a
 * slow frame shows as a stutter rather than the video running fast. Reading
 * the picture back stalls the GPU briefly, so a recording runs a little
 * slower than the game would.
 *
 * Editor-target builds (WITH_EDITOR), where the renderer can read a scene
 * back; elsewhere -record warns and does nothing. Not with -headless: there
 * is no picture.
 */
namespace NimbleEngine
{
    class FrameRecorder
    {
    public:
        FrameRecorder() = default;
        ~FrameRecorder();
        FrameRecorder(const FrameRecorder&)            = delete;
        FrameRecorder& operator=(const FrameRecorder&) = delete;

        /* Reads [Record]; false (nothing to do) without a File. */
        bool Start();

        /* After each frame's EndFrame: takes what was read back, asks for the next. */
        void Tick(RenderCore::Renderer& InRenderer);

        /* Closes the video. Idempotent; the destructor calls it. */
        void Stop();

        bool IsActive() const { return bActive; }

    private:
        struct Pending
        {
            uint64_t Request = 0;
            uint64_t Slot    = 0;   // the video frame it stands for
        };

        bool OpenEncoder(int InWidth, int InHeight);

        bool                                  bActive  = false;
        std::string                           File;
        double                                Fps          = 30.0;
        double                                Seconds      = 0.0;
        double                                StartSeconds = 0.0;
        std::chrono::steady_clock::time_point Began;
        std::deque<Pending>                   Requests;
        uint64_t                              NextSlot    = 0;   // the next video frame to ask for
        uint64_t                              Written     = 0;   // video frames written, repeats included
        uint64_t                              Captured    = 0;   // pictures read back
        std::FILE*                            Encoder     = nullptr;
        int                                   Width = 0, Height = 0;
    };
}

#endif //NIMBLEENGINE_FRAMERECORDER_H
