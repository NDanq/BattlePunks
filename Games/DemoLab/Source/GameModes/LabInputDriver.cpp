#include "GameModes/LabInputDriver.h"

#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/Input.h"

#include <algorithm>
#include <cmath>

using namespace GameCore;
using namespace NimbleEngine;
using EngineCore::Vector3;

namespace
{
    constexpr double RAD = 3.14159265358979 / 180.0;

    double Wrap(double InDegrees)
    {
        while(InDegrees > 180.0) { InDegrees -= 360.0; }
        while(InDegrees < -180.0) { InDegrees += 360.0; }
        return InDegrees;
    }
}

void LabDrive::SetAxis(InputSnapshot& InOut, const char* InAxis, const float InValue)
{
    const int Index = FindInputAxis(MAP, InAxis);
    if(Index >= 0 && Index < int(INPUT_AXES_MAX)) { InOut.Axes[Index] = InValue; }
}

void LabDrive::Press(InputSnapshot& InOut, const char* InAction, const bool bInHold)
{
    const int Index = FindInputAction(MAP, InAction);
    if(Index < 0) { return; }
    InOut.Down |= 1u << Index;
    if(!bInHold) { InOut.Pressed |= 1u << Index; }
}

float LabDrive::AimAt(World& InWorld, const EntityID InAvatar, InputSnapshot& InOut, const Vector3& InTarget, const float InMaxDegrees)
{
    const CharacterMoverComponent* Mover = InWorld.GetEntity(InAvatar).GetComponent<CharacterMoverComponent>();
    Vector3 Origin, Dir;
    if(!Mover || !GetCharacterAim(InWorld, InAvatar, Origin, Dir)) { return 180.f; }
    const Vector3 To = InTarget - Origin;
    const double Length = To.GetLength();
    if(Length < 1.0) { return 0.f; }
    const double WantYaw = std::atan2(To.GetX(), To.GetZ()) / RAD;
    const double WantPitch = -std::asin(std::clamp(To.GetY() / Length, -1.0, 1.0)) / RAD;
    const double Most = double(InMaxDegrees);
    SetAxis(InOut, "LookX", float(std::clamp(Wrap(WantYaw - double(Mover->CameraYaw)) * 0.35, -Most, Most)));
    SetAxis(InOut, "LookY", float(std::clamp((WantPitch - double(Mover->Pitch)) * 0.35, -Most, Most)));
    const double Cos = std::clamp((To * (1.0 / Length)).Dot(Dir), -1.0, 1.0);
    return float(std::acos(Cos) / RAD);
}

bool LabDrive::WalkTo(World& InWorld, const EntityID InAvatar, InputSnapshot& InOut, const Vector3& InTarget, const float InStop)
{
    const CharacterMoverComponent* Mover = InWorld.GetEntity(InAvatar).GetComponent<CharacterMoverComponent>();
    if(!Mover) { return false; }
    const Vector3 Feet = InWorld.GetEntity(InAvatar).GetWorldPose().Position;
    const double Dx = InTarget.GetX() - Feet.GetX(), Dz = InTarget.GetZ() - Feet.GetZ();
    const double Distance = std::sqrt(Dx * Dx + Dz * Dz);
    const double Error = Wrap(std::atan2(Dx, Dz) / RAD - double(Mover->CameraYaw));
    SetAxis(InOut, "LookX", float(std::clamp(Error * 0.3, -6.0, 6.0)));
    SetAxis(InOut, "LookY", float(std::clamp((8.0 - double(Mover->Pitch)) * 0.2, -3.0, 3.0)));
    if(Distance <= double(InStop)) { return true; }
    if(std::fabs(Error) < 35.0)
    {
        SetAxis(InOut, "MoveForward", 1.f);
        if(Distance > 500.0) { Press(InOut, "Run", true); }
    }
    return false;
}

Vector3 LabDrive::ChestOf(World& InWorld, const EntityID InLab)
{
    return InWorld.GetEntity(InLab).GetWorldPose().Position + Vector3(0, 115, 0);
}
