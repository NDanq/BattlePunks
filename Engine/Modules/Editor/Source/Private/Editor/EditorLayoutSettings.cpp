#if WITH_EDITOR

#include "Private/Editor/EditorLayoutSettings.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace EditorLayoutSettings
{
    QSettings& Get()
    {
        static QSettings Settings(QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                                  .filePath(QStringLiteral("EditorLayout.ini")), QSettings::IniFormat);
        return Settings;
    }
}

#endif // WITH_EDITOR
