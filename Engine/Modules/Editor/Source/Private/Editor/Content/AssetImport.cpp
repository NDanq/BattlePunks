#if WITH_EDITOR

#include "Private/Editor/Content/AssetImport.h"

#include <algorithm>
#include <cstring>
#include <vector>

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
            Out << QStringLiteral("hdr");   // Radiance, read here (ReadRadiance): an HDRI, imported as RGBE8
            Out.removeDuplicates();
            return Out;
        }

        bool WriteArchive(const QString& InPath, const EngineCore::Archive& InArchive, QString& OutError);

        /*
         * A Radiance .hdr (RGBE, "-Y height +X width", flat or run-length
         * scanlines) as RGBE8 texels, row 0 the top. False, with OutError,
         * on anything else.
         */
        bool ReadRadiance(const QString& InPath, int& OutWidth, int& OutHeight, std::vector<uint8_t>& OutTexels, QString& OutError)
        {
            QFile File(InPath);
            if(!File.open(QIODevice::ReadOnly)) { OutError = File.errorString(); return false; }
            const QByteArray Bytes = File.readAll();
            const auto* Data = reinterpret_cast<const uint8_t*>(Bytes.constData());
            const qsizetype Size = Bytes.size();
            qsizetype At = 0;
            const auto Line = [&]()
            {
                QByteArray Out;
                while(At < Size && Data[At] != '\n') { Out += char(Data[At++]); }
                ++At;
                return Out;
            };
            const QByteArray Magic = Line();
            if(!Magic.startsWith("#?")) { OutError = QStringLiteral("not a Radiance file"); return false; }
            for(QByteArray Each = Line(); !Each.isEmpty(); Each = Line())
            {
                if(Each.startsWith("FORMAT=") && Each != "FORMAT=32-bit_rle_rgbe")
                {
                    OutError = QStringLiteral("only RGBE Radiance files are read (this is %1)").arg(QString::fromLatin1(Each));
                    return false;
                }
                if(At >= Size) { OutError = QStringLiteral("no image after the header"); return false; }
            }
            const QList<QByteArray> Resolution = Line().simplified().split(' ');
            if(Resolution.size() != 4 || Resolution[0] != "-Y" || Resolution[2] != "+X")
            {
                OutError = QStringLiteral("only the standard -Y H +X W orientation is read");
                return false;
            }
            OutHeight = Resolution[1].toInt();
            OutWidth  = Resolution[3].toInt();
            if(OutWidth <= 0 || OutHeight <= 0 || OutWidth > 32768 || OutHeight > 32768)
            {
                OutError = QStringLiteral("an implausible size");
                return false;
            }
            OutTexels.assign(size_t(OutWidth) * size_t(OutHeight) * 4, 0);
            std::vector<uint8_t> Planes(size_t(OutWidth) * 4);
            for(int Row = 0; Row < OutHeight; ++Row)
            {
                uint8_t* Out = &OutTexels[size_t(Row) * size_t(OutWidth) * 4];
                const bool bRle = OutWidth >= 8 && OutWidth < 32768 && At + 4 <= Size && Data[At] == 2 && Data[At + 1] == 2
                                  && (Data[At + 2] & 0x80) == 0 && ((int(Data[At + 2]) << 8) | Data[At + 3]) == OutWidth;
                if(!bRle)
                {
                    /* Flat (old run-length scanlines are rare and not read). */
                    if(At + qsizetype(OutWidth) * 4 > Size) { OutError = QStringLiteral("the image ends early"); return false; }
                    std::memcpy(Out, Data + At, size_t(OutWidth) * 4);
                    At += qsizetype(OutWidth) * 4;
                    continue;
                }
                At += 4;
                for(int Channel = 0; Channel < 4; ++Channel)
                {
                    uint8_t* Plane = &Planes[size_t(Channel) * size_t(OutWidth)];
                    int X = 0;
                    while(X < OutWidth)
                    {
                        if(At >= Size) { OutError = QStringLiteral("the image ends early"); return false; }
                        int Count = Data[At++];
                        if(Count > 128)
                        {
                            Count -= 128;
                            if(At >= Size || X + Count > OutWidth) { OutError = QStringLiteral("a bad run"); return false; }
                            std::memset(Plane + X, Data[At++], size_t(Count));
                        }
                        else
                        {
                            if(Count == 0 || X + Count > OutWidth || At + Count > Size) { OutError = QStringLiteral("a bad run"); return false; }
                            std::memcpy(Plane + X, Data + At, size_t(Count));
                            At += Count;
                        }
                        X += Count;
                    }
                }
                for(int X = 0; X < OutWidth; ++X)
                {
                    for(int Channel = 0; Channel < 4; ++Channel) { Out[X * 4 + Channel] = Planes[size_t(Channel) * size_t(OutWidth) + size_t(X)]; }
                }
            }
            return true;
        }

        /* An HDRI into InPath: RGBE8, linear, point sampled, no mips; halved (in float) until it fits MaxDimension. */
        bool WriteRadianceTexture(const QString& InSource, const QString& InPath, const TextureImportOptions& InOptions,
                                  const RenderCore::AssetImportInfo& InInfo, QStringList& OutWarnings, QString& OutError)
        {
            int Width = 0, Height = 0;
            std::vector<uint8_t> Texels;
            QString Why;
            if(!ReadRadiance(InSource, Width, Height, Texels, Why))
            {
                OutError = QStringLiteral("%1: %2").arg(QFileInfo(InSource).fileName(), Why);
                return false;
            }
            const int Limit = qBound(1, InOptions.MaxDimension, int(RenderCore::TEXTURE_MAX_DIMENSION));
            if(Width > Limit || Height > Limit)
            {
                const int FromW = Width, FromH = Height;
                while(Width > Limit || Height > Limit)
                {
                    const int HalfW = std::max(Width / 2, 1), HalfH = std::max(Height / 2, 1);
                    std::vector<uint8_t> Half(size_t(HalfW) * size_t(HalfH) * 4);
                    for(int Y = 0; Y < HalfH; ++Y)
                    {
                        for(int X = 0; X < HalfW; ++X)
                        {
                            float Sum[3] = { 0.0f, 0.0f, 0.0f };
                            for(int Dy = 0; Dy < 2; ++Dy)
                            {
                                for(int Dx = 0; Dx < 2; ++Dx)
                                {
                                    const int Sx = std::min(X * 2 + Dx, Width - 1), Sy = std::min(Y * 2 + Dy, Height - 1);
                                    float Rgb[3];
                                    RenderCore::DecodeRgbe(&Texels[(size_t(Sy) * size_t(Width) + size_t(Sx)) * 4], Rgb);
                                    for(int C = 0; C < 3; ++C) { Sum[C] += Rgb[C] * 0.25f; }
                                }
                            }
                            RenderCore::EncodeRgbe(Sum, &Half[(size_t(Y) * size_t(HalfW) + size_t(X)) * 4]);
                        }
                    }
                    Texels = std::move(Half);
                    Width  = HalfW;
                    Height = HalfH;
                }
                OutWarnings << QStringLiteral("%1: scaled from %2x%3 to %4x%5").arg(QFileInfo(InSource).fileName())
                               .arg(FromW).arg(FromH).arg(Width).arg(Height);
            }

            RenderCore::TextureData Texture;
            Texture.Pixels = EngineCore::Image(Width, Height, EngineCore::EImageFormat::RGBA8);
            std::memcpy(Texture.Pixels.GetData(), Texels.data(), Texels.size());
            if(InOptions.bFlipVertically) { Texture.Pixels.FlipVertical(); }
            Texture.Format     = RenderCore::ETexturePixelFormat::RGBE8;
            Texture.bSRGB      = false;
            Texture.Filter     = RenderCore::ETextureFilter::Nearest;
            Texture.Wrap       = RenderCore::ETextureWrap::Repeat;
            Texture.MipGen     = RenderCore::ETextureMipGen::None;
            Texture.ImportInfo = InInfo;
            EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
            RenderCore::SerializeTexture(Ar, Texture);
            return WriteArchive(InPath, Ar, OutError);
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
            Texture.MipGen      = InOptions.MipGen;
            Texture.Compression = InOptions.Compression;
            Texture.ImportInfo = InInfo;
            if(Texture.Pixels.IsNull())
            {
                OutError = QStringLiteral("%1: the image could not be converted").arg(QFileInfo(InPath).fileName());
                return false;
            }

            RenderCore::BuildTextureMips(Texture);

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

        /* Read directly, not via the file provider (rooted at the exe). */
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
        return QStringLiteral("SRGB=%1\nFilter=%2\nWrap=%3\nMaxDimension=%4\nFlipVertically=%5\nMipGen=%6\nCompression=%7\nDetectDataMaps=%8\n")
               .arg(int(bSRGB)).arg(int(Filter)).arg(int(Wrap)).arg(MaxDimension).arg(int(bFlipVertically))
               .arg(int(MipGen)).arg(int(Compression)).arg(int(bDetectDataMaps));
    }

    TextureImportOptions TextureImportOptions::FromSettings(const QString& InSettings)
    {
        const QMap<QString, QString> Map = ParseSettings(InSettings);
        TextureImportOptions Out;
        Out.bSRGB           = ReadBool(Map, QStringLiteral("SRGB"), Out.bSRGB);
        Out.bFlipVertically = ReadBool(Map, QStringLiteral("FlipVertically"), Out.bFlipVertically);
        Out.bDetectDataMaps = ReadBool(Map, QStringLiteral("DetectDataMaps"), false);   // absent: settings from before, as recorded
        const int Filter = Map.value(QStringLiteral("Filter"), QString::number(int(Out.Filter))).toInt();
        const int Wrap   = Map.value(QStringLiteral("Wrap"),   QString::number(int(Out.Wrap))).toInt();
        if(Filter >= 0 && Filter <= int(RenderCore::ETextureFilter::Linear)) { Out.Filter = RenderCore::ETextureFilter(Filter); }
        if(Wrap   >= 0 && Wrap   <= int(RenderCore::ETextureWrap::Mirror))   { Out.Wrap   = RenderCore::ETextureWrap(Wrap); }
        const int MaxDimension = Map.value(QStringLiteral("MaxDimension")).toInt();
        if(MaxDimension > 0) { Out.MaxDimension = MaxDimension; }
        /* Absent from a texture imported before mips: none, as it was. */
        const int MipGen      = Map.value(QStringLiteral("MipGen"), QStringLiteral("0")).toInt();
        const int Compression = Map.value(QStringLiteral("Compression"), QStringLiteral("0")).toInt();
        Out.MipGen      = MipGen >= 0 && MipGen <= int(RenderCore::ETextureMipGen::Kaiser) ? RenderCore::ETextureMipGen(MipGen)
                                                                                        : RenderCore::ETextureMipGen::None;
        Out.Compression = Compression >= 0 && Compression <= int(RenderCore::ETextureCompression::BC7)
                          ? RenderCore::ETextureCompression(Compression) : RenderCore::ETextureCompression::None;
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

    bool TextureImportOptions::IsDataMapName(const QString& InName)
    {
        /* Whole words of the name (split at _, -, ., space and case changes), so "Bricks" is not "ks". */
        static const QStringList Words = {
            QStringLiteral("normal"), QStringLiteral("normals"), QStringLiteral("nrm"), QStringLiteral("nor"), QStringLiteral("n"),
            QStringLiteral("rough"), QStringLiteral("roughness"), QStringLiteral("metal"), QStringLiteral("metallic"),
            QStringLiteral("metalness"), QStringLiteral("ao"), QStringLiteral("occlusion"), QStringLiteral("ambientocclusion"),
            QStringLiteral("height"), QStringLiteral("displacement"), QStringLiteral("disp"), QStringLiteral("bump"),
            QStringLiteral("mask"), QStringLiteral("orm"), QStringLiteral("arm"), QStringLiteral("gloss"), QStringLiteral("specular") };
        QString Spaced;
        const QString Base = QFileInfo(InName).completeBaseName();
        for(int Index = 0; Index < Base.size(); ++Index)
        {
            const QChar Ch = Base[Index];
            if(Index > 0 && Ch.isUpper() && Base[Index - 1].isLower()) { Spaced += QLatin1Char(' '); }
            Spaced += Ch.isLetterOrNumber() ? Ch.toLower() : QLatin1Char(' ');
        }
        for(const QString& Word : Spaced.split(QLatin1Char(' '), Qt::SkipEmptyParts))
        {
            if(Words.contains(Word)) { return true; }
        }
        return false;
    }

    TextureImportOptions TextureImportOptions::ResolvedFor(const QString& InName) const
    {
        TextureImportOptions Out = *this;
        if(bDetectDataMaps && IsDataMapName(InName)) { Out.bSRGB = false; }
        Out.bDetectDataMaps = false;   // decided: a reimport keeps it
        return Out;
    }

    ImportResult ImportTexture(const QString& InSource, const QString& InDestFolder, const QString& InBaseName,
                               const TextureImportOptions& InRequested, const QString& InProjectRoot)
    {
        const TextureImportOptions InOptions = InRequested.ResolvedFor(InBaseName + QLatin1Char('_') + QFileInfo(InSource).fileName());
        ImportResult Result;
        if(QFileInfo(InSource).suffix().compare(QStringLiteral("hdr"), Qt::CaseInsensitive) == 0)
        {
            const QString HdrPath = QDir(InDestFolder).absoluteFilePath(SanitizeName(InBaseName) + QString::fromLatin1(TEXTURE_EXTENSION));
            QString HdrError;
            if(!WriteRadianceTexture(InSource, HdrPath, InOptions, MakeImportInfo(InSource, InProjectRoot, InOptions.ToSettings()),
                                     Result.Warnings, HdrError))
            {
                Result.Message = HdrError;
                return Result;
            }
            Result.bOK = true;
            Result.Written << HdrPath;
            Result.Message = QStringLiteral("%1: HDR environment (RGBE)").arg(QFileInfo(InSource).fileName());
            return Result;
        }
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
                                       const TextureImportOptions& InRequested, const QString& InProjectRoot)
    {
        if(!InImage.SourcePath.isEmpty())
        {
            return ImportTexture(InImage.SourcePath, InDestFolder, InBaseName, InRequested, InProjectRoot);
        }
        const TextureImportOptions InOptions = InRequested.ResolvedFor(InBaseName + QLatin1Char('_') + InImage.Name);

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

        /* Same options, over this one file (a split mesh finds its part
         * again). Textures reimport separately. */
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
