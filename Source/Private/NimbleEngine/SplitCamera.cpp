#include "Public/NimbleEngine/SplitCamera.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Public/GameCore/TransformMath.h"

/*
 * SPLITSCREEN's dynamic split, as plain math (SplitCamera.h): GameSession
 * places its cameras and hands the renderer its sites; the tests check it
 * alone.
 *
 * TWO PLAYERS, exactly, in perspective: every camera moves in one plane (level
 * with the shared camera's eye; its forward's plane for a camera looking flat).
 * The shared camera is placed so the two players sit symmetrically about the
 * screen's centre; once they are farther apart on screen than the centres of
 * the two halves the split line would make, each view's camera is placed so
 * its player sits exactly at the centre of their half (along the way between
 * them). At the moment of the split both conditions hold for one camera, so
 * the views open with no jump.
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

        Vector3 Cross(const Vector3& InA, const Vector3& InB)
        {
            return Vector3(InA.GetY() * InB.GetZ() - InA.GetZ() * InB.GetY(),
                           InA.GetZ() * InB.GetX() - InA.GetX() * InB.GetZ(),
                           InA.GetX() * InB.GetY() - InA.GetY() * InB.GetX());
        }

        /* One frame's camera geometry over the window. */
        struct Lens
        {
            Basis  Axes;
            double TanX = 1.0, TanY = 1.0;
            double Wide = 1.0, High = 1.0;
            double Distance = 1.0;
        };

        Lens LensOf(const SplitCameraSettings& InSettings, const EngineCore::Size InWindow)
        {
            Lens Out;
            Out.Axes     = BasisOf(InSettings);
            Out.Wide     = double(std::max(InWindow.Width, 1));
            Out.High     = double(std::max(InWindow.Height, 1));
            Out.TanY     = std::tan(double(std::clamp(InSettings.FovY, 1.0f, 170.0f)) * 0.5 * PI / 180.0);
            Out.TanX     = Out.TanY * Out.Wide / Out.High;
            Out.Distance = std::max(double(InSettings.Distance), 1.0);
            return Out;
        }

        /* InPoint seen from an eye at InEye: window pixels, row 0 the top. False behind the eye. */
        bool PixelFrom(const Lens& InLens, const Vector3& InEye, const Vector3& InPoint, double& OutX, double& OutY)
        {
            const Vector3 Seen  = InPoint - InEye;
            const double  Depth = Seen.Dot(InLens.Axes.Forward);
            if(Depth <= 1e-3) { return false; }
            const double NX = Seen.Dot(InLens.Axes.Right) / Depth / InLens.TanX;
            const double NY = Seen.Dot(InLens.Axes.Up) / Depth / InLens.TanY;
            OutX = (NX + 1.0) * 0.5 * InLens.Wide;
            OutY = (1.0 - NY) * 0.5 * InLens.High;
            return true;
        }

        /* The plane every camera's eye moves in, through InEye: level for a camera looking
         * down (or up), its forward's for one looking flat (a side view). */
        Vector3 PlaneNormalOf(const Lens& InLens)
        {
            return std::abs(InLens.Axes.Forward.GetY()) > 0.3 ? Vector3(0.0, 1.0, 0.0) : InLens.Axes.Forward;
        }

        /* The eye in that plane (through InPlaneEye) that sees InPoint InOffsetX/Y pixels from the
         * screen's centre: back along the ray through that pixel, to the plane. */
        Vector3 EyeSeeing(const Lens& InLens, const Vector3& InPlaneEye, const Vector3& InPoint, const double InOffsetX, const double InOffsetY)
        {
            const double  NX  = InOffsetX / (0.5 * InLens.Wide);
            const double  NY  = -InOffsetY / (0.5 * InLens.High);
            const Vector3 Ray = InLens.Axes.Forward + InLens.Axes.Right * (NX * InLens.TanX) + InLens.Axes.Up * (NY * InLens.TanY);
            const Vector3 N   = PlaneNormalOf(InLens);
            const double  Across = N.Dot(Ray);
            if(std::abs(Across) < 1e-6) { return InPoint - InLens.Axes.Forward * InLens.Distance; }
            const double  Along = N.Dot(InPoint - InPlaneEye) / Across;
            return InPoint - Ray * Along;
        }

        /* The shared eye, in the plane through InStart, from which InA and InB sit symmetrically
         * about the screen's centre (a few Newton steps from InStart). False if either is
         * behind it -- they do not fit. */
        bool SharedEye(const Lens& InLens, const Vector3& InStart, const Vector3& InA, const Vector3& InB, Vector3& OutEye,
                       double& OutAX, double& OutAY, double& OutBX, double& OutBY)
        {
            const Vector3 N  = PlaneNormalOf(InLens);
            Vector3 Across   = InLens.Axes.Right - N * N.Dot(InLens.Axes.Right);
            if(Across.GetLength() < 1e-9) { Across = InLens.Axes.Up - N * N.Dot(InLens.Axes.Up); }
            Across = Across.GetNormalize();
            const Vector3 Along = Cross(N, Across).GetNormalize();

            const auto Miss = [&](const Vector3& InEye, double& OutX, double& OutY)
            {
                double AX, AY, BX, BY;
                if(!PixelFrom(InLens, InEye, InA, AX, AY) || !PixelFrom(InLens, InEye, InB, BX, BY)) { return false; }
                OutX = 0.5 * (AX + BX) - 0.5 * InLens.Wide;
                OutY = 0.5 * (AY + BY) - 0.5 * InLens.High;
                return true;
            };

            Vector3 Eye = InStart;
            const double Step = InLens.Distance * 1e-3;
            for(int Pass = 0; Pass < 8; ++Pass)
            {
                double FX, FY;
                if(!Miss(Eye, FX, FY)) { return false; }
                if(std::abs(FX) < 1e-3 && std::abs(FY) < 1e-3) { break; }
                double X1, Y1, X2, Y2;
                if(!Miss(Eye + Across * Step, X1, Y1) || !Miss(Eye + Along * Step, X2, Y2)) { return false; }
                const double J11 = (X1 - FX) / Step, J21 = (Y1 - FY) / Step;
                const double J12 = (X2 - FX) / Step, J22 = (Y2 - FY) / Step;
                const double Det = J11 * J22 - J12 * J21;
                if(std::abs(Det) < 1e-12) { break; }
                const double DA = ( J22 * FX - J12 * FY) / Det;
                const double DB = (-J21 * FX + J11 * FY) / Det;
                Eye = Eye - Across * DA - Along * DB;
            }
            OutEye = Eye;
            return PixelFrom(InLens, Eye, InA, OutAX, OutAY) && PixelFrom(InLens, Eye, InB, OutBX, OutBY);
        }

        /* How far from the centre, along InU, the centre of the window's half on InU's far side is:
         * the half's centroid (a rectangle cut through its centre), projected on InU. */
        double HalfCentre(const Lens& InLens, const double InUX, const double InUY)
        {
            const double HW = 0.5 * InLens.Wide, HH = 0.5 * InLens.High;
            const double Corners[4][2] = { { -HW, -HH }, { HW, -HH }, { HW, HH }, { -HW, HH } };
            double Kept[8][2];
            int    Count = 0;
            for(int Index = 0; Index < 4; ++Index)   // Sutherland-Hodgman: keep where p.u >= 0
            {
                const double* A = Corners[Index];
                const double* B = Corners[(Index + 1) % 4];
                const double  SA = A[0] * InUX + A[1] * InUY, SB = B[0] * InUX + B[1] * InUY;
                if(SA >= 0.0) { Kept[Count][0] = A[0]; Kept[Count][1] = A[1]; ++Count; }
                if((SA >= 0.0) != (SB >= 0.0))
                {
                    const double T = SA / (SA - SB);
                    Kept[Count][0] = A[0] + (B[0] - A[0]) * T;
                    Kept[Count][1] = A[1] + (B[1] - A[1]) * T;
                    ++Count;
                }
            }
            double Area = 0.0, CX = 0.0, CY = 0.0;
            for(int Index = 0; Index < Count; ++Index)
            {
                const double* A = Kept[Index];
                const double* B = Kept[(Index + 1) % Count];
                const double  Cross2 = A[0] * B[1] - B[0] * A[1];
                Area += Cross2;
                CX   += (A[0] + B[0]) * Cross2;
                CY   += (A[1] + B[1]) * Cross2;
            }
            if(std::abs(Area) < 1e-9) { return 0.25 * std::min(InLens.Wide, InLens.High); }
            CX /= 3.0 * Area;
            CY /= 3.0 * Area;
            return std::max(CX * InUX + CY * InUY, 1.0);
        }

        /* InPoint on the screen of a camera looking at InFocus: NDC, +Y up; far off when behind it. */
        void Project(const Lens& InLens, const Vector3& InFocus, const Vector3& InPoint, double& OutX, double& OutY)
        {
            double PX, PY;
            if(!PixelFrom(InLens, InFocus - InLens.Axes.Forward * InLens.Distance, InPoint, PX, PY))
            {
                OutX = OutY = 1e6;
                return;
            }
            OutX = PX / (0.5 * InLens.Wide) - 1.0;
            OutY = 1.0 - PY / (0.5 * InLens.High);
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

    bool ProjectSplitCamera(const SplitCameraSettings& InSettings, const EngineCore::Vector3& InFocus, const EngineCore::Vector3& InPoint,
                            const EngineCore::Size InWindow, float& OutX, float& OutY)
    {
        const Lens Lens = LensOf(InSettings, InWindow);
        double X, Y;
        if(!PixelFrom(Lens, InFocus - Lens.Axes.Forward * Lens.Distance, InPoint, X, Y)) { return false; }
        OutX = float(X);
        OutY = float(Y);
        return true;
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

        const Lens Lens = LensOf(InSettings, InWindow);
        const Vector3 Back = Lens.Axes.Forward * Lens.Distance;   // focus - eye

        Vector3 Centre;
        for(uint32_t Index = 0; Index < InCount; ++Index) { Centre = Centre + InPlayers[Index]; }
        Centre = Centre * (1.0 / double(InCount));
        OutFrame.Focus[0] = Centre;
        if(InCount == 1)
        {
            bInOutAngleSet = false;
            return;
        }

        if(InCount > 2)
        {
            /* Three or four: one camera on their centroid while all are within Margin of the centre;
             * else the fixed layout, a camera on each (an N-way split is later). */
            const double Margin = std::clamp(double(InSettings.Margin), 0.01, 1.0);
            double Out = 0.0;
            for(uint32_t Index = 0; Index < InCount; ++Index)
            {
                double X, Y;
                Project(Lens, Centre, InPlayers[Index], X, Y);
                Out = std::max({ Out, std::abs(X) / Margin, std::abs(Y) / Margin });
            }
            bInOutAngleSet = false;
            if(Out <= 1.0) { return; }
            OutFrame.ViewCount = InCount;
            for(uint32_t Index = 0; Index < InCount; ++Index) { OutFrame.Focus[Index] = InPlayers[Index]; }
            return;
        }

        /* Two: the shared eye that holds them symmetrically about the centre, and how far apart on
         * screen, and which way, they are from it. */
        Vector3 Eye;
        double AX = 0.0, AY = 0.0, BX = 0.0, BY = 0.0;
        const bool bSeen = SharedEye(Lens, Centre - Back, InPlayers[0], InPlayers[1], Eye, AX, AY, BX, BY);
        double Apart = std::numeric_limits<double>::infinity();
        double Target;
        if(bSeen)
        {
            Apart  = std::hypot(BX - AX, BY - AY);
            Target = Apart > 1e-6 ? std::atan2(BY - AY, BX - AX) : double(InOutAngle);
        }
        else
        {
            /* One behind the shared eye: the way between them as the camera turns it. */
            const Vector3 Way = InPlayers[1] - InPlayers[0];
            Eye    = Centre - Back;
            Target = std::atan2(-Way.Dot(Lens.Axes.Up) / Lens.TanY * Lens.High, Way.Dot(Lens.Axes.Right) / Lens.TanX * Lens.Wide);
        }

        /* Together while each is no farther out than the centre of their half. */
        const double Reach = HalfCentre(Lens, std::cos(Target), std::sin(Target));
        if(bSeen && Apart <= 2.0 * Reach)
        {
            OutFrame.Focus[0] = Eye + Back;
            bInOutAngleSet    = false;   // the next split snaps to its line
            return;
        }

        /* Apart: the line turns toward theirs, and each view's camera holds its player at the centre
         * of their half, along it. At the split this is the shared eye itself. */
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
        const double UX = std::cos(double(InOutAngle)), UY = std::sin(double(InOutAngle));
        const double Hold = HalfCentre(Lens, UX, UY);
        OutFrame.ViewCount = 2;
        OutFrame.bSites    = true;
        for(uint32_t Index = 0; Index < 2; ++Index)
        {
            const double Side = Index == 0 ? -Hold : Hold;
            OutFrame.Focus[Index] = EyeSeeing(Lens, Eye, InPlayers[Index], UX * Side, UY * Side) + Back;
        }

        /* The sites, either side of the centre along the line's normal: their bisector is the line. */
        const double SiteReach = 0.25 * std::min(Lens.Wide, Lens.High);
        const double CX = 0.5 * Lens.Wide, CY = 0.5 * Lens.High;
        OutFrame.SiteX[0] = float(CX - UX * SiteReach);
        OutFrame.SiteY[0] = float(CY - UY * SiteReach);
        OutFrame.SiteX[1] = float(CX + UX * SiteReach);
        OutFrame.SiteY[1] = float(CY + UY * SiteReach);

        /* The divider fades in over the first fifth past the split. */
        const double Past = bSeen ? Apart / (2.0 * Reach) : 2.0;
        OutFrame.DividerAlpha = float(std::clamp((Past - 1.0) / 0.2, 0.0, 1.0));
    }
}
