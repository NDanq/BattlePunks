#include "Public/RenderCore/MaterialDescription.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace RenderCore
{
    namespace
    {
        uint32_t TypeBytes(const std::string& InType)
        {
            if(InType == "float") { return 4; }
            if(InType == "vec2")  { return 8; }
            if(InType == "vec3")  { return 12; }
            if(InType == "vec4")  { return 16; }
            return 0;
        }

        void PutFloat(std::vector<uint8_t>& InOutBytes, const uint32_t InOffset, const float InValue)
        {
            std::memcpy(InOutBytes.data() + InOffset, &InValue, sizeof(float));
        }
    }

    std::string GetPermutationFileName(const std::string& InMaterial, const uint32_t InKey, const bool bInVertex)
    {
        char Key[16];
        std::snprintf(Key, sizeof(Key), "%08x", InKey);
        return InMaterial + "@" + Key + (bInVertex ? ".vsh" : ".fsh");
    }

    bool GetMaterialStem(const std::string& InMaterial, std::string& OutStem)
    {
        const size_t Extension = std::strlen(MATERIAL_GRAPH_EXTENSION);
        if(InMaterial.size() > Extension
           && InMaterial.compare(InMaterial.size() - Extension, Extension, MATERIAL_GRAPH_EXTENSION) == 0)
        {
            OutStem = InMaterial.substr(0, InMaterial.size() - Extension);
            return true;
        }
        if(InMaterial.empty() || InMaterial.find_first_of("/\\.") != std::string::npos)
        {
            return false;
        }
        OutStem = "Materials/" + InMaterial;
        return true;
    }

    const MaterialParameter* MaterialDescription::FindParameter(const std::string& InName) const
    {
        for(const MaterialParameter& Each : Parameters)
        {
            if(Each.Name == InName) { return &Each; }
        }
        return nullptr;
    }

    const char* GetMaterialDomainName(const EMaterialDomain InDomain)
    {
        switch(InDomain)
        {
            case EMaterialDomain::Opaque:      return "Opaque";
            case EMaterialDomain::Masked:      return "Masked";
            case EMaterialDomain::Translucent: return "Translucent";
            case EMaterialDomain::PostProcess: return "PostProcess";
        }
        return "Opaque";
    }

    namespace
    {
        const char* const SHADING_MODEL_NAMES[MATERIAL_SHADING_MODEL_COUNT] = {
            "Default", "ClearCoat", "Anisotropic", "Cloth", "Subsurface", "Skin", "Volume",
        };
    }

    const char* GetShadingModelName(const EMaterialShadingModel InModel)
    {
        return uint32_t(InModel) < MATERIAL_SHADING_MODEL_COUNT ? SHADING_MODEL_NAMES[uint32_t(InModel)] : "Default";
    }

    bool FindShadingModel(const std::string& InName, EMaterialShadingModel& OutModel)
    {
        for(uint32_t Index = 0; Index < MATERIAL_SHADING_MODEL_COUNT; ++Index)
        {
            if(InName == SHADING_MODEL_NAMES[Index])
            {
                OutModel = EMaterialShadingModel(Index);
                return true;
            }
        }
        return false;
    }

    bool ParseMaterialDescription(const std::string& InText, MaterialDescription& OutDescription, std::string& OutError)
    {
        MaterialDescription Result;
        struct UVLine
        {
            uint32_t                 Slot  = 0;
            float                    Scale = 1.0f;
            std::vector<std::string> Params;
        };
        std::vector<UVLine> UVLines;
        std::istringstream Lines(InText);
        std::string Line;
        bool bHeader = false;
        while(std::getline(Lines, Line))
        {
            const size_t Hash = Line.find('#');
            if(Hash != std::string::npos) { Line.resize(Hash); }

            std::istringstream Words(Line);
            std::string Keyword;
            if(!(Words >> Keyword)) { continue; }

            if(Keyword == "NimbleMaterial")
            {
                int Version = 0;
                Words >> Version;
                if(Version != 3)
                {
                    OutError = "version " + std::to_string(Version) + " material; recompile its graph (version 3 is current)";
                    return false;
                }
                bHeader = true;
                continue;
            }

            std::string Value;
            if(Keyword == "Domain")
            {
                Words >> Value;
                if(Value == "Opaque")           { Result.Domain = EMaterialDomain::Opaque; }
                else if(Value == "Masked")      { Result.Domain = EMaterialDomain::Masked; }
                else if(Value == "Translucent") { Result.Domain = EMaterialDomain::Translucent; }
                else if(Value == "PostProcess") { Result.Domain = EMaterialDomain::PostProcess; }
                else { OutError = "unknown Domain '" + Value + "'"; return false; }
            }
            else if(Keyword == "Shading")
            {
                Words >> Value;
                if(Value == "Lit")        { Result.Shading = EMaterialShading::Lit; }
                else if(Value == "Unlit") { Result.Shading = EMaterialShading::Unlit; }
                else { OutError = "unknown Shading '" + Value + "'"; return false; }
            }
            else if(Keyword == "ShadingModel")
            {
                Words >> Value;
                if(!FindShadingModel(Value, Result.ShadingModel)) { OutError = "unknown ShadingModel '" + Value + "'"; return false; }
            }
            else if(Keyword == "TwoSided")  { int Flag = 0; Words >> Flag; Result.bTwoSided  = Flag != 0; }
            else if(Keyword == "ParticleLighting") { int Flag = 0; Words >> Flag; Result.bParticleLighting = Flag != 0; }
            else if(Keyword == "TranslucentShadow") { Words >> Result.TranslucentShadow; Result.TranslucentShadow = std::clamp(Result.TranslucentShadow, 0.0f, 1.0f); }
            else if(Keyword == "TranslucencyLighting")
            {
                Words >> Value;
                if(Value == "Surface")     { Result.bTranslucencySurface = true; }
                else if(Value == "Legacy") { Result.bTranslucencySurface = false; }
                else { OutError = "unknown TranslucencyLighting '" + Value + "'"; return false; }
            }
            else if(Keyword == "HasOffset") { int Flag = 0; Words >> Flag; Result.bHasOffset = Flag != 0; }
            else if(Keyword == "ClipValue") { Words >> Result.ClipValue; }
            else if(Keyword == "MaxOffset") { Words >> Result.MaxOffset; }
            else if(Keyword == "BlockSize") { Words >> Result.BlockSize; }
            else if(Keyword == "Param")
            {
                MaterialParameter Param;
                Words >> Param.Name >> Param.Type >> Param.Offset
                      >> Param.Default[0] >> Param.Default[1] >> Param.Default[2] >> Param.Default[3];
                const uint32_t Bytes = TypeBytes(Param.Type);
                if(Words.fail() || Bytes == 0)
                {
                    OutError = "bad parameter line '" + Line + "'";
                    return false;
                }
                Result.Parameters.push_back(Param);
            }
            else if(Keyword == "Texture")
            {
                /* Texture <Slot> <Filter> <Wrap> <Path>: the path is the rest of the line. */
                MaterialTexture Texture;
                std::string Filter, Wrap, Path;
                Words >> Texture.Slot >> Filter >> Wrap;
                std::getline(Words, Path);
                const size_t First = Path.find_first_not_of(" \t");
                const size_t Last  = Path.find_last_not_of(" \t\r");
                Texture.Path = First == std::string::npos ? std::string() : Path.substr(First, Last - First + 1);

                bool bKnown = true;
                if(Filter == "Nearest")      { Texture.bFilter = true; Texture.Filter = ETextureFilter::Nearest; }
                else if(Filter == "Linear")  { Texture.bFilter = true; Texture.Filter = ETextureFilter::Linear; }
                else if(Filter != "Default") { bKnown = false; }
                if(Wrap == "Repeat")         { Texture.bWrap = true; Texture.Wrap = ETextureWrap::Repeat; }
                else if(Wrap == "Clamp")     { Texture.bWrap = true; Texture.Wrap = ETextureWrap::Clamp; }
                else if(Wrap == "Mirror")    { Texture.bWrap = true; Texture.Wrap = ETextureWrap::Mirror; }
                else if(Wrap != "Default")   { bKnown = false; }

                if(Words.bad() || !bKnown || Texture.Path.empty()
                   || Texture.Slot < MATERIAL_TEXTURE_SLOT_FIRST || Texture.Slot >= MATERIAL_TEXTURE_SLOTS)
                {
                    OutError = "bad texture line '" + Line + "'";
                    return false;
                }
                Result.Textures.push_back(Texture);
                continue;   // getline leaves the stream at its end: not a failure
            }
            else if(Keyword == "TextureUV")
            {
                /* TextureUV <Slot> <x> [<Param> ...]: resolved below, once every Param is known. */
                uint32_t Slot = 0;
                float    Scale = 1.0f;
                Words >> Slot >> Scale;
                std::vector<std::string> Params;
                std::string Param;
                while(Words >> Param) { Params.push_back(Param); }
                if(Words.bad())
                {
                    OutError = "bad TextureUV line '" + Line + "'";
                    return false;
                }
                UVLines.push_back({ Slot, Scale, std::move(Params) });
                continue;
            }
            else
            {
                continue;   // a newer line this build does not know
            }

            if(Words.fail())
            {
                OutError = "bad line '" + Line + "'";
                return false;
            }
        }

        if(!bHeader)
        {
            OutError = "no NimbleMaterial header";
            return false;
        }
        if(Result.BlockSize < MATERIAL_ENGINE_BLOCK_BYTES || Result.BlockSize > MATERIAL_BLOCK_MAX_BYTES)
        {
            OutError = "BlockSize " + std::to_string(Result.BlockSize) + " is outside " + std::to_string(MATERIAL_ENGINE_BLOCK_BYTES)
                     + "-" + std::to_string(MATERIAL_BLOCK_MAX_BYTES) + " bytes";
            return false;
        }
        for(const MaterialParameter& Each : Result.Parameters)
        {
            if(Each.Offset < MATERIAL_ENGINE_BLOCK_BYTES || Each.Offset + TypeBytes(Each.Type) > Result.BlockSize)
            {
                OutError = "parameter '" + Each.Name + "' lies outside the block";
                return false;
            }
        }

        /* Each texture's smallest UV scale: the sample that wants the most of it. */
        std::vector<bool> bScaled(Result.Textures.size(), false);
        for(const UVLine& Each : UVLines)
        {
            float Scale = std::abs(Each.Scale);
            for(const std::string& Name : Each.Params)
            {
                const MaterialParameter* Param = Result.FindParameter(Name);
                if(!Param) { Scale = 0.0f; break; }   // not known: no scale for this sample
                Scale *= std::max({ std::abs(Param->Default[0]), std::abs(Param->Default[1]),
                                    std::abs(Param->Default[2]), std::abs(Param->Default[3]) });
            }
            for(size_t Index = 0; Index < Result.Textures.size(); ++Index)
            {
                MaterialTexture& Texture = Result.Textures[Index];
                if(Texture.Slot != Each.Slot) { continue; }
                if(Scale <= 0.0f)
                {
                    Texture.StreamUVScale = 1.0f;   // a sample it cannot read decides: as UV0
                    bScaled[Index] = true;
                    continue;
                }
                Texture.StreamUVScale = bScaled[Index] ? std::min(Texture.StreamUVScale, Scale) : Scale;
                bScaled[Index] = true;
            }
        }

        OutDescription = std::move(Result);
        return true;
    }

    void BuildMaterialBlock(const MaterialDescription& InDescription, const EngineCore::Color InTint, std::vector<uint8_t>& OutBytes)
    {
        OutBytes.assign(InDescription.BlockSize, 0);

        PutFloat(OutBytes, 0,  InTint.Rf());
        PutFloat(OutBytes, 4,  InTint.Gf());
        PutFloat(OutBytes, 8,  InTint.Bf());
        PutFloat(OutBytes, 12, InTint.Af());
        PutFloat(OutBytes, 16, InDescription.ClipValue);
        PutFloat(OutBytes, 20, InDescription.bTwoSided ? 1.0f : 0.0f);

        for(const MaterialParameter& Each : InDescription.Parameters)
        {
            const uint32_t Lanes = TypeBytes(Each.Type) / 4;
            for(uint32_t Lane = 0; Lane < Lanes; ++Lane)
            {
                PutFloat(OutBytes, Each.Offset + Lane * 4, Each.Default[Lane]);
            }
        }
    }
}
