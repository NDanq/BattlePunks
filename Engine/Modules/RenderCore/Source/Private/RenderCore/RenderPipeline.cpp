#include "Public/RenderCore/RenderPipeline.h"

namespace RenderCore
{
    const char* GetRenderStageName(const ERenderStage InStage)
    {
        switch(InStage)
        {
            case ERenderStage::PrePass:          return "PrePass";
            case ERenderStage::BasePass:         return "BasePass";
            case ERenderStage::Shadows:          return "Shadows";
            case ERenderStage::Lighting:         return "Lighting";
            case ERenderStage::Unlit:            return "Unlit";
            case ERenderStage::TranslucentLit:   return "TranslucentLit";
            case ERenderStage::TranslucentUnlit: return "TranslucentUnlit";
            case ERenderStage::PostProcess:      return "PostProcess";
            case ERenderStage::Count:            break;
        }
        return "Unknown";
    }

    ERenderStage GetMaterialRenderStage(const EMaterialDomain InDomain, const EMaterialShading InShading)
    {
        const bool bLit = InShading == EMaterialShading::Lit;
        switch(InDomain)
        {
            case EMaterialDomain::Opaque:
            case EMaterialDomain::Masked:      return bLit ? ERenderStage::BasePass : ERenderStage::Unlit;
            case EMaterialDomain::Translucent: return bLit ? ERenderStage::TranslucentLit : ERenderStage::TranslucentUnlit;
            case EMaterialDomain::PostProcess: return ERenderStage::PostProcess;
        }
        return ERenderStage::Unlit;
    }
}
