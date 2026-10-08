#if WITH_EDITOR

#include "Private/Editor/Build/ProjectBuild.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "Private/Editor/ConfigIniFile.h"
#include "Private/Editor/ProjectSettingsDialog.h"

namespace
{
    const char* const BuildSection = "Build";

    /* ${hostSystemName}, as CMake spells it. */
    QString HostSystemName()
    {
#if defined(_WIN32)
        return QStringLiteral("Windows");
#elif defined(__APPLE__)
        return QStringLiteral("Darwin");
#else
        return QStringLiteral("Linux");
#endif
    }

    /* The Binaries/<Platform>_ name (Engine/CMakeLists.txt). */
    QString BinariesPlatform()
    {
#if defined(_WIN32)
        return QStringLiteral("Windows");
#elif defined(__APPLE__)
        return QStringLiteral("Mac");
#else
        return QStringLiteral("Linux");
#endif
    }

    /* A preset's own condition: equals / notEquals on ${hostSystemName}.
     * Anything else is taken to hold; cmake will say if it does not. */
    bool ConditionHolds(const QJsonObject& InPreset)
    {
        const QJsonObject Condition = InPreset.value(QStringLiteral("condition")).toObject();
        if(Condition.isEmpty())
        {
            return true;
        }
        const QString Type = Condition.value(QStringLiteral("type")).toString();
        const QString Lhs  = Condition.value(QStringLiteral("lhs")).toString().replace(QStringLiteral("${hostSystemName}"), HostSystemName());
        const QString Rhs  = Condition.value(QStringLiteral("rhs")).toString().replace(QStringLiteral("${hostSystemName}"), HostSystemName());
        if(Type == QStringLiteral("equals"))    { return Lhs == Rhs; }
        if(Type == QStringLiteral("notEquals")) { return Lhs != Rhs; }
        return true;
    }

    bool HasUsablePreset(const QJsonArray& InPresets, const QString& InName)
    {
        for(const QJsonValue& Value : InPresets)
        {
            const QJsonObject Preset = Value.toObject();
            if(Preset.value(QStringLiteral("name")).toString() == InName)
            {
                return !Preset.value(QStringLiteral("hidden")).toBool() && ConditionHolds(Preset);
            }
        }
        return false;
    }

    bool ParseBool(const QString& InText, const bool bInDefault)
    {
        const QString T = InText.trimmed().toLower();
        if(T == QStringLiteral("true") || T == QStringLiteral("1") || T == QStringLiteral("yes") || T == QStringLiteral("on"))   { return true; }
        if(T == QStringLiteral("false") || T == QStringLiteral("0") || T == QStringLiteral("no") || T == QStringLiteral("off")) { return false; }
        return bInDefault;
    }
}

namespace EditorProjectBuild
{
    const char* GetConfigurationName(const EConfiguration InConfiguration)
    {
        switch(InConfiguration)
        {
        case BC_Debug:          return "Debug";
        case BC_RelWithDebInfo: return "RelWithDebInfo";
        default:                return "Shipping";
        }
    }

    EConfiguration ParseConfiguration(const QString& InText)
    {
        for(int Index = 0; Index < BC_Count; ++Index)
        {
            if(InText.trimmed().compare(QLatin1String(GetConfigurationName(EConfiguration(Index))), Qt::CaseInsensitive) == 0)
            {
                return EConfiguration(Index);
            }
        }
        return BC_Shipping;
    }

    BuildOptions LoadOptions()
    {
        BuildOptions Options;
        ConfigIniFile File;
        if(!File.Load(ProjectSettingsDialog::GetProjectConfigPath()))
        {
            return Options;
        }
        const QString Section = QLatin1String(BuildSection);
        bool bFound = false;
        const QString Configuration = File.Get(Section, QStringLiteral("Configuration"), &bFound);
        if(bFound) { Options.Configuration = ParseConfiguration(Configuration); }
        Options.bCook       = ParseBool(File.Get(Section, QStringLiteral("Cook")), Options.bCook);
        Options.bCompile    = ParseBool(File.Get(Section, QStringLiteral("Compile")), Options.bCompile);
        Options.bOpenOutput = ParseBool(File.Get(Section, QStringLiteral("OpenOutput")), Options.bOpenOutput);
        return Options;
    }

    bool SaveOptions(const BuildOptions& InOptions, QString* OutError)
    {
        const QString Path = ProjectSettingsDialog::GetProjectConfigPath();
        ConfigIniFile File;
        if(!File.Load(Path))
        {
            if(OutError) { *OutError = File.GetError(); }
            return false;
        }

        const QString Section = QLatin1String(BuildSection);
        const auto Bool = [](const bool bIn) { return bIn ? QStringLiteral("true") : QStringLiteral("false"); };
        const QList<QPair<QString, QString>> Wanted = {
            { QStringLiteral("Configuration"), QLatin1String(GetConfigurationName(InOptions.Configuration)) },
            { QStringLiteral("Cook"),          Bool(InOptions.bCook) },
            { QStringLiteral("Compile"),       Bool(InOptions.bCompile) },
            { QStringLiteral("OpenOutput"),    Bool(InOptions.bOpenOutput) },
        };
        bool bChanged = false;
        for(const QPair<QString, QString>& Key : Wanted)
        {
            bool bFound = false;
            if(File.Get(Section, Key.first, &bFound) != Key.second || !bFound)
            {
                File.Set(Section, Key.first, Key.second);
                bChanged = true;
            }
        }
        if(bChanged && !File.Save(Path))
        {
            if(OutError) { *OutError = File.GetError(); }
            return false;
        }
        return true;
    }

    QString FindClientPreset(const QString& InProjectRoot, const EConfiguration InConfiguration, QString* OutError)
    {
        const QString Path = QDir(InProjectRoot).filePath(QStringLiteral("CMakePresets.json"));
        QFile File(Path);
        if(!File.open(QIODevice::ReadOnly))
        {
            if(OutError) { *OutError = QStringLiteral("%1 cannot be read: %2").arg(QDir::toNativeSeparators(Path), File.errorString()); }
            return QString();
        }
        QJsonParseError ParseError;
        const QJsonObject Root = QJsonDocument::fromJson(File.readAll(), &ParseError).object();
        if(ParseError.error != QJsonParseError::NoError)
        {
            if(OutError) { *OutError = QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(Path), ParseError.errorString()); }
            return QString();
        }

        const QJsonArray Configure = Root.value(QStringLiteral("configurePresets")).toArray();
        const QJsonArray Build     = Root.value(QStringLiteral("buildPresets")).toArray();
        const QString    Base      = QStringLiteral("client-") + QString::fromLatin1(GetConfigurationName(InConfiguration)).toLower();
        for(const QString& Candidate : { Base + QStringLiteral("-clang"), Base })
        {
            if(HasUsablePreset(Configure, Candidate) && HasUsablePreset(Build, Candidate))
            {
                return Candidate;
            }
        }
        if(OutError)
        {
            *OutError = QStringLiteral("%1 has no configure and build preset named %2 or %2-clang for this platform")
                        .arg(QDir::toNativeSeparators(Path), Base);
        }
        return QString();
    }

    QString GetOutputDir(const QString& InProjectRoot, const EConfiguration InConfiguration)
    {
        return QDir(InProjectRoot).filePath(QStringLiteral("Binaries/%1_%2_Client")
                                            .arg(BinariesPlatform(), QLatin1String(GetConfigurationName(InConfiguration))));
    }

    QStringList ConfigureArgs(const QString& InPreset)
    {
        /* Always packaged: the content in Paks/ (NimblePack.cmake), with each
         * path kept beside its hash in every configuration but Shipping. */
        return { QStringLiteral("--preset"), InPreset, QStringLiteral("-DNIMBLE_PACKAGE_CONTENT=ON") };
    }

    QStringList BuildArgs(const QString& InPreset)
    {
        return { QStringLiteral("--build"), QStringLiteral("--preset"), InPreset };
    }
}

#endif // WITH_EDITOR
