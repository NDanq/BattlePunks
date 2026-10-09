#include "Public/RenderCore/PostProcessStack.h"

#include <algorithm>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "Public/Core/CoreFileProvider.h"
#include "Public/GameCore/GraphText.h"
#include "Public/RenderCore/RenderCoreLog.h"

/* PostProcessStack.h: reading, blending and caching post process stacks. */

namespace RenderCore
{
    namespace
    {
        bool EndsWith(const std::string& InText, const char* InSuffix)
        {
            const size_t Length = std::strlen(InSuffix);
            return InText.size() >= Length && InText.compare(InText.size() - Length, Length, InSuffix) == 0;
        }

        bool ToFloat(const std::string& InText, float& OutValue)
        {
            char* End = nullptr;
            const double Value = std::strtod(InText.c_str(), &End);
            if(InText.empty() || End != InText.c_str() + InText.size())
            {
                return false;
            }
            OutValue = float(Value);
            return true;
        }

        /* "1,0.5,0.25" into up to InCount floats; one number fills them all. */
        bool ToFloats(const std::string& InText, float* OutValues, const size_t InCount)
        {
            std::vector<float> Values;
            size_t Start = 0;
            while(Start <= InText.size())
            {
                const size_t Comma = InText.find(',', Start);
                float Value = 0.0f;
                if(!ToFloat(InText.substr(Start, Comma == std::string::npos ? std::string::npos : Comma - Start), Value))
                {
                    return false;
                }
                Values.push_back(Value);
                if(Comma == std::string::npos) { break; }
                Start = Comma + 1;
            }
            if(Values.size() != 1 && Values.size() != InCount)
            {
                return false;
            }
            for(size_t Index = 0; Index < InCount; ++Index)
            {
                OutValues[Index] = Values.size() == 1 ? Values[0] : Values[Index];
            }
            return true;
        }

        float Lerp(const float InA, const float InB, const float InT) { return InA + (InB - InA) * InT; }

        void AddMaterial(PostProcessSettings& InOut, const char* InPath, const uint8_t InStage, const float InOrder)
        {
            if(InOut.MaterialCount >= RENDER_POST_MATERIAL_MAX)
            {
                return;
            }
            PostProcessMaterial& Added = InOut.Materials[InOut.MaterialCount++];
            std::snprintf(Added.Path, sizeof(Added.Path), "%s", InPath);
            Added.Stage = InStage;
            Added.Order = InOrder;
        }
    }

    bool ParsePostProcessStack(const std::string& InText, PostProcessSettings& OutSettings, std::string& OutError)
    {
        OutSettings = PostProcessSettings();
        std::istringstream Lines(InText);
        std::string Line;
        int Number = 0;
        while(std::getline(Lines, Line))
        {
            ++Number;
            if(!Line.empty() && Line.back() == '\r') { Line.pop_back(); }
            if(Line.empty() || Line[0] == '#')
            {
                continue;
            }
            const auto Fail = [&](const std::string& InWhy)
            {
                OutError = "line " + std::to_string(Number) + ": " + InWhy;
                return false;
            };

            std::istringstream Words(Line);
            std::string Kind;
            Words >> Kind;
            if(Kind == "Material")
            {
                /* Stage=.. Order=.. Path=<the rest of the line>. */
                const size_t PathAt = Line.find(" Path=");
                if(PathAt == std::string::npos)
                {
                    return Fail("a Material without a Path");
                }
                const std::string Path = Line.substr(PathAt + 6);
                std::istringstream Settings(Line.substr(0, PathAt));
                std::string Word;
                Settings >> Word;   // "Material"
                uint8_t Stage = PPS_AfterEffects;
                float   Order = 0.0f;
                while(Settings >> Word)
                {
                    if(Word == "Stage=Before")      { Stage = PPS_BeforeEffects; }
                    else if(Word == "Stage=After")  { Stage = PPS_AfterEffects; }
                    else if(Word.rfind("Order=", 0) == 0 && !ToFloat(Word.substr(6), Order)) { return Fail("bad Order '" + Word + "'"); }
                }
                if(Path.empty() || Path.size() >= RENDER_POST_PATH_MAX)
                {
                    return Fail("a Material path that is empty or too long");
                }
                AddMaterial(OutSettings, Path.c_str(), Stage, Order);
                continue;
            }
            if(Kind != "Effect")
            {
                continue;   // a newer kind of line
            }

            std::string Effect;
            Words >> Effect;
            PostProcessSettings& S = OutSettings;
            std::string Pair;
            while(Words >> Pair)
            {
                const size_t Equals = Pair.find('=');
                if(Equals == std::string::npos)
                {
                    return Fail("'" + Pair + "' is not Key=Value");
                }
                const std::string Key   = Pair.substr(0, Equals);
                const std::string Value = Pair.substr(Equals + 1);
                float* Target = nullptr;
                if(Effect == "AmbientOcclusion")
                {
                    if(Key == "Intensity")    { Target = &S.AoIntensity; }
                    else if(Key == "Radius")  { Target = &S.AoRadius; }
                    else if(Key == "Samples") { Target = &S.AoSamples; }
                    else if(Key == "Power")   { Target = &S.AoPower; }
                    else if(Key == "Blur")    { S.bAoBlur = Value == "true" ? 1 : 0; }
                }
                else if(Effect == "VolumetricFog")
                {
                    if(Key == "Density")              { Target = &S.FogDensity; }
                    else if(Key == "HeightFalloff")   { Target = &S.FogHeightFalloff; }
                    else if(Key == "BaseHeight")      { Target = &S.FogBaseHeight; }
                    else if(Key == "MaxDistance")     { Target = &S.FogMaxDistance; }
                    else if(Key == "Anisotropy")      { Target = &S.FogAnisotropy; }
                    else if(Key == "SunScattering")   { Target = &S.FogSun; }
                    else if(Key == "SkyScattering")   { Target = &S.FogSky; }
                    else if(Key == "LightScattering") { Target = &S.FogLights; }
                    else if(Key == "Steps")           { Target = &S.FogSteps; }
                    else if(Key == "Shadows")         { S.bFogShadows = Value == "true" ? 1 : 0; }
                    else if(Key == "Albedo" && !ToFloats(Value, S.FogAlbedo, 3)) { return Fail("bad Albedo '" + Value + "'"); }
                }
                else if(Effect == "ScreenSpaceReflections")
                {
                    if(Key == "Intensity")         { Target = &S.SsrIntensity; }
                    else if(Key == "MaxRoughness") { Target = &S.SsrMaxRoughness; }
                    else if(Key == "MaxDistance")  { Target = &S.SsrMaxDistance; }
                    else if(Key == "Thickness")    { Target = &S.SsrThickness; }
                    else if(Key == "Steps")        { Target = &S.SsrSteps; }
                }
                else if(Effect == "DepthOfField")
                {
                    if(Key == "FocusDistance")  { Target = &S.DofFocusDistance; }
                    else if(Key == "FocusRange") { Target = &S.DofFocusRange; }
                    else if(Key == "MaxBlur")    { Target = &S.DofMaxBlur; }
                    else if(Key == "NearBlur")   { S.bDofNearBlur = Value == "true" ? 1 : 0; }
                }
                else if(Effect == "MotionBlur")
                {
                    if(Key == "Intensity")      { Target = &S.MotionIntensity; }
                    else if(Key == "MaxLength") { Target = &S.MotionMaxLength; }
                    else if(Key == "Samples")   { Target = &S.MotionSamples; }
                }
                else if(Effect == "Bloom")
                {
                    if(Key == "Threshold")      { Target = &S.BloomThreshold; }
                    else if(Key == "Intensity") { Target = &S.BloomIntensity; }
                    else if(Key == "Radius")    { Target = &S.BloomRadius; }
                    else if(Key == "Levels")    { Target = &S.BloomLevels; }
                    else if(Key == "Tint" && !ToFloats(Value, S.BloomTint, 3)) { return Fail("bad Tint '" + Value + "'"); }
                }
                else if(Effect == "ToneMapping")
                {
                    if(Key == "Exposure")        { Target = &S.Exposure; }
                    else if(Key == "ToneMapper") { S.ToneMapper = Value == "None" ? 0 : Value == "Reinhard" ? 1 : 2; }
                }
                else if(Effect == "AutoExposure")
                {
                    if(Key == "Key")               { Target = &S.AeKey; }
                    else if(Key == "MinEV")        { Target = &S.AeMinEV; }
                    else if(Key == "MaxEV")        { Target = &S.AeMaxEV; }
                    else if(Key == "SpeedUp")      { Target = &S.AeSpeedUp; }
                    else if(Key == "SpeedDown")    { Target = &S.AeSpeedDown; }
                    else if(Key == "CentreWeight") { Target = &S.AeCentreWeight; }
                }
                else if(Effect == "Antialiasing")
                {
                    if(Key == "EdgeThreshold") { Target = &S.AaEdgeThreshold; }
                    else if(Key == "Quality")  { S.AaQuality = Value == "Low" ? 0 : Value == "High" ? 2 : 1; }
                    else if(Key == "Method")   { S.AaMethod  = Value == "TAA" ? 1 : 0; }
                    else if(Key == "Sharpness") { Target = &S.AaSharpness; }
                }
                if(Target && !ToFloat(Value, *Target))
                {
                    return Fail("bad number '" + Pair + "'");
                }
            }
            if(Effect == "AmbientOcclusion")  { S.bAmbientOcclusion = 1; }
            else if(Effect == "DepthOfField") { S.bDepthOfField = 1; }
            else if(Effect == "MotionBlur")   { S.bMotionBlur   = 1; }
            else if(Effect == "Bloom")        { S.bBloom        = 1; }
            else if(Effect == "Antialiasing") { S.bAntialiasing = 1; }
            else if(Effect == "ToneMapping")  { S.bToneMapping  = 1; }
            else if(Effect == "AutoExposure") { S.bAutoExposure = 1; }
            else if(Effect == "ScreenSpaceReflections") { S.bScreenReflections = 1; }
            else if(Effect == "VolumetricFog") { S.bVolumetricFog = 1; }
        }
        return true;
    }

    float GetPostProcessVolumeWeight(const float InDistance, const float InBlendDistance)
    {
        if(InDistance <= 0.0f)
        {
            return 1.0f;
        }
        if(InBlendDistance <= 0.0f)
        {
            return 0.0f;
        }
        return std::clamp(1.0f - InDistance / InBlendDistance, 0.0f, 1.0f);
    }

    void BlendPostProcess(PostProcessSettings& InOutBlend, const PostProcessSettings& InStack, const float InWeight)
    {
        const float W = std::clamp(InWeight, 0.0f, 1.0f);
        if(W <= 0.0f)
        {
            return;
        }
        const bool bSwitch = W >= 0.5f;
        PostProcessSettings& B = InOutBlend;

        if(InStack.bAmbientOcclusion)
        {
            if(!B.bAmbientOcclusion)
            {
                B.AoIntensity = 0.0f;
                B.AoRadius    = InStack.AoRadius;
                B.AoSamples   = InStack.AoSamples;
                B.AoPower     = InStack.AoPower;
                B.bAoBlur     = InStack.bAoBlur;
            }
            B.bAmbientOcclusion = 1;
            B.AoIntensity = Lerp(B.AoIntensity, InStack.AoIntensity, W);
            B.AoRadius    = Lerp(B.AoRadius, InStack.AoRadius, W);
            B.AoSamples   = Lerp(B.AoSamples, InStack.AoSamples, W);
            B.AoPower     = Lerp(B.AoPower, InStack.AoPower, W);
            if(bSwitch) { B.bAoBlur = InStack.bAoBlur; }
        }
        if(InStack.bToneMapping)
        {
            if(!B.bToneMapping)
            {
                /* From off: no exposure, and the operator once the volume is half in. */
                B.Exposure   = 0.0f;
                B.ToneMapper = 0;
            }
            B.bToneMapping = 1;
            B.Exposure     = Lerp(B.Exposure, InStack.Exposure, W);
            if(bSwitch) { B.ToneMapper = InStack.ToneMapper; }
        }
        if(InStack.bVolumetricFog)
        {
            if(!B.bVolumetricFog)
            {
                /* From off: the stack's look, no density yet -- it thickens as the volume comes in. */
                B.FogDensity       = 0.0f;
                B.FogHeightFalloff = InStack.FogHeightFalloff;
                B.FogBaseHeight    = InStack.FogBaseHeight;
                B.FogMaxDistance   = InStack.FogMaxDistance;
                B.FogAnisotropy    = InStack.FogAnisotropy;
                B.FogSun           = InStack.FogSun;
                B.FogSky           = InStack.FogSky;
                B.FogLights        = InStack.FogLights;
                std::copy(std::begin(InStack.FogAlbedo), std::end(InStack.FogAlbedo), B.FogAlbedo);
                B.FogSteps         = InStack.FogSteps;
                B.bFogShadows      = InStack.bFogShadows;
            }
            B.bVolumetricFog   = 1;
            B.FogDensity       = Lerp(B.FogDensity, InStack.FogDensity, W);
            B.FogHeightFalloff = Lerp(B.FogHeightFalloff, InStack.FogHeightFalloff, W);
            B.FogBaseHeight    = Lerp(B.FogBaseHeight, InStack.FogBaseHeight, W);
            B.FogMaxDistance   = Lerp(B.FogMaxDistance, InStack.FogMaxDistance, W);
            B.FogAnisotropy    = Lerp(B.FogAnisotropy, InStack.FogAnisotropy, W);
            B.FogSun           = Lerp(B.FogSun, InStack.FogSun, W);
            B.FogSky           = Lerp(B.FogSky, InStack.FogSky, W);
            B.FogLights        = Lerp(B.FogLights, InStack.FogLights, W);
            for(int C = 0; C < 3; ++C) { B.FogAlbedo[C] = Lerp(B.FogAlbedo[C], InStack.FogAlbedo[C], W); }
            B.FogSteps         = Lerp(B.FogSteps, InStack.FogSteps, W);
            if(bSwitch) { B.bFogShadows = InStack.bFogShadows; }
        }
        if(InStack.bScreenReflections)
        {
            if(!B.bScreenReflections)
            {
                /* From off: the stack's reach and quality, no strength yet. */
                B.SsrIntensity    = 0.0f;
                B.SsrMaxRoughness = InStack.SsrMaxRoughness;
                B.SsrMaxDistance  = InStack.SsrMaxDistance;
                B.SsrThickness    = InStack.SsrThickness;
                B.SsrSteps        = InStack.SsrSteps;
            }
            B.bScreenReflections = 1;
            B.SsrIntensity    = Lerp(B.SsrIntensity, InStack.SsrIntensity, W);
            B.SsrMaxRoughness = Lerp(B.SsrMaxRoughness, InStack.SsrMaxRoughness, W);
            B.SsrMaxDistance  = Lerp(B.SsrMaxDistance, InStack.SsrMaxDistance, W);
            B.SsrThickness    = Lerp(B.SsrThickness, InStack.SsrThickness, W);
            B.SsrSteps        = Lerp(B.SsrSteps, InStack.SsrSteps, W);
        }
        if(InStack.bAutoExposure)
        {
            if(!B.bAutoExposure)
            {
                /* From off: no correction either way, widening to the stack's range as the volume comes in. */
                B.AeKey          = InStack.AeKey;
                B.AeMinEV        = 0.0f;
                B.AeMaxEV        = 0.0f;
                B.AeSpeedUp      = InStack.AeSpeedUp;
                B.AeSpeedDown    = InStack.AeSpeedDown;
                B.AeCentreWeight = InStack.AeCentreWeight;
            }
            B.bAutoExposure  = 1;
            B.AeKey          = Lerp(B.AeKey, InStack.AeKey, W);
            B.AeMinEV        = Lerp(B.AeMinEV, InStack.AeMinEV, W);
            B.AeMaxEV        = Lerp(B.AeMaxEV, InStack.AeMaxEV, W);
            B.AeSpeedUp      = Lerp(B.AeSpeedUp, InStack.AeSpeedUp, W);
            B.AeSpeedDown    = Lerp(B.AeSpeedDown, InStack.AeSpeedDown, W);
            B.AeCentreWeight = Lerp(B.AeCentreWeight, InStack.AeCentreWeight, W);
        }
        if(InStack.bDepthOfField)
        {
            if(!B.bDepthOfField)
            {
                /* From off: the stack's settings, with no blur yet. */
                B.DofFocusDistance = InStack.DofFocusDistance;
                B.DofFocusRange    = InStack.DofFocusRange;
                B.DofMaxBlur       = 0.0f;
                B.bDofNearBlur     = InStack.bDofNearBlur;
            }
            B.bDepthOfField    = 1;
            B.DofFocusDistance = Lerp(B.DofFocusDistance, InStack.DofFocusDistance, W);
            B.DofFocusRange    = Lerp(B.DofFocusRange, InStack.DofFocusRange, W);
            B.DofMaxBlur       = Lerp(B.DofMaxBlur, InStack.DofMaxBlur, W);
            if(bSwitch) { B.bDofNearBlur = InStack.bDofNearBlur; }
        }
        if(InStack.bMotionBlur)
        {
            if(!B.bMotionBlur)
            {
                B.MotionIntensity = 0.0f;
                B.MotionMaxLength = InStack.MotionMaxLength;
                B.MotionSamples   = InStack.MotionSamples;
            }
            B.bMotionBlur     = 1;
            B.MotionIntensity = Lerp(B.MotionIntensity, InStack.MotionIntensity, W);
            B.MotionMaxLength = Lerp(B.MotionMaxLength, InStack.MotionMaxLength, W);
            B.MotionSamples   = Lerp(B.MotionSamples, InStack.MotionSamples, W);
        }
        if(InStack.bBloom)
        {
            if(!B.bBloom)
            {
                B.BloomThreshold = InStack.BloomThreshold;
                B.BloomIntensity = 0.0f;
                B.BloomRadius    = InStack.BloomRadius;
                B.BloomLevels    = InStack.BloomLevels;
                std::copy(InStack.BloomTint, InStack.BloomTint + 3, B.BloomTint);
            }
            B.bBloom         = 1;
            B.BloomThreshold = Lerp(B.BloomThreshold, InStack.BloomThreshold, W);
            B.BloomIntensity = Lerp(B.BloomIntensity, InStack.BloomIntensity, W);
            B.BloomRadius    = Lerp(B.BloomRadius, InStack.BloomRadius, W);
            B.BloomLevels    = Lerp(B.BloomLevels, InStack.BloomLevels, W);
            for(int Channel = 0; Channel < 3; ++Channel)
            {
                B.BloomTint[Channel] = Lerp(B.BloomTint[Channel], InStack.BloomTint[Channel], W);
            }
        }
        if(InStack.bAntialiasing)
        {
            /* Nothing to fade: on, at its settings, from half way in. */
            if(!B.bAntialiasing)
            {
                B.AaEdgeThreshold = InStack.AaEdgeThreshold;
                B.AaSharpness     = InStack.AaSharpness;
            }
            B.AaEdgeThreshold = Lerp(B.AaEdgeThreshold, InStack.AaEdgeThreshold, W);
            B.AaSharpness     = Lerp(B.AaSharpness, InStack.AaSharpness, W);
            if(bSwitch)
            {
                B.bAntialiasing = 1;
                B.AaQuality     = InStack.AaQuality;
                B.AaMethod      = InStack.AaMethod;
            }
        }
        if(bSwitch)
        {
            for(uint32_t Index = 0; Index < InStack.MaterialCount; ++Index)
            {
                const PostProcessMaterial& Each = InStack.Materials[Index];
                AddMaterial(B, Each.Path, Each.Stage, Each.Order);
            }
        }
    }

    namespace
    {
        struct StackCacheState
        {
            std::mutex Lock;
            std::unordered_map<std::string, std::shared_ptr<const PostProcessSettings>> Stacks;
        };

        StackCacheState& GetStackCache()
        {
            static StackCacheState State;
            return State;
        }

        /* Null (warned) if InPath cannot be read as a stack. */
        std::shared_ptr<const PostProcessSettings> LoadStack(const std::string& InPath)
        {
            auto Loaded = std::make_shared<PostProcessSettings>();
            if(EndsWith(InPath, ".material.nmgraph"))
            {
                if(InPath.size() >= RENDER_POST_PATH_MAX)
                {
                    RENDERCORE_LOG_WARN("post process: '" + InPath + "' is too long a path")
                    return nullptr;
                }
                AddMaterial(*Loaded, InPath.c_str(), PPS_AfterEffects, 0.0f);
                return Loaded;
            }

            std::string Text, Stack, Error;
            EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
            if(!Provider || !Provider->ReadAllText(InPath, Text))
            {
                RENDERCORE_LOG_WARN("post process stack '" + InPath + "' cannot be read")
                return nullptr;
            }
            if(!GameCore::ExtractGraphGeneratedCode(Text, ".ppstack", Stack))
            {
                RENDERCORE_LOG_WARN("post process stack '" + InPath + "' has no compiled stack -- save it in the editor")
                return nullptr;
            }
            if(!ParsePostProcessStack(Stack, *Loaded, Error))
            {
                RENDERCORE_LOG_WARN("post process stack '" + InPath + "': " + Error)
                return nullptr;
            }
            return Loaded;
        }
    }

    std::shared_ptr<const PostProcessSettings> PostProcessStackCache::Find(const std::string& InPath)
    {
        if(InPath.empty())
        {
            return nullptr;
        }
        StackCacheState& Cache = GetStackCache();
        {
            std::lock_guard<std::mutex> Guard(Cache.Lock);
            const auto Found = Cache.Stacks.find(InPath);
            if(Found != Cache.Stacks.end())
            {
                return Found->second;
            }
        }

        /* Loaded outside the lock; if two threads race, the first stored wins.
         * A failure is remembered too, so it warns once. */
        std::shared_ptr<const PostProcessSettings> Loaded = LoadStack(InPath);
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        return Cache.Stacks.emplace(InPath, Loaded).first->second;
    }

    void PostProcessStackCache::Invalidate(const std::string& InPath)
    {
        StackCacheState& Cache = GetStackCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        if(InPath.empty())
        {
            Cache.Stacks.clear();
            return;
        }
        Cache.Stacks.erase(InPath);
    }
}

// === Overrides (Sequencer's Post Process tracks) ===================================

namespace RenderCore
{
    namespace
    {
        const char* const PARAM_NAMES[] =
        {
            "Exposure", "DofFocusDistance", "DofFocusRange", "DofMaxBlur", "BloomIntensity", "BloomThreshold",
            "FogDensity", "MotionIntensity", "AoIntensity", "SsrIntensity",
        };
        static_assert(sizeof(PARAM_NAMES) / sizeof(PARAM_NAMES[0]) == size_t(EPostProcessParam::Count), "one name each");

        std::mutex& OverrideMutex()
        {
            static std::mutex Mutex;
            return Mutex;
        }

        std::unordered_map<const void*, PostProcessOverride>& Overrides()
        {
            static std::unordered_map<const void*, PostProcessOverride> Map;
            return Map;
        }
    }

    const char* PostProcessParamName(const EPostProcessParam InParam)
    {
        const size_t Index = size_t(InParam);
        return Index < size_t(EPostProcessParam::Count) ? PARAM_NAMES[Index] : "";
    }

    bool FindPostProcessParam(const char* InName, EPostProcessParam& OutParam)
    {
        if(!InName) { return false; }
        for(size_t Index = 0; Index < size_t(EPostProcessParam::Count); ++Index)
        {
            if(std::strcmp(InName, PARAM_NAMES[Index]) == 0)
            {
                OutParam = EPostProcessParam(Index);
                return true;
            }
        }
        return false;
    }

    void ApplyPostProcessParam(PostProcessSettings& S, const EPostProcessParam InParam, const float InValue)
    {
        switch(InParam)
        {
            case EPostProcessParam::Exposure:         S.bToneMapping = 1;       S.Exposure = InValue; break;
            case EPostProcessParam::DofFocusDistance: S.bDepthOfField = 1;      S.DofFocusDistance = std::max(0.0f, InValue); break;
            case EPostProcessParam::DofFocusRange:    S.bDepthOfField = 1;      S.DofFocusRange = std::max(0.0f, InValue); break;
            case EPostProcessParam::DofMaxBlur:       S.bDepthOfField = 1;      S.DofMaxBlur = std::max(0.0f, InValue); break;
            case EPostProcessParam::BloomIntensity:   S.bBloom = 1;             S.BloomIntensity = std::max(0.0f, InValue); break;
            case EPostProcessParam::BloomThreshold:   S.bBloom = 1;             S.BloomThreshold = InValue; break;
            case EPostProcessParam::FogDensity:       S.bVolumetricFog = 1;     S.FogDensity = std::max(0.0f, InValue); break;
            case EPostProcessParam::MotionIntensity:  S.bMotionBlur = 1;        S.MotionIntensity = std::max(0.0f, InValue); break;
            case EPostProcessParam::AoIntensity:      S.bAmbientOcclusion = 1;  S.AoIntensity = std::max(0.0f, InValue); break;
            case EPostProcessParam::SsrIntensity:     S.bScreenReflections = 1; S.SsrIntensity = std::max(0.0f, InValue); break;
            case EPostProcessParam::Count:            break;
        }
    }

    void SetPostProcessOverride(const void* InWorld, const PostProcessOverride& InOverride)
    {
        std::lock_guard<std::mutex> Lock(OverrideMutex());
        if(InOverride.IsEmpty()) { Overrides().erase(InWorld); }
        else                     { Overrides()[InWorld] = InOverride; }
    }

    PostProcessOverride GetPostProcessOverride(const void* InWorld)
    {
        std::lock_guard<std::mutex> Lock(OverrideMutex());
        const auto Found = Overrides().find(InWorld);
        return Found != Overrides().end() ? Found->second : PostProcessOverride();
    }

    void ApplyPostProcessOverride(const void* InWorld, PostProcessSettings& InOutSettings)
    {
        const PostProcessOverride Override = GetPostProcessOverride(InWorld);
        for(const PostProcessOverride::Stack& Each : Override.Stacks)
        {
            if(!(Each.Weight > 0.0f)) { continue; }
            if(const std::shared_ptr<const PostProcessSettings> Stack = PostProcessStackCache::Find(Each.Path))
            {
                BlendPostProcess(InOutSettings, *Stack, std::min(Each.Weight, 1.0f));
            }
        }
        for(const PostProcessOverride::Param& Each : Override.Params)
        {
            ApplyPostProcessParam(InOutSettings, Each.Param, Each.Value);
        }
    }
}
