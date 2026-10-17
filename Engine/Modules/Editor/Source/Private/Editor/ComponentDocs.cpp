#if WITH_EDITOR

#include "Private/Editor/ComponentDocs.h"

#include <cstring>
#include <vector>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ScriptStruct.h"
#include "Private/Editor/Project/ProjectTemplate.h"

using namespace GameCore;

namespace
{
    QString& RootOverride()
    {
        static QString Root;
        return Root;
    }

    QString TypeNameOf(const ComponentTypeInfo& InInfo)
    {
        return QString::fromLatin1(InInfo.Name);
    }

    /* "Audio Source" -> "AudioSource". */
    QString Squashed(const QString& InName)
    {
        QString Out;
        for(const QChar Each : InName)
        {
            if(Each.isLetterOrNumber() || Each == QLatin1Char('_')) { Out += Each; }
        }
        return Out;
    }

    /* The C++ class by the engine's convention: AudioSourceComponent. */
    QString ClassOf(const QString& InTypeName)
    {
        const QString Name = Squashed(InTypeName);
        return Name.endsWith(QStringLiteral("Component")) ? Name : Name + QStringLiteral("Component");
    }

    struct FHeader
    {
        QString Path;        // "Public/GameCore/Components/AudioSourceComponent.h"
        QString Namespace;   // "GameCore"
    };

    QString ReadText(const QString& InPath)
    {
        QFile File(InPath);
        return File.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(File.readAll()) : QString();
    }

    /* The first namespace the header opens on a line of its own. */
    QString NamespaceOf(const QString& InText)
    {
        static const QRegularExpression Opened(QStringLiteral("^namespace\\s+(\\w+)\\s*$"), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch Match = Opened.match(InText);
        return Match.hasMatch() ? Match.captured(1) : QString();
    }

    /* The public header declaring InClass, in the engine's modules (<Module>/Source) or the NimbleEngine
     * library (Engine/Source): <InClass>.h, else any header that declares it. Empty if not found. */
    FHeader HeaderOf(const QString& InClass)
    {
        const QString Engine = EditorProject::GetEngineDir();
        if(Engine.isEmpty()) { return {}; }

        QStringList Sources{ QDir(Engine).filePath(QStringLiteral("Source")) };
        QDirIterator Modules(QDir(Engine).filePath(QStringLiteral("Modules")), QDir::Dirs | QDir::NoDotAndDotDot);
        while(Modules.hasNext()) { Sources << QDir(Modules.next()).filePath(QStringLiteral("Source")); }

        const QRegularExpression Declared(QStringLiteral("\\b(struct|class)\\s+(\\w+_API\\s+)?%1\\b\\s*(final\\s*)?[:{]").arg(InClass));
        for(const bool bByContent : { false, true })
        {
            for(const QString& Source : Sources)
            {
                QDirIterator Files(QDir(Source).filePath(QStringLiteral("Public")),
                                   bByContent ? QStringList{ QStringLiteral("*.h") } : QStringList{ InClass + QStringLiteral(".h") },
                                   QDir::Files, QDirIterator::Subdirectories);
                while(Files.hasNext())
                {
                    const QString File = Files.next();
                    const QString Text = ReadText(File);
                    if(bByContent && !Declared.match(Text).hasMatch()) { continue; }
                    return { QStringLiteral("Public/") + QDir(QDir(Source).filePath(QStringLiteral("Public"))).relativeFilePath(File),
                             NamespaceOf(Text) };
                }
            }
        }
        return {};
    }

    const char* LuaTypeOf(const EScriptFieldType InType)
    {
        switch(InType)
        {
            case EScriptFieldType::Float:
            case EScriptFieldType::Double:      return "number";
            case EScriptFieldType::Int32:
            case EScriptFieldType::UInt8:       return "integer";
            case EScriptFieldType::Bool:
            case EScriptFieldType::Flag:        return "boolean";
            case EScriptFieldType::Vector3:     return "x, y, z";
            case EScriptFieldType::AABB:        return "minX, minY, minZ, maxX, maxY, maxZ";
            case EScriptFieldType::Color:       return "r, g, b, a";
            case EScriptFieldType::FixedString: return "string";
            case EScriptFieldType::Struct:      return "struct";
        }
        return "";
    }

    QString CppTypeOf(const EScriptFieldType InType)
    {
        switch(InType)
        {
            case EScriptFieldType::Float:       return QStringLiteral("float");
            case EScriptFieldType::Double:      return QStringLiteral("double");
            case EScriptFieldType::Int32:       return QStringLiteral("int32_t");
            case EScriptFieldType::UInt8:       return QStringLiteral("uint8_t");
            case EScriptFieldType::Bool:        return QStringLiteral("bool");
            case EScriptFieldType::Flag:        return QStringLiteral("uint8_t (flag)");
            case EScriptFieldType::Vector3:     return QStringLiteral("Vector3");
            case EScriptFieldType::AABB:        return QStringLiteral("AABB");
            case EScriptFieldType::Color:       return QStringLiteral("Color");
            case EScriptFieldType::FixedString: return QStringLiteral("char[N]");
            case EScriptFieldType::Struct:      return QStringLiteral("struct");
        }
        return QString();
    }

    QString Real(const double InValue)
    {
        return QString::number(InValue, 'g', 6);
    }

    /* A property's default, from the default-constructed component's bytes. */
    QString DefaultOf(const ComponentPropertyEntry& InEntry, const std::vector<uint8_t>& InDefaults)
    {
        if(InDefaults.empty() || InEntry.IsArray() || size_t(InEntry.Offset) + InEntry.Size > InDefaults.size())
        {
            return InEntry.IsArray() ? QStringLiteral("(%1 elements)").arg(InEntry.ArrayCount) : QStringLiteral("-");
        }
        const uint8_t* Field = InDefaults.data() + InEntry.Offset;
        switch(InEntry.Type)
        {
            case EPropertyType::Float:  { float V;   std::memcpy(&V, Field, sizeof(V)); return Real(V); }
            case EPropertyType::Double: { double V;  std::memcpy(&V, Field, sizeof(V)); return Real(V); }
            case EPropertyType::Int32:  { int32_t V; std::memcpy(&V, Field, sizeof(V)); return QString::number(V); }
            case EPropertyType::UInt8:  return QString::number(*Field);
            case EPropertyType::Bool:
            case EPropertyType::Flag:   return *Field ? QStringLiteral("true") : QStringLiteral("false");
            case EPropertyType::Vector3:
            {
                const auto& V = *reinterpret_cast<const EngineCore::Vector3*>(Field);
                return QStringLiteral("(%1, %2, %3)").arg(Real(V.GetX()), Real(V.GetY()), Real(V.GetZ()));
            }
            case EPropertyType::AABB:
            {
                const auto& V = *reinterpret_cast<const AABB*>(Field);
                return QStringLiteral("(%1, %2, %3) to (%4, %5, %6)").arg(Real(V.Min.GetX()), Real(V.Min.GetY()), Real(V.Min.GetZ()),
                                                                         Real(V.Max.GetX()), Real(V.Max.GetY()), Real(V.Max.GetZ()));
            }
            case EPropertyType::Color:
            {
                const auto& V = *reinterpret_cast<const EngineCore::Color*>(Field);
                return QStringLiteral("(%1, %2, %3, %4)").arg(V.R).arg(V.G).arg(V.B).arg(V.A);
            }
            case EPropertyType::FixedString:
            {
                size_t Length = 0;
                while(Length < InEntry.Size && Field[Length] != 0) { ++Length; }
                return Length == 0 ? QStringLiteral("(empty)")
                                   : QStringLiteral("`%1`").arg(QString::fromUtf8(reinterpret_cast<const char*>(Field), qsizetype(Length)));
            }
            case EPropertyType::Struct: break;
        }
        return QStringLiteral("-");
    }

    QString AssetKindOf(const uint16_t InFlags)
    {
        struct Kind { EPropertyFlags Flag; const char* Text; };
        static const Kind Kinds[] = {
            { PF_AssetMesh, "mesh (.nmmesh)" },               { PF_AssetMaterial, "material graph (.material.nmgraph)" },
            { PF_AssetPostProcess, "post process stack (.pp.nmgraph)" },
            { PF_AssetRenderTarget, "render target (.nmrt2d, .nmrt3d)" }, { PF_AssetTexture, "texture (.nmtexture)" },
            { PF_AssetParticleSystem, "particle system (.nmparticle)" },
            { PF_AssetAnimation, "animation (.nmanim, .nmanimstate)" }, { PF_AssetSound, "sound (.nmsound)" },
            { PF_AssetPrefab, "prefab (.nmprefab)" },         { PF_AssetSequence, "sequence (.nmseq)" },
            { PF_AssetTerrain, "terrain (.nmterrain)" },      { PF_AssetFoliage, "foliage (.nmfoliage)" },
            { PF_AssetNavMesh, "navmesh (.nmnav)" },          { PF_AssetBehaviorTree, "behavior tree (.nmbt)" },
        };
        for(const Kind& Each : Kinds)
        {
            if(InFlags & Each.Flag) { return QString::fromLatin1(Each.Text); }
        }
        return QString();
    }

    /* Table cells may not hold '|'. */
    QString Cell(QString InText)
    {
        return InText.replace(QLatin1Char('|'), QStringLiteral("\\|"));
    }

    /* The property's description for the Notes column. */
    QString NotesOf(const ComponentPropertyEntry& InEntry)
    {
        QStringList Notes;
        if(InEntry.IsReadOnly()) { Notes << QStringLiteral("read-only"); }
        if(InEntry.IsChoice())
        {
            Notes << QStringLiteral("choice: %1").arg(QString::fromLatin1(InEntry.Choices).split(QLatin1Char('|')).join(QStringLiteral(", ")));
        }
        const QString Asset = AssetKindOf(InEntry.Flags);
        if(!Asset.isEmpty()) { Notes << QStringLiteral("asset: %1").arg(Asset); }
        if(InEntry.IsArray()) { Notes << QStringLiteral("array of %1").arg(InEntry.ArrayCount); }
        return Cell(Notes.join(QStringLiteral("; ")));
    }

    QString LuaSignatureOf(const ScriptBindingEntry& InEntry)
    {
        const QString Name = QString::fromLatin1(InEntry.Name);
        if(InEntry.bMethod)
        {
            QString Signature = QString::fromLatin1(InEntry.Signature);
            QString Returns;
            if(const qsizetype Arrow = Signature.indexOf(QStringLiteral("->")); Arrow >= 0)
            {
                Returns   = Signature.mid(Arrow + 2).trimmed();
                Signature = Signature.left(Arrow).trimmed();
            }
            QString Line = QStringLiteral("`c:%1(%2)`").arg(Name, Signature);
            if(!Returns.isEmpty()) { Line += QStringLiteral(" -> %1").arg(Returns); }
            if(InEntry.bPure)      { Line += QStringLiteral(" *(no effects)*"); }
            return Line;
        }
        const QString Type = InEntry.FieldType == EScriptFieldType::Struct
            ? QString::fromLatin1(GetScriptStructName(InEntry.FieldStruct))
            : QString::fromLatin1(LuaTypeOf(InEntry.FieldType));
        QString Line = QStringLiteral("`c:Get%1()` -> %2").arg(Name, Type);
        if(!InEntry.bReadOnly) { Line += QStringLiteral("<br>`c:Set%1(%2)`").arg(Name, Type); }
        return Line;
    }
}

namespace EditorDocs
{
    QString GetDocsRoot()
    {
        if(!RootOverride().isEmpty()) { return RootOverride(); }
        const QString Engine = EditorProject::GetEngineDir();
        return Engine.isEmpty() ? QString() : QDir::cleanPath(QDir(Engine).filePath(QStringLiteral("Docs")));
    }

    void SetDocsRoot(const QString& InRoot)
    {
        RootOverride() = InRoot.isEmpty() ? QString() : QDir::cleanPath(InRoot);
    }

    QString GetComponentDocPath(const QString& InTypeName)
    {
        const QString Root = GetDocsRoot();
        const QString Name = Squashed(InTypeName);
        if(Root.isEmpty() || Name.isEmpty()) { return QString(); }
        return QDir(Root).filePath(QStringLiteral("Components/%1.md").arg(Name));
    }

    QString GenerateComponentMarkdown(const ComponentTypeID InType)
    {
        const ComponentTypeInfo* Info = GetComponentType(InType);
        if(!Info) { return QString(); }

        const QString Name   = TypeNameOf(*Info);
        const QString Class  = ClassOf(Name);
        const FHeader Found  = HeaderOf(Class);
        const QString Header = Found.Path;
        const QString Qualified = Found.Namespace.isEmpty() ? Class : Found.Namespace + QStringLiteral("::") + Class;

        size_t PropertyCount = 0;
        const ComponentPropertyEntry* Properties = FindComponentProperties(InType, PropertyCount);
        std::vector<uint8_t> Defaults(Info->SizeBytes);
        if(!GetComponentPropertyDefaults(InType, Defaults.data())) { Defaults.clear(); }

        size_t BindingCount = 0;
        const ScriptBindingEntry* Bindings = FindComponentScript(InType, BindingCount);

        QString Out;
        const auto Line = [&Out](const QString& InText = QString()) { Out += InText + QLatin1Char('\n'); };

        Line(QStringLiteral("# %1").arg(Name));
        Line();
        Line(QStringLiteral("Reference for the **%1** component: its C++ class and its Lua API. Written by the editor from the "
                            "component's registration; edit it freely, it is not overwritten.").arg(Name));
        Line();
        Line(QStringLiteral("| | |"));
        Line(QStringLiteral("|---|---|"));
        Line(QStringLiteral("| Name | `%1` (what `GetComponent` / `AddComponent` take in Lua, and what a save stores) |").arg(Name));
        Line(QStringLiteral("| C++ class | `%1` |").arg(Qualified));
        if(!Header.isEmpty()) { Line(QStringLiteral("| Header | `#include \"%1\"` |").arg(Header)); }
        Line(QStringLiteral("| Size | %1 bytes |").arg(Info->SizeBytes));
        const QString Category = QString::fromLatin1(GetComponentTypeCategory(InType));
        Line(QStringLiteral("| Add Component | %1 |").arg(!IsComponentAddable(InType) ? QStringLiteral("no (added and owned by a system)")
                                                       : Category.isEmpty()           ? QStringLiteral("yes, under Other")
                                                       : QStringLiteral("yes, under %1").arg(Category)));
        Line();

        // --- Properties --------------------------------------------------------
        Line(QStringLiteral("## Properties"));
        Line();
        if(PropertyCount == 0)
        {
            Line(QStringLiteral("The editor lists no properties for this component."));
            Line();
        }
        else
        {
            Line(QStringLiteral("What the Details panel shows. The C++ members are public fields of `%1`.").arg(Class));
            Line();

            QString Category;
            bool bOpen = false;
            const auto Open = [&](const QString& InCategory)
            {
                if(bOpen && Category == InCategory) { return; }
                if(bOpen) { Line(); }
                if(!InCategory.isEmpty()) { Line(QStringLiteral("### %1").arg(InCategory)); Line(); }
                Line(QStringLiteral("| Property | Type | Default | Notes |"));
                Line(QStringLiteral("|---|---|---|---|"));
                Category = InCategory;
                bOpen    = true;
            };
            for(size_t Index = 0; Index < PropertyCount; ++Index)
            {
                const ComponentPropertyEntry& Entry = Properties[Index];
                if(Entry.IsMember()) { continue; }
                Open(QString::fromLatin1(Entry.Category));
                QString Type = CppTypeOf(Entry.Type);
                if(Entry.Type == EPropertyType::FixedString) { Type = QStringLiteral("string"); }
                if(Entry.IsStructArray()) { Type = QStringLiteral("struct[%1]").arg(Entry.ArrayCount); }
                Line(QStringLiteral("| `%1` | %2 | %3 | %4 |").arg(QString::fromLatin1(Entry.Name), Cell(Type),
                                                                    Cell(DefaultOf(Entry, Defaults)), NotesOf(Entry)));
                if(Entry.IsStructArray())
                {
                    for(size_t Member = 0; Member < PropertyCount; ++Member)
                    {
                        if(Properties[Member].Parent != Index) { continue; }
                        const ComponentPropertyEntry& Each = Properties[Member];
                        Line(QStringLiteral("| `%1[].%2` | %3 | | %4 |").arg(QString::fromLatin1(Entry.Name), QString::fromLatin1(Each.Name),
                                                                              Cell(CppTypeOf(Each.Type)), NotesOf(Each)));
                    }
                }
            }
            Line();
        }

        // --- C++ ---------------------------------------------------------------
        Line(QStringLiteral("## C++ API"));
        Line();
        Line(QStringLiteral("```cpp"));
        if(!Header.isEmpty()) { Line(QStringLiteral("#include \"%1\"").arg(Header)); Line(); }
        Line(QStringLiteral("GameCore::Entity Owner = World.GetEntity(Id);"));
        Line();
        Line(QStringLiteral("auto* C = Owner.GetComponent<%1>();        // nullptr if absent").arg(Qualified));
        if(IsComponentAddable(InType))
        {
            Line(QStringLiteral("Owner.AddComponent<%1>();                 // defaults; or pass an initialised one").arg(Qualified));
        }
        Line(QStringLiteral("Owner.HasComponent<%1>();").arg(Qualified));
        Line(QStringLiteral("Owner.RemoveComponent<%1>();").arg(Qualified));
        Line(QStringLiteral("```"));
        Line();
        Line(QStringLiteral("The component is plain data (trivially copyable): set its public fields directly. "
                            "Its methods, and any field not listed above, are in the header."));
        Line();

        // --- Lua ---------------------------------------------------------------
        Line(QStringLiteral("## Lua API"));
        Line();
        Line(QStringLiteral("```lua"));
        Line(QStringLiteral("local c = entity:GetComponent(\"%1\")      -- nil if absent").arg(Name));
        if(IsComponentAddable(InType)) { Line(QStringLiteral("local c = entity:AddComponent(\"%1\")").arg(Name)); }
        Line(QStringLiteral("entity:HasComponent(\"%1\")").arg(Name));
        Line(QStringLiteral("entity:RemoveComponent(\"%1\")").arg(Name));
        Line(QStringLiteral("```"));
        Line();
        Line(QStringLiteral("`c` is a proxy: it holds the entity, not a pointer, so a call after `RemoveComponent` is a Lua error."));
        Line();
        if(BindingCount == 0)
        {
            Line(QStringLiteral("Scripts can reach nothing of this component beyond `c:GetEntity()`: it lists no bindings."));
            Line();
        }
        else
        {
            Line(QStringLiteral("| Call | Description |"));
            Line(QStringLiteral("|---|---|"));
            Line(QStringLiteral("| `c:GetEntity()` -> Entity | The owner. |"));
            for(size_t Index = 0; Index < BindingCount; ++Index)
            {
                const ScriptBindingEntry& Entry = Bindings[Index];
                const char* Kind = Entry.bMethod ? "Method." : (Entry.bReadOnly ? "Read-only property." : "Property.");
                Line(QStringLiteral("| %1 | %2 |").arg(Cell(LuaSignatureOf(Entry)), QString::fromLatin1(Kind)));
            }
            Line();
        }
        Line(QStringLiteral("Types: `Vector3` is three numbers, `Color` four integers 0..255, `Entity` an entity value."));
        return Out;
    }

    QString EnsureComponentDoc(const ComponentTypeID InType)
    {
        const ComponentTypeInfo* Info = GetComponentType(InType);
        if(!Info) { return QString(); }

        const QString Path = GetComponentDocPath(TypeNameOf(*Info));
        if(Path.isEmpty()) { return QString(); }
        if(QFileInfo(Path).isFile()) { return Path; }

        const QString Markdown = GenerateComponentMarkdown(InType);
        if(Markdown.isEmpty() || !QDir().mkpath(QFileInfo(Path).absolutePath())) { return QString(); }

        QFile File(Path);
        if(!File.open(QIODevice::WriteOnly | QIODevice::Truncate)) { return QString(); }
        File.write(Markdown.toUtf8());
        return Path;
    }

    int GenerateMissingComponentDocs()
    {
        int Written = 0;
        for(ComponentTypeID Type = 0; Type < ComponentTypeID(GetComponentTypeCount()); ++Type)
        {
            const ComponentTypeInfo* Info = GetComponentType(Type);
            size_t Count = 0;
            if(!Info || !FindComponentProperties(Type, Count)) { continue; }

            const QString Path = GetComponentDocPath(TypeNameOf(*Info));
            if(Path.isEmpty() || QFileInfo(Path).isFile()) { continue; }
            if(!EnsureComponentDoc(Type).isEmpty()) { ++Written; }
        }
        return Written;
    }
}

#endif // WITH_EDITOR
