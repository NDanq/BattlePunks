#include "Public/GameCore/WorldSettings.h"

#include <algorithm>

#include "Public/Core/CoreArchive.h"

namespace GameCore
{
    void WorldLightingSettings::Clamp()
    {
        TexelsPerUnit   = std::clamp(TexelsPerUnit, 0.001f, 4.0f);
        Bounces         = std::clamp(Bounces, 0, 8);
        SamplesPerTexel = std::clamp(SamplesPerTexel, 1, 4096);
        ProbeSpacing    = std::clamp(ProbeSpacing, 10.0f, 10000.0f);

        /* A power of two, 128-4096. */
        int32_t Size = 128;
        while(Size < MaxAtlasSize && Size < 4096) { Size *= 2; }
        MaxAtlasSize = Size;
    }

    bool WorldLightingSettings::operator==(const WorldLightingSettings& InOther) const
    {
        return bBakedLighting == InOther.bBakedLighting && TexelsPerUnit == InOther.TexelsPerUnit
            && Bounces == InOther.Bounces && SamplesPerTexel == InOther.SamplesPerTexel
            && ProbeSpacing == InOther.ProbeSpacing && MaxAtlasSize == InOther.MaxAtlasSize
            && BakedData == InOther.BakedData;
    }

    void SerializeWorldSettings(EngineCore::Archive& Ar, WorldSettings& InOutSettings, const uint16_t InVersion)
    {
        WorldLightingSettings& Lighting = InOutSettings.Lighting;
        uint8_t Baked = Lighting.bBakedLighting ? 1 : 0;
        Ar << Baked << Lighting.TexelsPerUnit << Lighting.Bounces << Lighting.SamplesPerTexel
           << Lighting.ProbeSpacing << Lighting.MaxAtlasSize << Lighting.BakedData;
        if(InVersion >= 2)
        {
            Ar << InOutSettings.GameMode;
        }
        else if(Ar.IsLoading())
        {
            InOutSettings.GameMode.clear();
        }
        if(Ar.IsLoading())
        {
            Lighting.bBakedLighting = Baked != 0;
            Lighting.Clamp();
        }
    }
}
