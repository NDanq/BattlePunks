#ifndef RENDERCORE_ASSETIMPORTINFO_H
#define RENDERCORE_ASSETIMPORTINFO_H

#include <cstdint>
#include <string>

/* An imported asset's source and settings (optional 'ISRC' chunk), for
 * Reimport. Ignored at run time. */
namespace RenderCore
{
    struct AssetImportInfo
    {
        /* Project-relative if under the project root, else absolute. Forward slashes. */
        std::string SourcePath;

        /* Source mtime at import (epoch seconds), to detect a changed source. */
        int64_t SourceTime = 0;

        /* The importer's options as "Key=Value" lines; the editor's to define. */
        std::string Settings;

        bool IsEmpty() const { return SourcePath.empty(); }
    };
}

#endif //RENDERCORE_ASSETIMPORTINFO_H
