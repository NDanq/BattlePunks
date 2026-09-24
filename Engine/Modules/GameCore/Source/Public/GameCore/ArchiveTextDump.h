#ifndef GAMECORE_ARCHIVETEXTDUMP_H
#define GAMECORE_ARCHIVETEXTDUMP_H

#include <string>

#include "Public/Core/CoreArchive.h"
#include "Public/GameCore/GameCoreAPI.h"

namespace GameCore
{
    /*
     * An archive's chunk tree as text: tag, version, size and offset per chunk,
     * for debugging a save. Field values are not shown -- their meaning lives in
     * each type's serializer. Safe on a failed or mid-load archive.
     *
     * A reader only. A text parser would be a second format to keep in step
     * with every Serialize; an editable scene format belongs in a separate tool
     * that emits a binary archive.
     */
    GAMECORE_API bool DumpArchiveToText(const EngineCore::Archive& InArchive, std::string& OutText);
}

#endif //GAMECORE_ARCHIVETEXTDUMP_H
