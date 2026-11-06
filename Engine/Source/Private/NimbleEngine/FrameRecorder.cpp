#include "Private/NimbleEngine/FrameRecorder.h"

#include <algorithm>
#include <cmath>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreImage.h"
#include "Public/Core/CoreLog.h"
#include "Public/RenderCore/Renderer.h"

/* FrameRecorder.h: the game, as a video. */

NIMBLE_CONFIG_STRING(GRecordFile,         "Record", "File",         "", nullptr);   // -record=<file.mp4>
NIMBLE_CONFIG_FLOAT(GRecordFps,           "Record", "Fps",          30.0f, nullptr);
NIMBLE_CONFIG_FLOAT(GRecordSeconds,       "Record", "Seconds",      0.0f, nullptr);  // 0: until the game ends
NIMBLE_CONFIG_FLOAT(GRecordStartSeconds,  "Record", "StartSeconds", 0.0f, nullptr);  // the level loading, say, left out
NIMBLE_CONFIG_INT(GRecordStartFrame,      "Record", "StartFrame",   0, nullptr);     // > 0: by frame, from this one (FrameRecorder.h)

#if defined(_WIN32)
    #define NM_POPEN  _popen
    #define NM_PCLOSE _pclose
    #define NM_PIPE_WRITE "wb"
#else
    #define NM_POPEN  popen
    #define NM_PCLOSE pclose
    #define NM_PIPE_WRITE "w"
#endif

namespace NimbleEngine
{
    FrameRecorder::~FrameRecorder()
    {
        Stop();
    }

    bool FrameRecorder::Start()
    {
        File = GRecordFile.GetString();
        if(File.empty()) { return false; }
#if !WITH_EDITOR
        CORE_LOG_WARN("-record: this build cannot read its picture back (an Editor-target build can); not recording")
        return false;
#else
        Fps          = std::clamp(double(GRecordFps.GetFloat()), 1.0, 120.0);
        Seconds      = std::max(0.0, double(GRecordSeconds.GetFloat()));
        StartSeconds = std::max(0.0, double(GRecordStartSeconds.GetFloat()));
        Began        = std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(StartSeconds * 1000.0));
        StartFrame   = uint64_t(std::max(0, GRecordStartFrame.GetInt()));
        bActive      = true;
        CORE_LOG("Recording " + File + " at " + std::to_string(int(Fps)) + " fps"
                 + (Seconds > 0.0 ? ", " + std::to_string(Seconds) + " s" : std::string(", until the game ends")))
        return true;
#endif
    }

    bool FrameRecorder::OpenEncoder(const int InWidth, const int InHeight)
    {
        Width  = InWidth;
        Height = InHeight;
        /* Raw RGBA in at a constant rate; even dimensions out, as yuv420p needs. */
        const std::string Command = "ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s "
                                  + std::to_string(Width) + "x" + std::to_string(Height) + " -r " + std::to_string(int(Fps))
                                  + " -i - -vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\" -c:v libx264 -preset veryfast -crf 18"
                                  + " -pix_fmt yuv420p -movflags +faststart \"" + File + "\"";
        Encoder = NM_POPEN(Command.c_str(), NM_PIPE_WRITE);
        if(!Encoder)
        {
            CORE_LOG_ERROR("-record: could not start ffmpeg (is it on the PATH?); not recording")
            bActive = false;
            return false;
        }
        return true;
    }

    void FrameRecorder::Tick(RenderCore::Renderer& InRenderer)
    {
#if WITH_EDITOR
        if(!bActive) { return; }

        /* Pictures read back since: each fills the video up to its slot. */
        while(!Requests.empty())
        {
            EngineCore::Image Picture;
            std::string       Error;
            if(!InRenderer.TakeMaterialPreview(Requests.front().Request, Picture, Error)) { break; }
            const uint64_t Slot = Requests.front().Slot;
            Requests.pop_front();
            if(!Error.empty() || Picture.GetWidth() <= 0) { continue; }

            const EngineCore::Image Rgba = Picture.GetFormat() == EngineCore::EImageFormat::RGBA8
                                         ? Picture : Picture.ConvertedTo(EngineCore::EImageFormat::RGBA8);
            if(Rgba.GetWidth() <= 0) { continue; }
            if(!Encoder && !OpenEncoder(Rgba.GetWidth(), Rgba.GetHeight())) { return; }
            if(Rgba.GetWidth() != Width || Rgba.GetHeight() != Height) { continue; }   // the window was resized: skipped

            /* This picture, as often as it takes to reach its slot -- a late one repeats. */
            for(; Written <= Slot; ++Written)
            {
                if(std::fwrite(Rgba.GetData(), 1, Rgba.GetSizeBytes(), Encoder) != Rgba.GetSizeBytes())
                {
                    CORE_LOG_ERROR("-record: ffmpeg stopped taking frames; recording ends")
                    Stop();
                    return;
                }
            }
            ++Captured;
        }

        /* The next picture, once its slot's time has come: by the wall clock, or a frame a slot. */
        ++Ticks;
        const double Elapsed = StartFrame > 0 ? (double(Ticks) - double(StartFrame)) / Fps
                                              : std::chrono::duration<double>(std::chrono::steady_clock::now() - Began).count();
        if(Elapsed < 0.0) { return; }
        if(Seconds > 0.0 && Elapsed >= Seconds)
        {
            if(Requests.empty()) { Stop(); }
            return;
        }
        const uint64_t Due = uint64_t(std::floor(Elapsed * Fps));
        if(Due >= NextSlot && Requests.size() < 3)
        {
            const uint64_t Request = InRenderer.RequestSceneCapture();
            if(Request != 0) { Requests.push_back({ Request, Due }); }
            NextSlot = Due + 1;
        }
#else
        (void)InRenderer;
#endif
    }

    void FrameRecorder::Stop()
    {
        if(!bActive && !Encoder) { return; }
        bActive = false;
        Requests.clear();
        if(Encoder)
        {
            NM_PCLOSE(Encoder);
            Encoder = nullptr;
            CORE_LOG("Recorded " + File + ": " + std::to_string(Written) + " frames (" + std::to_string(Captured)
                     + " pictures), " + std::to_string(double(Written) / Fps) + " s")
        }
    }
}
