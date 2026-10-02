#ifndef RENDERCORE_MATERIALPATHUPGRADE_H
#define RENDERCORE_MATERIALPATHUPGRADE_H

#include <cstddef>
#include <cstring>
#include <string>

#include "Public/RenderCore/MaterialDescription.h"

namespace RenderCore
{
    /* For a component serializer loading a version from before materials were
     * paths: a bare name ("Glow") is the graph Materials/Glow.material.nmgraph,
     * which is what it meant. */
    inline void UpgradeMaterialName(char* InOutMaterial, const size_t InCapacity)
    {
        const std::string Name(InOutMaterial, strnlen(InOutMaterial, InCapacity));
        std::string Stem;
        if(Name.empty() || Name.find('/') != std::string::npos || !GetMaterialStem(Name, Stem))
        {
            return;
        }
        const std::string Path = Stem + MATERIAL_GRAPH_EXTENSION;
        if(Path.size() < InCapacity)
        {
            std::memcpy(InOutMaterial, Path.c_str(), Path.size() + 1);
        }
    }
}

#endif //RENDERCORE_MATERIALPATHUPGRADE_H
