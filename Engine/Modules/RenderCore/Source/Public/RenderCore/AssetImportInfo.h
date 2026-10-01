#ifndef RENDERCORE_ASSETIMPORTINFO_H
#define RENDERCORE_ASSETIMPORTINFO_H

#include <cstdint>
#include <string>

/*
 * Where an imported asset came from, kept in the asset itself (an optional
 * 'ISRC' chunk) so the editor can import it again with the same settings.
 * Nothing at run time reads it; a cooked asset without one loads the same.
 */
namespace RenderCore
{
    struct AssetImportInfo
    {
        /* The source file, relative to the project root when it is under it
         * ("Source/Crate.glb"), else absolute. Forward slashes. */
        std::string SourcePath;

        /* The source's modification time when imported, in seconds since the
         * epoch -- so the editor can say the source has changed since. */
        int64_t SourceTime = 0;

        /* The importer's options as "Key=Value" lines; the editor's to define. */
        std::string Settings;

        bool IsEmpty() const { return SourcePath.empty(); }
    };
}

#endif //RENDERCORE_ASSETIMPORTINFO_H
