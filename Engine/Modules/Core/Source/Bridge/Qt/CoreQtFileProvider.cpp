#include "CoreQtFileProvider.h"

#include <QFile>

namespace EngineCore
{
namespace QtBridge
{
QString QrcFileProvider::Resolve(const std::string& InPath) const
{
    return Prefix + QString::fromUtf8(InPath.data(), int(InPath.size()));
}

bool QrcFileProvider::ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes)
{
    OutBytes.clear();

    QFile File(Resolve(InPath));

    // ReadOnly only -- never QIODevice::Text. See the note in the header:
    // text mode corrupts SPIR-V on Windows.
    if(!File.open(QIODevice::ReadOnly))
    {
        return false;
    }

    const QByteArray Bytes = File.readAll();
    File.close();

    OutBytes.assign(reinterpret_cast<const uint8_t*>(Bytes.constData()),
                    reinterpret_cast<const uint8_t*>(Bytes.constData()) + Bytes.size());
    return true;
}

bool QrcFileProvider::Exists(const std::string& InPath)
{
    return QFile::exists(Resolve(InPath));
}
}
}
