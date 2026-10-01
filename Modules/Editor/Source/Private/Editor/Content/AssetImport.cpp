#if WITH_EDITOR

#include "Private/Editor/Content/AssetImport.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>

#include "CoreQtConversions.h"
#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Private/Editor/Content/GltfImporter.h"

namespace EditorContent
{
    namespace
    {
        const char* const MESH_EXTENSION    = ".nmmesh";
        const char* const TEXTURE_EXTENSION = ".nmtexture";

        /* Where an embedded image's source points: the glTF, then which image. */
        const char* const EMBEDDED_IMAGE_MARK = "#image";

        QStringList MeshSuffixes() { return { QStringLiteral("gltf"), QStringLiteral("glb") }; }

        QStringList ImageSuffixes()
        {
            QStringList Out;
            for(const QByteArray& Format : QImageReader::supportedImageFormats())
            {
                const QString Suffix = QString::fromLatin1(Format).toLower();
                /* Formats that are not pictures, or that no one textures with. */
                if(Suffix == QStringLiteral("svg") || Suffix == QStringLiteral("svgz") || Suffix == QStringLiteral("pdf")) { continue; }
                Out << Suffix;
            }
            Out.removeDuplicates();
            return Out;
        }

        /* Settings as "Key=Value" lines, read back into a map. */
        QMap<QString, QString> ParseSettings(const QString& InSettings)
        {
            QMap<QString, QString> Out;
            for(const QString& Line : InSettings.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            {
                const qsizetype Equals = Line.indexOf(QLatin1Char('='));
                if(Equals > 0) { Out.insert(Line.left(Equals).trimmed(), Line.mid(Equals + 1).trimmed()); }
            }
            return Out;
        }

        bool ReadBool(const QMap<QString, QString>& InSettings, const QString& InKey, const bool InDefault)
        {
            const auto Found = InSettings.find(InKey);
            return Found == InSettings.end() ? InDefault : (Found.value() == QStringLiteral("1"));
        }

        /* A file name from a mesh's or image's name: what Windows allows, no more. */
        QString SanitizeName(const QString& InName)
        {
            static const QRegularExpression Invalid(QStringLiteral("[^A-Za-z0-9_\\-. ]"));
            QString Out = InName.trimmed();
            Out.replace(Invalid, QStringLiteral("_"));
            return Out.isEmpty() ? QStringLiteral("Asset") : Out;
        }

        /* InSource relative to the project when it is inside it, forward slashes. */
        RenderCore::AssetImportInfo MakeImportInfo(const QString& InSource, const QString& InProjectRoot, const QString& InSettings)
        {
            const QFileInfo Source(InSource.section(QString::fromLatin1(EMBEDDED_IMAGE_MARK), 0, 0));
            QString Path = QFileInfo(InSource).isAbsolute() ? InSource : Source.absoluteFilePath();
            if(!InProjectRoot.isEmpty())
            {
                const QString Relative = QDir(InProjectRoot).relativeFilePath(Path);
                if(!Relative.startsWith(QStringLiteral(".."))) { Path = Relative; }
            }

            RenderCore::AssetImportInfo Info;
            Info.SourcePath = QDir::fromNativeSeparators(Path).toStdString();
            Info.SourceTime = Source.lastModified().toSecsSinceEpoch();
            Info.Settings   = InSettings.toStdString();
            return Info;
        }

        /* An ImportInfo's source back to an absolute path. */
        QString ResolveSource(const RenderCore::AssetImportInfo& InInfo, const QString& InProjectRoot)
        {
            const QString Path = QString::fromStdString(InInfo.SourcePath);
            return QDir::isAbsolutePath(Path) ? Path : QDir(InProjectRoot).absoluteFilePath(Path);
        }

        /* Serialized whole in memory, then committed: a failure leaves the
         * old file. Creates the folder. */
        bool WriteArchive(const QString& InPath, const EngineCore::Archive& InArchive, QString& OutError)
        {
            if(InArchive.HasError())
            {
                OutError = QStringLiteral("%1: could not be serialized -- see the log").arg(QFileInfo(InPath).fileName());
                return false;
            }
            QDir().mkpath(QFileInfo(InPath).absolutePath());
            QSaveFile File(InPath);
            if(!File.open(QIODevice::WriteOnly))
            {
                OutError = QStringLiteral("%1: %2").arg(InPath, File.errorString());
                return false;
            }
            const std::vector<uint8_t>& Bytes = InArchive.GetBytes();
            File.write(reinterpret_cast<const char*>(Bytes.data()), qint64(Bytes.size()));
            if(!File.commit())
            {
                OutError = QStringLiteral("%1: %2").arg(InPath, File.errorString());
                return false;
            }
            return true;
        }

        bool WriteMesh(const QString& InPath, RenderCore::MeshData& InMesh, QString& OutError)
        {
            EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
            RenderCore::SerializeMesh(Ar, InMesh);
            return WriteArchive(InPath, Ar, OutError);
        }

        /* InImage, decoded, into a texture as InOptions say, written to InPath. */
        bool WriteTexture(const QString& InPath, QImage InImage, const TextureImportOptions& InOptions,
                          const RenderCore::AssetImportInfo& InInfo, QStringList& OutWarnings, QString& OutError)
        {
            if(InImage.isNull())
            {
                OutError = QStringLiteral("%1: the image is empty").arg(QFileInfo(InPath).fileName());
                return false;
            }

            const int Limit = qBound(1, InOptions.MaxDimension, int(RenderCore::TEXTURE_MAX_DIMENSION));
            if(InImage.width() > Limit || InImage.height() > Limit)
            {
                const QSize Before = InImage.size();
                InImage = InImage.scaled(Limit, Limit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                OutWarnings << QStringLiteral("%1: scaled from %2x%3 to %4x%5")
                               .arg(QFileInfo(InPath).fileName()).arg(Before.width()).arg(Before.height())
                               .arg(InImage.width()).arg(InImage.height());
            }
            if(InOptions.bFlipVertically)
            {
                InImage = InImage.mirrored(false, true);
            }

            RenderCore::TextureData Texture;
            Texture.Pixels     = EngineCore::QtBridge::FromQImage(InImage.convertToFormat(QImage::Format_RGBA8888));
            Texture.bSRGB      = InOptions.bSRGB;
            Texture.Filter     = InOptions.Filter;
            Texture.Wrap       = InOptions.Wrap;
            Texture.ImportInfo = InInfo;
            if(Texture.Pixels.IsNull())
            {
                OutError = QStringLiteral("%1: the image could not be converted").arg(QFileInfo(InPath).fileName());
                return false;
            }

            EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
            RenderCore::SerializeTexture(Ar, Texture);
            return WriteArchive(InPath, Ar, OutError);
        }

        QImage ReadImage(const QString& InPath, QString& OutError)
        {
            QImageReader Reader(InPath);
            Reader.setAutoTransform(true);   // honour a photo's EXIF orientation
            const QImage Image = Reader.read();
            if(Image.isNull())
            {
                OutError = QStringLiteral("%1: %2").arg(QFileInfo(InPath).fileName(), Reader.errorString());
            }
            return Image;
        }

        /* An existing asset's import record, read straight from the file --
         * not through the file provider, which is rooted at the executable. */
        bool ReadImportInfo(const QString& InAssetPath, EImportKind& OutKind, RenderCore::AssetImportInfo& OutInfo, QString& OutError)
        {
            QFile File(InAssetPath);
            if(!File.open(QIODevice::ReadOnly))
            {
                OutError = QStringLiteral("%1: %2").arg(InAssetPath, File.errorString());
                return false;
            }
            const QByteArray Bytes = File.readAll();
            EngineCore::Archive Ar = EngineCore::Archive::ForReading(std::vector<uint8_t>(Bytes.begin(), Bytes.end()));

            bool bRead = false;
            if(InAssetPath.endsWith(QString::fromLatin1(MESH_EXTENSION), Qt::CaseInsensitive))
            {
                RenderCore::MeshData Mesh;
                bRead   = RenderCore::SerializeMesh(Ar, Mesh);
                OutInfo = Mesh.ImportInfo;
                OutKind = EImportKind::Mesh;
            }
            else if(InAssetPath.endsWith(QString::fromLatin1(TEXTURE_EXTENSION), Qt::CaseInsensitive))
            {
                RenderCore::TextureData Texture;
                bRead   = RenderCore::SerializeTexture(Ar, Texture);
                OutInfo = Texture.ImportInfo;
                OutKind = EImportKind::Texture;
            }
            if(!bRead)
            {
                OutError = QStringLiteral("%1: not a mesh or texture this editor can read").arg(QFileInfo(InAssetPath).fileName());
                return false;
            }
            if(OutInfo.IsEmpty())
            {
                OutError = QStringLiteral("%1 records no source to import it from").arg(QFileInfo(InAssetPath).fileName());
                return false;
            }
            return true;
        }

        void Merge(ImportResult& InOutInto, const ImportResult& InFrom)
        {
            InOutInto.Written  << InFrom.Written;
            InOutInto.Warnings << InFrom.Warnings;
            if(!InFrom.bOK) { InOutInto.Warnings << InFrom.Message; }
        }
    }

    // === Kinds and filters ============================================================

    EImportKind ClassifySource(const QString& InPath)
    {
        const QString Suffix = QFileInfo(InPath).suffix().toLower();
        if(MeshSuffixes().contains(Suffix))  { return EImportKind::Mesh; }
        if(ImageSuffixes().contains(Suffix)) { return EImportKind::Texture; }
        return EImportKind::None;
    }

    QString GetImportFileFilter()
    {
        const auto Patterns = [](const QStringList& InSuffixes)
        {
            QStringList Out;
            for(const QString& Suffix : InSuffixes) { Out << QStringLiteral("*.%1").arg(Suffix); }
            return Out.join(QLatin1Char(' '));
        };
        return QStringLiteral("Importable Files (%1 %2);;glTF Meshes (%1);;Images (%2);;All Files (*)")
               .arg(Patterns(MeshSuffixes()), Patterns(ImageSuffixes()));
    }

    QString GetAssetExtension(const EImportKind InKind)
    {
        switch(InKind)
        {
            case EImportKind::Mesh:    return QString::fromLatin1(MESH_EXTENSION);
            case EImportKind::Texture: return QString::fromLatin1(TEXTURE_EXTENSION);
            case EImportKind::None:    break;
        }
        return QString();
    }

    // === Options ========================================================================

    QString MeshImportOptions::ToSettings() const
    {
        return QStringLiteral("Scale=%1\nConvertCoordinates=%2\nGenerateNormals=%3\nCombineMeshes=%4\nImportTextures=%5\n")
               .arg(double(Scale)).arg(int(bConvertCoordinates)).arg(int(bGenerateNormals))
               .arg(int(bCombineMeshes)).arg(int(bImportTextures));
    }

    MeshImportOptions MeshImportOptions::FromSettings(const QString& InSettings)
    {
        const QMap<QString, QString> Map = ParseSettings(InSettings);
        MeshImportOptions Out;
        bool bScaleOK = false;
        const float Scale = Map.value(QStringLiteral("Scale")).toFloat(&bScaleOK);
        if(bScaleOK && Scale > 0.0f) { Out.Scale = Scale; }
        Out.bConvertCoordinates = ReadBool(Map, QStringLiteral("ConvertCoordinates"), Out.bConvertCoordinates);
        Out.bGenerateNormals    = ReadBool(Map, QStringLiteral("GenerateNormals"), Out.bGenerateNormals);
        Out.bCombineMeshes      = ReadBool(Map, QStringLiteral("CombineMeshes"), Out.bCombineMeshes);
        Out.bImportTextures     = ReadBool(Map, QStringLiteral("ImportTextures"), Out.bImportTextures);
        return Out;
    }

    QString TextureImportOptions::ToSettings() const
    {
        return QStringLiteral("SRGB=%1\nFilter=%2\nWrap=%3\nMaxDimension=%4\nFlipVertically=%5\n")
               .arg(int(bSRGB)).arg(int(Filter)).arg(int(Wrap)).arg(MaxDimension).arg(int(bFlipVertically));
    }

    TextureImportOptions TextureImportOptions::FromSettings(const QString& InSettings)
    {
        const QMap<QString, QString> Map = ParseSettings(InSettings);
        TextureImportOptions Out;
        Out.bSRGB           = ReadBool(Map, QStringLiteral("SRGB"), Out.bSRGB);
        Out.bFlipVertically = ReadBool(Map, QStringLiteral("FlipVertically"), Out.bFlipVertically);
        const int Filter = Map.value(QStringLiteral("Filter"), QString::number(int(Out.Filter))).toInt();
        const int Wrap   = Map.value(QStringLiteral("Wrap"),   QString::number(int(Out.Wrap))).toInt();
        if(Filter >= 0 && Filter <= int(RenderCore::ETextureFilter::Linear)) { Out.Filter = RenderCore::ETextureFilter(Filter); }
        if(Wrap   >= 0 && Wrap   <= int(RenderCore::ETextureWrap::Mirror))   { Out.Wrap   = RenderCore::ETextureWrap(Wrap); }
        const int MaxDimension = Map.value(QStringLiteral("MaxDimension")).toInt();
        if(MaxDimension > 0) { Out.MaxDimension = MaxDimension; }
        return Out;
    }

    // === Importing ======================================================================

    bool FindReferencedImages(const QString& InGltfPath, QList<ReferencedImage>& OutImages, QString& OutError)
    {
        return LoadGltfImages(InGltfPath, OutImages, OutError);
    }

    ImportResult ImportMesh(const QString& InSource, const QString& InDestFolder, const QString& InBaseName,
                            const MeshImportOptions& InOptions, const TextureImportOptions& InTextureOptions,
                            const QString& InProjectRoot)
    {
        ImportResult Result;
        QList<GltfMesh> Meshes;
        QString Error;
        if(!LoadGltf(InSource, InOptions, Meshes, Result.Images, Result.Warnings, Error))
        {
            Result.Message = Error;
            return Result;
        }

        const QString Base = SanitizeName(InBaseName);
        uint64_t Triangles = 0;
        for(GltfMesh& Mesh : Meshes)
        {
            /* A split mesh records which one it is, for Reimport to find again. */
            QString Settings = InOptions.ToSettings();
            QString FileName = Base;
            if(!InOptions.bCombineMeshes)
            {
                Settings += QStringLiteral("Mesh=%1\n").arg(Mesh.Name);
                FileName += QStringLiteral("_") + SanitizeName(Mesh.Name);
            }
            Mesh.Data.ImportInfo = MakeImportInfo(InSource, InProjectRoot, Settings);

            const QString OutPath = QDir(InDestFolder).absoluteFilePath(FileName + QString::fromLatin1(MESH_EXTENSION));
            if(!WriteMesh(OutPath, Mesh.Data, Error))
            {
                Result.Message = Error;
                return Result;
            }
            Result.Written << OutPath;
            Triangles += Mesh.Data.GetTriangleCount();
        }

        if(InOptions.bImportTextures)
        {
            for(const ReferencedImage& Image : Result.Images)
            {
                Merge(Result, ImportReferencedImage(Image, InSource, InDestFolder, Image.Name, InTextureOptions, InProjectRoot));
            }
        }

        Result.bOK = true;
        Result.Message = QStringLiteral("%1: %2 mesh file(s), %3 triangles")
                         .arg(QFileInfo(InSource).fileName()).arg(Meshes.size()).arg(Triangles);
        return Result;
    }

    ImportResult ImportTexture(const QString& InSource, const QString& InDestFolder, const QString& InBaseName,
                               const TextureImportOptions& InOptions, const QString& InProjectRoot)
    {
        ImportResult Result;
        QString Error;
        const QImage Image = ReadImage(InSource, Error);
        const QString OutPath = QDir(InDestFolder).absoluteFilePath(SanitizeName(InBaseName) + QString::fromLatin1(TEXTURE_EXTENSION));
        if(Image.isNull()
           || !WriteTexture(OutPath, Image, InOptions, MakeImportInfo(InSource, InProjectRoot, InOptions.ToSettings()),
                            Result.Warnings, Error))
        {
            Result.Message = Error;
            return Result;
        }
        Result.bOK = true;
        Result.Written << OutPath;
        Result.Message = QStringLiteral("%1: %2x%3 texture").arg(QFileInfo(InSource).fileName()).arg(Image.width()).arg(Image.height());
        return Result;
    }

    ImportResult ImportReferencedImage(const ReferencedImage& InImage, const QString& InGltfPath,
                                       const QString& InDestFolder, const QString& InBaseName,
                                       const TextureImportOptions& InOptions, const QString& InProjectRoot)
    {
        if(!InImage.SourcePath.isEmpty())
        {
            return ImportTexture(InImage.SourcePath, InDestFolder, InBaseName, InOptions, InProjectRoot);
        }

        ImportResult Result;
        QImage Image;
        Image.loadFromData(InImage.Embedded);
        const QString OutPath = QDir(InDestFolder).absoluteFilePath(SanitizeName(InBaseName) + QString::fromLatin1(TEXTURE_EXTENSION));
        const QString Source  = QFileInfo(InGltfPath).absoluteFilePath() + QString::fromLatin1(EMBEDDED_IMAGE_MARK)
                              + QString::number(InImage.Index);

        QString Error = QStringLiteral("%1: embedded image '%2' could not be decoded").arg(QFileInfo(InGltfPath).fileName(), InImage.Name);
        if(Image.isNull()
           || !WriteTexture(OutPath, Image, InOptions, MakeImportInfo(Source, InProjectRoot, InOptions.ToSettings()),
                            Result.Warnings, Error))
        {
            Result.Message = Error;
            return Result;
        }
        Result.bOK = true;
        Result.Written << OutPath;
        Result.Message = QStringLiteral("%1: %2x%3 texture").arg(InImage.Name).arg(Image.width()).arg(Image.height());
        return Result;
    }

    ImportResult Reimport(const QString& InAssetPath, const QString& InProjectRoot)
    {
        ImportResult Result;
        EImportKind Kind = EImportKind::None;
        RenderCore::AssetImportInfo Info;
        if(!ReadImportInfo(InAssetPath, Kind, Info, Result.Message))
        {
            return Result;
        }

        const QString Source   = ResolveSource(Info, InProjectRoot);
        const QString Settings = QString::fromStdString(Info.Settings);
        const QString Folder   = QFileInfo(InAssetPath).absolutePath();
        const QString Base     = QFileInfo(InAssetPath).completeBaseName();

        /* An image that lived inside a glTF: find it there again by index. */
        const qsizetype Mark = Source.lastIndexOf(QString::fromLatin1(EMBEDDED_IMAGE_MARK));
        if(Kind == EImportKind::Texture && Mark > 0)
        {
            const QString Gltf  = Source.left(Mark);
            const int     Index = Source.mid(Mark + qsizetype(std::strlen(EMBEDDED_IMAGE_MARK))).toInt();
            QList<ReferencedImage> Images;
            if(!QFileInfo::exists(Gltf) || !FindReferencedImages(Gltf, Images, Result.Message))
            {
                if(Result.Message.isEmpty()) { Result.Message = QStringLiteral("the source %1 is gone").arg(Gltf); }
                return Result;
            }
            for(const ReferencedImage& Image : Images)
            {
                if(Image.Index == Index)
                {
                    return ImportReferencedImage(Image, Gltf, Folder, Base, TextureImportOptions::FromSettings(Settings), InProjectRoot);
                }
            }
            Result.Message = QStringLiteral("%1 no longer has image %2").arg(QFileInfo(Gltf).fileName()).arg(Index);
            return Result;
        }

        if(!QFileInfo::exists(Source))
        {
            Result.Message = QStringLiteral("the source %1 is gone").arg(Source);
            return Result;
        }

        if(Kind == EImportKind::Texture)
        {
            return ImportTexture(Source, Folder, Base, TextureImportOptions::FromSettings(Settings), InProjectRoot);
        }

        /* A mesh: the same options, written back over this one file -- a
         * split mesh picks its own part out of the scene again. Textures are
         * reimported on their own. */
        MeshImportOptions Options = MeshImportOptions::FromSettings(Settings);
        Options.bImportTextures = false;
        const QString Part = ParseSettings(Settings).value(QStringLiteral("Mesh"));

        QList<GltfMesh> Meshes;
        QList<ReferencedImage> Images;
        if(!LoadGltf(Source, Options, Meshes, Images, Result.Warnings, Result.Message))
        {
            return Result;
        }
        GltfMesh* Chosen = Options.bCombineMeshes ? &Meshes.front() : nullptr;
        for(GltfMesh& Mesh : Meshes)
        {
            if(!Chosen && Mesh.Name == Part) { Chosen = &Mesh; }
        }
        if(!Chosen)
        {
            Result.Message = QStringLiteral("%1 no longer has a mesh named %2").arg(QFileInfo(Source).fileName(), Part);
            return Result;
        }

        Chosen->Data.ImportInfo = MakeImportInfo(Source, InProjectRoot, Settings);
        if(!WriteMesh(InAssetPath, Chosen->Data, Result.Message))
        {
            return Result;
        }
        Result.bOK = true;
        Result.Written << QFileInfo(InAssetPath).absoluteFilePath();
        Result.Message = QStringLiteral("%1: reimported, %2 triangles").arg(QFileInfo(InAssetPath).fileName())
                         .arg(Chosen->Data.GetTriangleCount());
        return Result;
    }
}

#endif // WITH_EDITOR
