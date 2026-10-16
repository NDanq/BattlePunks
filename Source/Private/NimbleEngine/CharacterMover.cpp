#include "Public/NimbleEngine/CharacterMover.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/PhysicsFacade.h"

#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/Components/CameraComponent.h"

/*
 * CharacterMover.h: the CharacterMove simulation, the presentation system and
 * the component's tables. Generalised from WorldsLab's RangeMove
 * (Games/WorldsLab/WorldsLabRange.cpp): the same walk, its constants made the
 * component's tunables.
 *
 *   ground   rays down round the feet find the highest walkable surface
 *            within a step: floors, ramps, stairs
 *   walls    rays across, at three heights, slide the move along what they
 *            hit; rays round the body push it back out of anything it is in
 *   air      gravity, a jump, a head check
 *   view     yaw and pitch from the look axes; aiming blends the camera in
 *            and slows the look
 *   body     turns to the way it moves, or to the view (aiming, FaceView)
 */

namespace NimbleEngine
{
namespace
{
    constexpr double DegToRad = 3.14159265358979323846 / 180.0;
    constexpr double WallNormalY = 0.7;   // a hit flatter than this is not a wall

    struct Vec { double X, Y, Z; };

    /* A step's working copy of the state, in doubles. */
    struct MoverState
    {
        double BodyYaw = 0.0, CameraYaw = 0.0, Pitch = 8.0, Scope = 0.0;
        double VelX = 0.0, VelY = 0.0, VelZ = 0.0;
        double AirTime = 0.0;
        uint8_t JumpCount = 0;
        bool bGrounded = false, bCrouched = false, bStarted = false;
    };

    MoverState LoadMover(const CharacterMoverComponent& In)
    {
        MoverState S;
        S.BodyYaw = In.BodyYaw; S.CameraYaw = In.CameraYaw; S.Pitch = In.Pitch; S.Scope = In.Scope;
        S.VelX = In.VelX; S.VelY = In.VelY; S.VelZ = In.VelZ; S.AirTime = In.AirTime;
        S.JumpCount = In.JumpCount;
        S.bGrounded = In.bGrounded != 0; S.bCrouched = In.bCrouched != 0; S.bStarted = In.bStarted != 0;
        return S;
    }

    void StoreMover(const MoverState& S, CharacterMoverComponent& Out)
    {
        Out.BodyYaw = float(S.BodyYaw); Out.CameraYaw = float(S.CameraYaw); Out.Pitch = float(S.Pitch); Out.Scope = float(S.Scope);
        Out.VelX = float(S.VelX); Out.VelY = float(S.VelY); Out.VelZ = float(S.VelZ); Out.AirTime = float(S.AirTime);
        Out.JumpCount = S.JumpCount;
        Out.bGrounded = S.bGrounded ? 1 : 0; Out.bCrouched = S.bCrouched ? 1 : 0; Out.bStarted = S.bStarted ? 1 : 0;
    }

    double WrapDegrees(double InDegrees)
    {
        InDegrees = std::fmod(InDegrees + 180.0, 360.0);
        return (InDegrees < 0.0 ? InDegrees + 360.0 : InDegrees) - 180.0;
    }

    double Lerp(const double InA, const double InB, const double InT) { return InA + (InB - InA) * InT; }

    GameCore::EntityID FindChild(GameCore::World& InWorld, const GameCore::EntityID InParent, const char* InName)
    {
        for(GameCore::EntityID C = InWorld.GetEntity(InParent).GetFirstChild(); C.IsSet(); C = InWorld.GetEntity(C).GetNextSibling())
        {
            if(std::strcmp(InWorld.GetEntity(C).GetName(), InName) == 0) { return C; }
        }
        return GameCore::EntityID();
    }

    // --- Bindings ----------------------------------------------------------------

    enum EBinding { B_MoveForward, B_MoveRight, B_LookX, B_LookY, B_Run, B_Jump, B_Crouch, B_Aim, B_Count };

    /* A mover's names resolved in one input map. Kept per (map, names), so
     * avatars on different maps or with renamed bindings each get theirs. */
    struct ResolvedBindings
    {
        char Map[INPUT_NAME_MAX] = {};
        char Names[B_Count][INPUT_NAME_MAX] = {};
        int  Index[B_Count] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    };

    std::vector<ResolvedBindings>& BindingCache()
    {
        static std::vector<ResolvedBindings> Cache;
        return Cache;
    }

    void NamesOf(const CharacterMoverComponent& InMover, const char* (&OutNames)[B_Count])
    {
        OutNames[B_MoveForward] = InMover.MoveForward;
        OutNames[B_MoveRight]   = InMover.MoveRight;
        OutNames[B_LookX]       = InMover.LookX;
        OutNames[B_LookY]       = InMover.LookY;
        OutNames[B_Run]         = InMover.RunAction;
        OutNames[B_Jump]        = InMover.JumpAction;
        OutNames[B_Crouch]      = InMover.CrouchAction;
        OutNames[B_Aim]         = InMover.AimAction;
    }

    bool IsAxis(const int InBinding) { return InBinding <= B_LookY; }

    /* The indices of InMover's bindings in InMap; -1 for one unnamed or not
     * in the map. A name not found yet is looked up again next time (its map
     * may register later, a module loading). */
    const ResolvedBindings& ResolveBindings(const char* InMap, const CharacterMoverComponent& InMover)
    {
        const char* Names[B_Count];
        NamesOf(InMover, Names);
        std::vector<ResolvedBindings>& Cache = BindingCache();
        ResolvedBindings* Found = nullptr;
        for(ResolvedBindings& Each : Cache)
        {
            if(std::strncmp(Each.Map, InMap, INPUT_NAME_MAX) != 0) { continue; }
            bool bSame = true;
            for(int B = 0; B < B_Count && bSame; ++B) { bSame = std::strncmp(Each.Names[B], Names[B], INPUT_NAME_MAX) == 0; }
            if(bSame) { Found = &Each; break; }
        }
        if(!Found)
        {
            Cache.emplace_back();
            Found = &Cache.back();
            std::strncpy(Found->Map, InMap, INPUT_NAME_MAX - 1);
            for(int B = 0; B < B_Count; ++B) { std::strncpy(Found->Names[B], Names[B], INPUT_NAME_MAX - 1); }
        }
        for(int B = 0; B < B_Count; ++B)
        {
            if(Found->Index[B] >= 0 || Found->Names[B][0] == '\0') { continue; }
            Found->Index[B] = IsAxis(B) ? FindInputAxis(Found->Map, Found->Names[B]) : FindInputAction(Found->Map, Found->Names[B]);
        }
        return *Found;
    }

    // --- The walk's rays -----------------------------------------------------------

    /* The body's dimensions this step: the component's, crouched or not. */
    struct BodyShape
    {
        double Radius, Height, StepHeight, SnapDown, GroundNormalY;
        double ProbeHeights[3];   // the wall probes: above a step, the middle, the head
        int32_t Mask;
    };

    BodyShape ShapeOf(const CharacterMoverComponent& InMover, const bool bInCrouched)
    {
        BodyShape Shape;
        Shape.Radius     = (std::max)(1.0, double(InMover.Radius));
        Shape.Height     = (std::max)(Shape.Radius, double(bInCrouched ? InMover.CrouchHeight : InMover.Height));
        Shape.StepHeight = (std::max)(0.0, double(InMover.StepHeight));
        Shape.SnapDown   = (std::max)(0.0, double(InMover.SnapDown));
        Shape.GroundNormalY = std::cos(std::clamp(double(InMover.MaxSlope), 0.0, 89.0) * DegToRad);
        /* RangeMove's 38, 88 and 163 for a 180 body with a 45 step: never a stair tread's 25 multiple. */
        Shape.ProbeHeights[0] = Shape.StepHeight * 38.0 / 45.0;
        Shape.ProbeHeights[1] = Shape.Height * 88.0 / 180.0;
        Shape.ProbeHeights[2] = (std::max)(Shape.ProbeHeights[1], Shape.Height - 17.0);
        Shape.Mask = InMover.Mask;
        return Shape;
    }

    bool Ray(GameCore::World& InWorld, const BodyShape& InShape, const Vec& InFrom, const Vec& InDir, const double InLength,
             PhysicsRaycastHit& OutHit)
    {
        return PhysicsRaycast(InWorld, EngineCore::Vector3(InFrom.X, InFrom.Y, InFrom.Z), EngineCore::Vector3(InDir.X, InDir.Y, InDir.Z),
                              float(InLength), OutHit, InShape.Mask);
    }

    bool HasNormal(const PhysicsRaycastHit& InHit)
    {
        return InHit.Normal.GetX() != 0.0 || InHit.Normal.GetY() != 0.0 || InHit.Normal.GetZ() != 0.0;
    }

    /* A hit of a ray across the ground is a wall, with its normal flattened.
     * Without physics (Bounds) a hit has no normal: a wall facing the ray. */
    bool WallHit(const PhysicsRaycastHit& InHit, const Vec& InDir, Vec& OutNormal)
    {
        if(!HasNormal(InHit))
        {
            OutNormal = { -InDir.X, 0.0, -InDir.Z };
            return true;
        }
        if(std::fabs(InHit.Normal.GetY()) >= WallNormalY) { return false; }
        OutNormal = { InHit.Normal.GetX(), 0.0, InHit.Normal.GetZ() };
        return true;
    }

    /* The move (DX, DZ) from (X, FeetY, Z), slid along whatever the probes hit. */
    void SlideMove(GameCore::World& InWorld, const BodyShape& InShape, double& InOutX, const double InFeetY, double& InOutZ, double DX,
                   double DZ)
    {
        const double Radius = InShape.Radius;
        for(int Pass = 0; Pass < 3; ++Pass)
        {
            const double Length = std::sqrt(DX * DX + DZ * DZ);
            if(Length < 1e-4) { return; }
            const Vec Dir { DX / Length, 0.0, DZ / Length };
            double Nearest = Length + Radius;
            Vec    Normal { 0.0, 0.0, 0.0 };
            for(const double H : InShape.ProbeHeights)
            {
                PhysicsRaycastHit Hit;
                Vec HitNormal { 0.0, 0.0, 0.0 };
                if(Ray(InWorld, InShape, { InOutX, InFeetY + H, InOutZ }, Dir, Length + Radius, Hit) && WallHit(Hit, Dir, HitNormal)
                   && Hit.Distance < Nearest)
                {
                    Nearest = Hit.Distance;
                    Normal  = HitNormal;
                }
            }
            if(Nearest >= Length + Radius)
            {
                InOutX += DX;
                InOutZ += DZ;
                return;
            }
            /* Up to the wall, then what is left along it. */
            const double Allowed = (std::max)(0.0, Nearest - Radius);
            InOutX += Dir.X * Allowed;
            InOutZ += Dir.Z * Allowed;
            const double NLength = std::sqrt(Normal.X * Normal.X + Normal.Z * Normal.Z);
            if(NLength < 1e-4) { return; }
            const double NX = Normal.X / NLength, NZ = Normal.Z / NLength;
            const double LeftX = DX - Dir.X * Allowed, LeftZ = DZ - Dir.Z * Allowed;
            const double Into = LeftX * NX + LeftZ * NZ;
            DX = LeftX - NX * Into;
            DZ = LeftZ - NZ * Into;
        }
    }

    /* Out of anything closer than Radius, round the body at two heights. */
    void PushOut(GameCore::World& InWorld, const BodyShape& InShape, double& InOutX, const double InFeetY, double& InOutZ)
    {
        for(const double H : { InShape.ProbeHeights[0], InShape.ProbeHeights[2] })
        {
            for(int Spoke = 0; Spoke < 8; ++Spoke)
            {
                const double A = Spoke * 45.0 * DegToRad;
                const Vec Dir { std::sin(A), 0.0, std::cos(A) };
                PhysicsRaycastHit Hit;
                Vec Normal { 0.0, 0.0, 0.0 };
                if(Ray(InWorld, InShape, { InOutX, InFeetY + H, InOutZ }, Dir, InShape.Radius, Hit) && WallHit(Hit, Dir, Normal))
                {
                    const double Push = InShape.Radius - Hit.Distance;
                    InOutX -= Dir.X * Push;
                    InOutZ -= Dir.Z * Push;
                }
            }
        }
    }

    /* The highest walkable surface under the circle at (X, Z), searching from
     * InTop down InDepth; false if none. A hit without a normal (Bounds) is
     * walkable. */
    bool FindGround(GameCore::World& InWorld, const BodyShape& InShape, const double InX, const double InTop, const double InZ,
                    const double InDepth, double& OutY)
    {
        bool bFound = false;
        const double R = InShape.Radius * 0.6;
        const Vec Offsets[] = { { 0, 0, 0 }, { R, 0, 0 }, { -R, 0, 0 }, { 0, 0, R }, { 0, 0, -R } };
        for(const Vec& O : Offsets)
        {
            PhysicsRaycastHit Hit;
            if(Ray(InWorld, InShape, { InX + O.X, InTop, InZ + O.Z }, { 0.0, -1.0, 0.0 }, InDepth, Hit)
               && (!HasNormal(Hit) || Hit.Normal.GetY() > InShape.GroundNormalY))
            {
                const double Y = InTop - Hit.Distance;
                if(!bFound || Y > OutY) { OutY = Y; }
                bFound = true;
            }
        }
        return bFound;
    }

    /* Room above (X, FeetY, Z) from InFrom up to InTo (feet-relative). */
    bool HeadClear(GameCore::World& InWorld, const BodyShape& InShape, const double InX, const double InFeetY, const double InZ,
                   const double InFrom, const double InTo)
    {
        PhysicsRaycastHit Hit;
        return !Ray(InWorld, InShape, { InX, InFeetY + InFrom - 10.0, InZ }, { 0.0, 1.0, 0.0 }, (InTo - InFrom) + 10.0, Hit);
    }

    // --- The view ------------------------------------------------------------------

    /* The view's axes for (pitch down-positive, yaw), +Z at yaw 0. */
    void ViewAxes(const double InPitch, const double InYaw, Vec& OutForward, Vec& OutRight, Vec& OutUp)
    {
        const double P = InPitch * DegToRad, Y = InYaw * DegToRad;
        OutForward = { std::sin(Y) * std::cos(P), -std::sin(P), std::cos(Y) * std::cos(P) };
        OutRight   = { std::cos(Y), 0.0, -std::sin(Y) };
        /* Forward x Right: up when level. (RangeMove's Right x Forward points down.) */
        OutUp      = { OutForward.Y * OutRight.Z - OutForward.Z * OutRight.Y, OutForward.Z * OutRight.X - OutForward.X * OutRight.Z,
                       OutForward.X * OutRight.Y - OutForward.Y * OutRight.X };
    }

    double Ease(const double InScope) { return InScope * InScope * (3.0 - 2.0 * InScope); }

    /* The boom's pivot above the feet: lower by the difference when crouched. */
    double BoomHeightOf(const CharacterMoverComponent& InMover)
    {
        return double(InMover.BoomHeight) - (InMover.bCrouched ? double(InMover.Height - InMover.CrouchHeight) : 0.0);
    }

    /* The camera's offset from the pivot, along the view, aim-blended: (side, up, back). */
    void CameraOffset(const CharacterMoverComponent& InMover, double& OutSide, double& OutUp, double& OutBack)
    {
        const double E = Ease(std::clamp(double(InMover.Scope), 0.0, 1.0));
        OutSide = Lerp(InMover.CameraRight, InMover.ScopeRight, E);
        OutUp   = Lerp(InMover.CameraUp, InMover.ScopeUp, E);
        OutBack = Lerp(InMover.CameraBack, InMover.ScopeBack, E);
    }

    void SetAnimFloat(RenderCore::AnimationComponent& InAnimation, const char* InName, const float InValue)
    {
        if(InName[0] != '\0') { InAnimation.SetFloat(InName, InValue); }
    }
}

// === The step ========================================================================

void StepCharacter(GameCore::World& InWorld, const GameCore::EntityID InAvatar, const AvatarInputFrame& InFrame)
{
    GameCore::Entity Avatar = InWorld.GetEntity(InAvatar);
    if(!Avatar.IsValid() || InFrame.Seconds <= 0.0f) { return; }
    /* First: adding it may move the avatar's other components. */
    CharacterMoverComponent* Mover = Avatar.GetComponent<CharacterMoverComponent>();
    if(!Mover) { Mover = Avatar.AddComponent<CharacterMoverComponent>(); }
    GameCore::TransformComponent* Body = Avatar.GetComponent<GameCore::TransformComponent>();
    if(!Mover || !Body) { return; }

    const AvatarHarnessComponent* Harness = Avatar.GetComponent<AvatarHarnessComponent>();
    const ResolvedBindings& Bindings = ResolveBindings(Harness ? Harness->InputMap : "Default", *Mover);
    const auto Axis = [&InFrame, &Bindings](const int InBinding)
    {
        const int Index = Bindings.Index[InBinding];
        return Index >= 0 && Index < int(INPUT_AXES_MAX) ? double(InFrame.Axes[Index]) : 0.0;
    };
    const auto Down = [&InFrame, &Bindings](const int InBinding)
    {
        const int Index = Bindings.Index[InBinding];
        return Index >= 0 && Index < 32 && (InFrame.Down & (1u << Index)) != 0;
    };
    const auto Pressed = [&InFrame, &Bindings](const int InBinding)
    {
        const int Index = Bindings.Index[InBinding];
        return Index >= 0 && Index < 32 && (InFrame.Pressed & (1u << Index)) != 0;
    };

    MoverState S = LoadMover(*Mover);
    const CharacterMoverComponent& T = *Mover;   // the tunables
    const double Dt = double(InFrame.Seconds);
    const double MeshYaw = double(T.MeshYaw);
    if(!S.bStarted)
    {
        S.BodyYaw   = WrapDegrees(double(Body->Rotation.GetY()));   // where it was placed: the Player Start's facing
        S.CameraYaw = S.BodyYaw;
        S.bStarted  = true;
    }
    double BodyYaw = S.BodyYaw;
    double X = Body->Position.GetX(), Y = Body->Position.GetY(), Z = Body->Position.GetZ();

    // --- Crouching: down at once, up when there is room ------------------------------
    if(Down(B_Crouch)) { S.bCrouched = true; }
    else if(S.bCrouched && HeadClear(InWorld, ShapeOf(T, false), X, Y, Z, double(T.CrouchHeight), double(T.Height)))
    {
        S.bCrouched = false;
    }
    const BodyShape Shape = ShapeOf(T, S.bCrouched);

    // --- The view, and the aim ----------------------------------------------------------
    const bool bAiming = Down(B_Aim);
    const double Sensitivity = Lerp(1.0, double(T.ScopeLook), S.Scope);
    S.CameraYaw = WrapDegrees(S.CameraYaw + Axis(B_LookX) * Sensitivity);
    S.Pitch     = std::clamp(S.Pitch + Axis(B_LookY) * Sensitivity, double(T.PitchMin), (std::max)(double(T.PitchMin), double(T.PitchMax)));
    const double ScopeStep = double(T.ScopeRate) * Dt;
    S.Scope = std::clamp(S.Scope + (bAiming ? ScopeStep : -ScopeStep), 0.0, 1.0);

    // --- Walking -----------------------------------------------------------------------
    double Forward = Axis(B_MoveForward), Right = Axis(B_MoveRight);
    const double Amount = std::sqrt(Forward * Forward + Right * Right);
    if(Amount > 1.0) { Forward /= Amount; Right /= Amount; }
    const double CY = S.CameraYaw * DegToRad;
    const double WishX = std::sin(CY) * Forward + std::cos(CY) * Right;
    const double WishZ = std::cos(CY) * Forward - std::sin(CY) * Right;
    const double Top = bAiming ? double(T.AimSpeed)
                     : S.bCrouched ? double(T.CrouchSpeed)
                     : Down(B_Run) ? double(T.RunSpeed) : double(T.WalkSpeed);
    const double TargetX = WishX * Top, TargetZ = WishZ * Top;
    const double DVX = TargetX - S.VelX, DVZ = TargetZ - S.VelZ;
    const double DV = std::sqrt(DVX * DVX + DVZ * DVZ);
    const double Rate = (Amount > 0.05 ? double(T.Accel) : double(T.Decel)) * (S.bGrounded ? 1.0 : double(T.AirControl)) * Dt;
    if(DV <= Rate) { S.VelX = TargetX; S.VelZ = TargetZ; }
    else           { S.VelX += DVX / DV * Rate; S.VelZ += DVZ / DV * Rate; }

    // --- Turning: to the view when aiming (or FaceView), else to where it walks ----------
    const bool bFaceView = bAiming || T.bFaceView != 0;
    const double Speed = std::sqrt(S.VelX * S.VelX + S.VelZ * S.VelZ);
    double Facing = BodyYaw;
    if(bFaceView)          { Facing = S.CameraYaw; }
    else if(Speed > 20.0)  { Facing = std::atan2(S.VelX, S.VelZ) / DegToRad; }
    const double Turn = WrapDegrees(Facing - BodyYaw);
    const double Step = double(T.TurnRate) * Dt * (bFaceView ? 1.6 : 1.0);
    BodyYaw = WrapDegrees(BodyYaw + std::clamp(Turn, -Step, Step));

    // --- Moving: across, then up and down ---------------------------------------------
    SlideMove(InWorld, Shape, X, Y, Z, S.VelX * Dt, S.VelZ * Dt);
    PushOut(InWorld, Shape, X, Y, Z);

    if(S.bGrounded && Pressed(B_Jump))
    {
        S.VelY = double(T.JumpSpeed);
        S.bGrounded = false;
        S.JumpCount = uint8_t(S.JumpCount + 1);
    }
    S.VelY -= double(T.Gravity) * Dt;
    double NewY = Y + S.VelY * Dt;
    if(S.VelY > 0.0 && !HeadClear(InWorld, Shape, X, Y, Z, Shape.Height, Shape.Height + S.VelY * Dt))
    {
        NewY = Y;
        S.VelY = 0.0;
    }
    const double Reach = Shape.StepHeight + (std::max)(0.0, Y - NewY) + (S.bGrounded ? Shape.SnapDown : 0.0);
    double Ground = 0.0;
    if(S.VelY <= 0.0 && FindGround(InWorld, Shape, X, Y + Shape.StepHeight, Z, Reach, Ground))
    {
        NewY = Ground;
        S.VelY = 0.0;
        S.bGrounded = true;
        S.AirTime = 0.0;
    }
    else
    {
        S.bGrounded = false;
        S.AirTime += Dt;
    }
    if(NewY < -2000.0) { NewY = 0.0; S.VelY = 0.0; }   // out of the world: back on the floor

    Body->Position = EngineCore::Vector3(X, NewY, Z);
    S.BodyYaw = BodyYaw;
    Body->Rotation = EngineCore::Vector3(0.0, WrapDegrees(BodyYaw + MeshYaw), 0.0);
    StoreMover(S, *Mover);
}

void TeleportCharacter(GameCore::World& InWorld, const GameCore::EntityID InAvatar, const EngineCore::Vector3& InPosition, const float InYaw)
{
    GameCore::Entity Avatar = InWorld.GetEntity(InAvatar);
    if(!Avatar.IsValid()) { return; }
    CharacterMoverComponent* Mover = Avatar.GetComponent<CharacterMoverComponent>();
    if(!Mover) { Mover = Avatar.AddComponent<CharacterMoverComponent>(); }
    if(!Mover) { return; }
    Mover->BodyYaw   = float(WrapDegrees(InYaw));
    Mover->CameraYaw = Mover->BodyYaw;
    Mover->Pitch     = std::clamp(8.0f, Mover->PitchMin, (std::max)(Mover->PitchMin, Mover->PitchMax));
    Mover->Scope     = 0.0f;
    Mover->VelX = Mover->VelY = Mover->VelZ = 0.0f;
    Mover->AirTime   = 0.0f;
    Mover->bGrounded = 0;
    Mover->bCrouched = 0;
    Mover->bStarted  = 1;
    if(GameCore::TransformComponent* Body = Avatar.GetComponent<GameCore::TransformComponent>())
    {
        Body->Position = InPosition;
        Body->Rotation = EngineCore::Vector3(0.0, WrapDegrees(double(InYaw) + double(Mover->MeshYaw)), 0.0);
    }
}

bool GetCharacterAim(GameCore::World& InWorld, const GameCore::EntityID InAvatar, EngineCore::Vector3& OutOrigin, EngineCore::Vector3& OutDir)
{
    GameCore::Entity Avatar = InWorld.GetEntity(InAvatar);
    if(!Avatar.IsValid()) { return false; }
    const CharacterMoverComponent* Mover = Avatar.GetComponent<CharacterMoverComponent>();
    const GameCore::TransformComponent* Body = Avatar.GetComponent<GameCore::TransformComponent>();
    if(!Mover || !Body) { return false; }
    Vec Fwd, RightV, UpV;
    ViewAxes(Mover->Pitch, Mover->CameraYaw, Fwd, RightV, UpV);
    double Side, Up, Back;
    CameraOffset(*Mover, Side, Up, Back);
    const Vec Pivot { Body->Position.GetX(), Body->Position.GetY() + BoomHeightOf(*Mover), Body->Position.GetZ() };
    OutOrigin = EngineCore::Vector3(Pivot.X + RightV.X * Side + UpV.X * Up - Fwd.X * Back,
                                    Pivot.Y + RightV.Y * Side + UpV.Y * Up - Fwd.Y * Back,
                                    Pivot.Z + RightV.Z * Side + UpV.Z * Up - Fwd.Z * Back);
    OutDir = EngineCore::Vector3(Fwd.X, Fwd.Y, Fwd.Z);
    return true;
}

// === The presentation system =========================================================

void UpdateCharacterMovers(GameCore::World& InWorld, const float InSeconds)
{
    (void)InSeconds;
    std::vector<GameCore::EntityID> Movers;
    InWorld.Each<CharacterMoverComponent>([&Movers](GameCore::Entity InEntity, CharacterMoverComponent&) { Movers.push_back(InEntity.GetID()); });
    for(const GameCore::EntityID Id : Movers)
    {
        GameCore::Entity Avatar = InWorld.GetEntity(Id);
        CharacterMoverComponent* Mover = Avatar.GetComponent<CharacterMoverComponent>();
        const GameCore::TransformComponent* Body = Avatar.GetComponent<GameCore::TransformComponent>();
        if(!Mover || !Body) { continue; }

        // --- The anim state, from the state: this frame's here, the server's elsewhere ---
        if(RenderCore::AnimationComponent* Animation = Avatar.GetComponent<RenderCore::AnimationComponent>())
        {
            SetAnimFloat(*Animation, Mover->AnimSpeed, std::hypot(Mover->VelX, Mover->VelZ));
            SetAnimFloat(*Animation, Mover->AnimGrounded, (Mover->bGrounded != 0 || Mover->AirTime < 0.12f) ? 1.0f : 0.0f);
            /* The spine pitches with the view (an anim state's look-at): its angle is up-positive, the view's down. */
            SetAnimFloat(*Animation, Mover->AnimAimPitch, -Mover->Pitch);
            SetAnimFloat(*Animation, Mover->AnimCrouch, Mover->bCrouched ? 1.0f : 0.0f);
            if(Mover->JumpCount != Mover->SeenJumpCount && Mover->AnimJump[0] != '\0') { Animation->SetTrigger(Mover->AnimJump); }
        }
        Mover->SeenJumpCount = Mover->JumpCount;

        // --- The camera: the boom turned to the view, the camera kept out of walls ------
        const GameCore::EntityID BoomId = FindChild(InWorld, Id, "CameraBoom");
        if(!BoomId.IsSet()) { continue; }
        const double BoomHeight = BoomHeightOf(*Mover);
        if(GameCore::TransformComponent* Boom = InWorld.GetEntity(BoomId).GetComponent<GameCore::TransformComponent>())
        {
            Boom->Position = EngineCore::Vector3(0.0, BoomHeight, 0.0);
            Boom->Rotation = EngineCore::Vector3(double(Mover->Pitch), WrapDegrees(double(Mover->CameraYaw) - double(Mover->BodyYaw) - double(Mover->MeshYaw)),
                                                 0.0);
        }
        const GameCore::EntityID CameraId = FindChild(InWorld, BoomId, "Camera");
        GameCore::TransformComponent* Camera = CameraId.IsSet() ? InWorld.GetEntity(CameraId).GetComponent<GameCore::TransformComponent>() : nullptr;
        if(!Camera) { continue; }

        Vec Fwd, RightV, UpV;
        ViewAxes(Mover->Pitch, Mover->CameraYaw, Fwd, RightV, UpV);
        double Side, Up, Back;
        CameraOffset(*Mover, Side, Up, Back);
        const Vec Pivot { Body->Position.GetX(), Body->Position.GetY() + BoomHeight, Body->Position.GetZ() };
        const Vec Off { RightV.X * Side + UpV.X * Up - Fwd.X * Back,
                        RightV.Y * Side + UpV.Y * Up - Fwd.Y * Back,
                        RightV.Z * Side + UpV.Z * Up - Fwd.Z * Back };
        const double Length = std::sqrt(Off.X * Off.X + Off.Y * Off.Y + Off.Z * Off.Z);
        double Scale = 1.0;
        /* Not for another process's player: its avatar's camera is never looked through here. */
        const AvatarHarnessComponent* Harness = Avatar.GetComponent<AvatarHarnessComponent>();
        const bool bRemote = Harness && Harness->IsPossessed() && !Harness->IsLocallyControlled();
        if(Length > 1e-4 && !bRemote)
        {
            const BodyShape Shape = ShapeOf(*Mover, Mover->bCrouched != 0);
            PhysicsRaycastHit Hit;
            if(Ray(InWorld, Shape, Pivot, { Off.X / Length, Off.Y / Length, Off.Z / Length }, Length + 15.0, Hit))
            {
                Scale = std::clamp((double(Hit.Distance) - 15.0) / Length, 0.1, 1.0);
            }
        }
        Camera->Position = EngineCore::Vector3(Side * Scale, Up * Scale, -Back * Scale);
        Camera->Rotation = EngineCore::Vector3(0.0, 0.0, 0.0);
        if(RenderCore::CameraComponent* Lens = InWorld.GetEntity(CameraId).GetComponent<RenderCore::CameraComponent>())
        {
            Lens->FovY = float(Lerp(Mover->FovNormal, Mover->FovScope, Ease(std::clamp(double(Mover->Scope), 0.0, 1.0))));
        }
        Mover->CameraX = float(Pivot.X + Off.X * Scale);
        Mover->CameraY = float(Pivot.Y + Off.Y * Scale);
        Mover->CameraZ = float(Pivot.Z + Off.Z * Scale);
        Mover->bCameraPlaced = 1;
    }
}

}   // namespace NimbleEngine

/* What an AvatarHarness names in Simulation. */
NIMBLE_DEFINE_AVATAR_SIMULATION(CharacterMove, 0.1f)
{
    NimbleEngine::StepCharacter(InWorld, InAvatar, InFrame);
}

// === The component's tables ===========================================================

namespace NimbleEngine
{

using NimbleEngine::CharacterMoverComponent;

GAMECORE_DEFINE_COMPONENT(CharacterMoverComponent, "Character Mover")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(CharacterMoverComponent, 1)
{
    Ar.Field("WalkSpeed",    C.WalkSpeed);
    Ar.Field("RunSpeed",     C.RunSpeed);
    Ar.Field("CrouchSpeed",  C.CrouchSpeed);
    Ar.Field("AimSpeed",     C.AimSpeed);
    Ar.Field("Accel",        C.Accel);
    Ar.Field("Decel",        C.Decel);
    Ar.Field("AirControl",   C.AirControl);
    Ar.Field("JumpSpeed",    C.JumpSpeed);
    Ar.Field("Gravity",      C.Gravity);
    Ar.Field("TurnRate",     C.TurnRate);
    Ar.Field("PitchMin",     C.PitchMin);
    Ar.Field("PitchMax",     C.PitchMax);
    Ar.Field("ScopeLook",    C.ScopeLook);
    Ar.Field("ScopeRate",    C.ScopeRate);
    Ar.Field("MeshYaw",      C.MeshYaw);
    Ar.Flag("FaceView",      C.bFaceView);
    Ar.Field("Radius",       C.Radius);
    Ar.Field("Height",       C.Height);
    Ar.Field("CrouchHeight", C.CrouchHeight);
    Ar.Field("StepHeight",   C.StepHeight);
    Ar.Field("SnapDown",     C.SnapDown);
    Ar.Field("MaxSlope",     C.MaxSlope);
    Ar.Field("Mask",         C.Mask);
    Ar.Field("BoomHeight",   C.BoomHeight);
    Ar.Field("CameraBack",   C.CameraBack);
    Ar.Field("CameraRight",  C.CameraRight);
    Ar.Field("CameraUp",     C.CameraUp);
    Ar.Field("ScopeBack",    C.ScopeBack);
    Ar.Field("ScopeRight",   C.ScopeRight);
    Ar.Field("ScopeUp",      C.ScopeUp);
    Ar.Field("FovNormal",    C.FovNormal);
    Ar.Field("FovScope",     C.FovScope);
    Ar.Field("MoveForward",  C.MoveForward);
    Ar.Field("MoveRight",    C.MoveRight);
    Ar.Field("LookX",        C.LookX);
    Ar.Field("LookY",        C.LookY);
    Ar.Field("RunAction",    C.RunAction);
    Ar.Field("JumpAction",   C.JumpAction);
    Ar.Field("CrouchAction", C.CrouchAction);
    Ar.Field("AimAction",    C.AimAction);
    Ar.Field("AnimSpeed",    C.AnimSpeed);
    Ar.Field("AnimGrounded", C.AnimGrounded);
    Ar.Field("AnimAimPitch", C.AnimAimPitch);
    Ar.Field("AnimJump",     C.AnimJump);
    Ar.Field("AnimCrouch",   C.AnimCrouch);
    Ar.Field("BodyYaw",      C.BodyYaw);
    Ar.Field("CameraYaw",    C.CameraYaw);
    Ar.Field("Pitch",        C.Pitch);
    Ar.Field("Scope",        C.Scope);
    Ar.Field("VelX",         C.VelX);
    Ar.Field("VelY",         C.VelY);
    Ar.Field("VelZ",         C.VelZ);
    Ar.Field("AirTime",      C.AirTime);
    Ar.Field("JumpCount",    C.JumpCount);
    Ar.Flag("Grounded",      C.bGrounded);
    Ar.Flag("Crouched",      C.bCrouched);
    Ar.Flag("Started",       C.bStarted);
}

/* Quantized (ComponentReplication.h); the avatar system snaps them after
 * every step on both ends, so prediction stays exact. The view and the
 * velocity are Smooth() for everyone else: another player's spine pitch and
 * stride (the anim params follow them) blend between updates as its
 * Transform does. The tunables come from the prefab and are not sent. */
GAMECORE_DEFINE_COMPONENT_REPLICATION(CharacterMoverComponent)
{
    Rep.Field("BodyYaw",   &CharacterMoverComponent::BodyYaw).Angles().Smooth();
    Rep.Field("CameraYaw", &CharacterMoverComponent::CameraYaw).Angles().Smooth();
    Rep.Field("Pitch",     &CharacterMoverComponent::Pitch).Precision(0.01f).Smooth();
    Rep.Field("Scope",     &CharacterMoverComponent::Scope).Precision(0.001f);
    Rep.Field("VelX",      &CharacterMoverComponent::VelX).Precision(0.01f).Smooth();
    Rep.Field("VelY",      &CharacterMoverComponent::VelY).Precision(0.01f);
    Rep.Field("VelZ",      &CharacterMoverComponent::VelZ).Precision(0.01f).Smooth();
    Rep.Field("AirTime",   &CharacterMoverComponent::AirTime).Precision(0.001f);
    Rep.Field("JumpCount", &CharacterMoverComponent::JumpCount);
    Rep.Flag("Grounded",   &CharacterMoverComponent::bGrounded);
    Rep.Flag("Crouched",   &CharacterMoverComponent::bCrouched);
    Rep.Flag("Started",    &CharacterMoverComponent::bStarted);
}

/* mover:Teleport(x, y, z, yaw), mover:GetAim() -> ox, oy, oz, dx, dy, dz,
 * mover:GetSpeed(). The common tunables read and write; the state reads. */
GAMECORE_DEFINE_COMPONENT_SCRIPT(CharacterMoverComponent)
{
    Bind.Field("WalkSpeed",   &CharacterMoverComponent::WalkSpeed);
    Bind.Field("RunSpeed",    &CharacterMoverComponent::RunSpeed);
    Bind.Field("CrouchSpeed", &CharacterMoverComponent::CrouchSpeed);
    Bind.Field("AimSpeed",    &CharacterMoverComponent::AimSpeed);
    Bind.Field("AirControl",  &CharacterMoverComponent::AirControl);
    Bind.Field("JumpSpeed",   &CharacterMoverComponent::JumpSpeed);
    Bind.Field("Gravity",     &CharacterMoverComponent::Gravity);
    Bind.Field("TurnRate",    &CharacterMoverComponent::TurnRate);
    Bind.Flag("FaceView",     &CharacterMoverComponent::bFaceView);
    Bind.ReadOnly("BodyYaw",   &CharacterMoverComponent::BodyYaw);
    Bind.ReadOnly("CameraYaw", &CharacterMoverComponent::CameraYaw);
    Bind.ReadOnly("Pitch",     &CharacterMoverComponent::Pitch);
    Bind.ReadOnly("Scope",     &CharacterMoverComponent::Scope);
    Bind.ReadOnly("AirTime",   &CharacterMoverComponent::AirTime);
    Bind.ReadOnly("JumpCount", &CharacterMoverComponent::JumpCount);
    Bind.Flag("Grounded",      &CharacterMoverComponent::bGrounded, true);
    Bind.Flag("Crouched",      &CharacterMoverComponent::bCrouched, true);

    Bind.Method("Teleport", [](CharacterMoverComponent&, GameCore::ScriptCall& Call)
    {
        const EngineCore::Vector3 Position = Call.GetVector3(1);
        const float Yaw = Call.HasArg(4) ? Call.GetFloat(4) : 0.0f;
        if(Call.HasError()) { return; }
        TeleportCharacter(Call.GetWorld(), Call.GetOwner(), Position, Yaw);
    }, "Position:Vector3, Yaw:number");
    Bind.Method("GetAim", [](CharacterMoverComponent&, GameCore::ScriptCall& Call)
    {
        EngineCore::Vector3 Origin, Dir;
        GetCharacterAim(Call.GetWorld(), Call.GetOwner(), Origin, Dir);
        Call.PushVector3(Origin);
        Call.PushVector3(Dir);
    }, "-> Origin:Vector3, Dir:Vector3");
    Bind.Method("GetSpeed", [](CharacterMoverComponent& C, GameCore::ScriptCall& Call)
    {
        Call.PushNumber(std::hypot(double(C.VelX), double(C.VelZ)));
    }, "-> Speed:number");
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(CharacterMoverComponent)
{
    Props.Category("Movement");
    Props.Field("Walk Speed",   &CharacterMoverComponent::WalkSpeed);
    Props.Field("Run Speed",    &CharacterMoverComponent::RunSpeed);
    Props.Field("Crouch Speed", &CharacterMoverComponent::CrouchSpeed);
    Props.Field("Aim Speed",    &CharacterMoverComponent::AimSpeed);
    Props.Field("Accel",        &CharacterMoverComponent::Accel);
    Props.Field("Decel",        &CharacterMoverComponent::Decel);
    Props.Field("Air Control",  &CharacterMoverComponent::AirControl);
    Props.Category("Jumping");
    Props.Field("Jump Speed",   &CharacterMoverComponent::JumpSpeed);
    Props.Field("Gravity",      &CharacterMoverComponent::Gravity);
    Props.Category("View");
    Props.Field("Turn Rate",    &CharacterMoverComponent::TurnRate);
    Props.Field("Pitch Min",    &CharacterMoverComponent::PitchMin);
    Props.Field("Pitch Max",    &CharacterMoverComponent::PitchMax);
    Props.Field("Aim Look",     &CharacterMoverComponent::ScopeLook);
    Props.Field("Aim Rate",     &CharacterMoverComponent::ScopeRate);
    Props.Field("Mesh Yaw",     &CharacterMoverComponent::MeshYaw);
    Props.Flag("Face View",     &CharacterMoverComponent::bFaceView);
    Props.Category("Body");
    Props.Field("Radius",        &CharacterMoverComponent::Radius);
    Props.Field("Height",        &CharacterMoverComponent::Height);
    Props.Field("Crouch Height", &CharacterMoverComponent::CrouchHeight);
    Props.Field("Step Height",   &CharacterMoverComponent::StepHeight);
    Props.Field("Snap Down",     &CharacterMoverComponent::SnapDown);
    Props.Field("Max Slope",     &CharacterMoverComponent::MaxSlope);
    Props.Field("Mask",          &CharacterMoverComponent::Mask);
    Props.Category("Camera");
    Props.Field("Boom Height",   &CharacterMoverComponent::BoomHeight);
    Props.Field("Camera Back",   &CharacterMoverComponent::CameraBack);
    Props.Field("Camera Right",  &CharacterMoverComponent::CameraRight);
    Props.Field("Camera Up",     &CharacterMoverComponent::CameraUp);
    Props.Field("Aim Back",      &CharacterMoverComponent::ScopeBack);
    Props.Field("Aim Right",     &CharacterMoverComponent::ScopeRight);
    Props.Field("Aim Up",        &CharacterMoverComponent::ScopeUp);
    Props.Field("Fov",           &CharacterMoverComponent::FovNormal);
    Props.Field("Aim Fov",       &CharacterMoverComponent::FovScope);
    Props.Category("Input");
    Props.Field("Move Forward",  &CharacterMoverComponent::MoveForward);
    Props.Field("Move Right",    &CharacterMoverComponent::MoveRight);
    Props.Field("Look X",        &CharacterMoverComponent::LookX);
    Props.Field("Look Y",        &CharacterMoverComponent::LookY);
    Props.Field("Run",           &CharacterMoverComponent::RunAction);
    Props.Field("Jump",          &CharacterMoverComponent::JumpAction);
    Props.Field("Crouch",        &CharacterMoverComponent::CrouchAction);
    Props.Field("Aim",           &CharacterMoverComponent::AimAction);
    Props.Category("Animation");
    Props.Field("Speed Param",     &CharacterMoverComponent::AnimSpeed);
    Props.Field("Grounded Param",  &CharacterMoverComponent::AnimGrounded);
    Props.Field("Aim Pitch Param", &CharacterMoverComponent::AnimAimPitch);
    Props.Field("Jump Trigger",    &CharacterMoverComponent::AnimJump);
    Props.Field("Crouch Param",    &CharacterMoverComponent::AnimCrouch);
    Props.Category("State");
    Props.Flag("Grounded", &CharacterMoverComponent::bGrounded, GameCore::PF_ReadOnly);
    Props.Flag("Crouched", &CharacterMoverComponent::bCrouched, GameCore::PF_ReadOnly);
}
#endif

}   // namespace NimbleEngine
