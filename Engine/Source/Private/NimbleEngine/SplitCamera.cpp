#include "Public/NimbleEngine/SplitCamera.h"

#include <algorithm>
#include <cmath>

#include "Public/GameCore/TransformMath.h"

/*
 * SPLITSCREEN's dynamic split, as plain math (SplitCamera.h): GameSession
 * places its cameras and hands the renderer its sites; the tests check it
 * alone.
 */
namespace NimbleEngine
{
    namespace
    {
        using EngineCore::Vector3;

        constexpr double PI = 3.14159265358979323846;

        /* The cameras' basis: forward from the Rotation, as a Camera component's
         * (LightForward), right and up from it and world up, as the renderer's
         * LookAt makes them (left-handed: +X right, +Y up, +Z forward). */
        struct Basis
        {
            Vector3 Forward, Right, Up;
        };

        Basis BasisOf(const SplitCameraSettings& InSettings)
        {
            Basis Out;
            Out.Forward = GameCore::TransformMath::EulerToRotation(InSettings.Rotation).GetAxis(2);
            if(std::abs(Out.Forward.GetY()) > 0.9999)   // never straight down, which LookAt cannot aim along
            {
                Out.Forward = Vector3(0.0, Out.Forward.GetY() > 0.0 ? 0.9999 : -0.9999, 0.01).GetNormalize();
            }
            const Vector3 F = Out.Forward;
            // world up x forward: (0,1,0) x F
            Out.Right = Vector3(F.GetZ(), 0.0, -F.GetX()).GetNormalize();
            // forward x right
            const Vector3 R = Out.Right;
            Out.Up = Vector3(F.GetY() * R.GetZ() - F.GetZ() * R.GetY(),
                             F.GetZ() * R.GetX() - F.GetX() * R.GetZ(),
                             F.GetX() * R.GetY() - F.GetY() * R.GetX());
            return Out;
        }

        /* InPoint on the screen of a camera looking at InFocus: NDC, +Y up; far off when behind it. */
        void Project(const SplitCameraSettings& InSettings, const Basis& InBasis, const Vector3& InFocus, const Vector3& InPoint,
                     const double InTanX, const double InTanY, double& OutX, double& OutY)
        {
            const Vector3 Eye   = InFocus - InBasis.Forward * double(InSettings.Distance);
            const Vector3 Seen  = InPoint - Eye;
            const double  Depth = Seen.Dot(InBasis.Forward);
            if(Depth <= 1e-3)
            {
                OutX = OutY = 1e6;
                return;
            }
            OutX = Seen.Dot(InBasis.Right) / Depth / InTanX;
            OutY = Seen.Dot(InBasis.Up) / Depth / InTanY;
        }

        /* The shortest turn from InFrom to InTo, radians in (-pi, pi]. */
        double TurnBetween(const double InFrom, const double InTo)
        {
            double Turn = std::fmod(InTo - InFrom, 2.0 * PI);
            if(Turn > PI)   { Turn -= 2.0 * PI; }
            if(Turn <= -PI) { Turn += 2.0 * PI; }
            return Turn;
        }
    }

    EngineCore::Vector3 SplitCameraPosition(const SplitCameraSettings& InSettings, const EngineCore::Vector3& InFocus)
    {
        return InFocus - BasisOf(InSettings).Forward * double(InSettings.Distance);
    }

    void SolveSplitCamera(const SplitCameraSettings& InSettings, const EngineCore::Vector3* InPlayers, uint32_t InCount,
                          const EngineCore::Size InWindow, const float InDeltaSeconds, float& InOutAngle, bool& bInOutAngleSet,
                          SplitCameraFrame& OutFrame)
    {
        OutFrame = SplitCameraFrame();
        InCount  = std::min<uint32_t>(InCount, 4);
        if(InCount == 0 || !InPlayers)
        {
            bInOutAngleSet = false;
            return;
        }

        const Basis  Axes   = BasisOf(InSettings);
        const double Aspect = InWindow.Height > 0 ? double(InWindow.Width) / double(InWindow.Height) : 1.0;
        const double TanY   = std::tan(double(std::clamp(InSettings.FovY, 1.0f, 170.0f)) * 0.5 * PI / 180.0);
        const double TanX   = TanY * Aspect;
        const double Margin = std::clamp(double(InSettings.Margin), 0.01, 1.0);

        /* The shared camera, on the players' centroid, and how far out the farthest of them is (1: at the margin). */
        Vector3 Centre;
        for(uint32_t Index = 0; Index < InCount; ++Index) { Centre = Centre + InPlayers[Index]; }
        Centre = Centre * (1.0 / double(InCount));
        double Out = 0.0;
        double X[4] = {}, Y[4] = {};
        for(uint32_t Index = 0; Index < InCount; ++Index)
        {
            Project(InSettings, Axes, Centre, InPlayers[Index], TanX, TanY, X[Index], Y[Index]);
            Out = std::max({ Out, std::abs(X[Index]) / Margin, std::abs(Y[Index]) / Margin });
        }

        OutFrame.Focus[0] = Centre;
        if(InCount == 1 || Out <= 1.0)
        {
            bInOutAngleSet = false;   // the next split snaps to its line
            return;
        }

        if(InCount > 2)
        {
            /* Three or four apart: the fixed layout, a camera on each (N-way Voronoi is later). */
            OutFrame.ViewCount = InCount;
            for(uint32_t Index = 0; Index < InCount; ++Index) { OutFrame.Focus[Index] = InPlayers[Index]; }
            bInOutAngleSet = false;
            return;
        }

        /* Two apart. The separation that just fits, T = L / Out (a screen offset grows with the
         * world one); each view's focus slides from the midpoint toward its player. */
        const double Keep = 1.0 / Out;   // T / L
        OutFrame.ViewCount = 2;
        OutFrame.bSites    = true;
        for(uint32_t Index = 0; Index < 2; ++Index)
        {
            OutFrame.Focus[Index] = Centre + (InPlayers[Index] - Centre) * (1.0 - Keep);
        }

        /* The line: at right angles to the way from player 1 to player 2 on screen -- the
         * way between them as the camera turns it, which holds even when one is behind it. */
        const double Wide = double(std::max(InWindow.Width, 1)), High = double(std::max(InWindow.Height, 1));
        const Vector3 Apart = InPlayers[1] - InPlayers[0];
        const double DX = Apart.Dot(Axes.Right) / TanX * Wide * 0.5;
        const double DY = -Apart.Dot(Axes.Up) / TanY * High * 0.5;   // pixel rows run down
        const double Target = std::atan2(DY, DX);
        if(!bInOutAngleSet || InSettings.TurnRate <= 0.0f)
        {
            InOutAngle     = float(Target);
            bInOutAngleSet = true;
        }
        else
        {
            const double Follow = 1.0 - std::exp(-double(InSettings.TurnRate) * double(std::max(InDeltaSeconds, 0.0f)));
            InOutAngle = float(double(InOutAngle) + TurnBetween(double(InOutAngle), Target) * Follow);
        }

        /* Each view's site a quarter of the way out from the centre, player 1's toward their side. */
        const double Reach = 0.25 * std::min(Wide, High);
        const double CX = Wide * 0.5, CY = High * 0.5;
        const double UX = std::cos(double(InOutAngle)), UY = std::sin(double(InOutAngle));
        OutFrame.SiteX[0] = float(CX - UX * Reach);
        OutFrame.SiteY[0] = float(CY - UY * Reach);
        OutFrame.SiteX[1] = float(CX + UX * Reach);
        OutFrame.SiteY[1] = float(CY + UY * Reach);

        /* The divider fades in over the first fifth past the merge distance. */
        OutFrame.DividerAlpha = float(std::clamp((Out - 1.0) / 0.2, 0.0, 1.0));
    }
}
