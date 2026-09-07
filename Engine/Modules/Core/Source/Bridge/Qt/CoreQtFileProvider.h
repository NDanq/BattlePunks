#ifndef ENGINECORE_COREQTFILEPROVIDER_H
#define ENGINECORE_COREQTFILEPROVIDER_H

#include <QString>

#include "Public/Core/CoreFileProvider.h"

/*
 * Reads resources out of a Qt .qrc bundle compiled into the executable.
 *
 * Odessa ships its cooked shaders this way, so paths that GDI hands over --
 * "Shaders/Cooked/Vulkan/Basic.vsh.spv" -- have to be resolved against the
 * ":" resource prefix. That prefix is exactly what this class adds, and the
 * reason GDI's own paths no longer carry it: a standalone or SDL host reads
 * the same relative path off disk through FilesystemProvider instead.
 *
 * BINARY, ALWAYS. QFile is opened without QIODevice::Text. Reading SPIR-V as
 * text on Windows translates CRLF and stops at 0x1A, which corrupts the
 * module in a way that surfaces much later as an unexplained pipeline
 * failure. There is no case where a text-mode read is wanted here.
 */

namespace EngineCore
{
namespace QtBridge
{
    class QrcFileProvider : public IFileProvider
    {
    public:
        /* InPrefix defaults to Qt's resource root. */
        explicit QrcFileProvider(QString InPrefix = QStringLiteral(":"))
            : Prefix(std::move(InPrefix)) {}

        bool ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes) override;
        bool Exists (const std::string& InPath) override;

    private:
        QString Resolve(const std::string& InPath) const;

        QString Prefix;
    };
}
}

#endif //ENGINECORE_COREQTFILEPROVIDER_H
