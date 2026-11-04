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
#include "Public/RenderCore/AnimClipAsset.h"
#include "Public/RenderCore/AnimRetarget.h"
#include "Public/RenderCore/HumanoidRig.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RetargetAsset.h"
#include "Public/RenderCore/SkeletonAsset.h"
#include "Public/RenderCore/TextureCompression.h"
#include "Private/Editor/Content/GltfImporter.h"
#if WITH_AUDIO
#include "Public/Audio/SoundAsset.h"
#endif

namespace EditorContent
{
    namespace
    {
        const char* const MESH_EXTENSION    = ".nmmesh";
        const char* const TEXTURE_EXTENSION = ".nmtexture";
        const char* const SKELETON_EXTENSION = ".nmskel";
        const char* const CLIP_EXTENSION     = ".nmanim";

        /* What a skeleton's and a clip's recorded settings add to the mesh options'. */
        const char* const SKELETON_PART_SETTING = "Part=Skeleton\n";
        const char* const CLIP_SETTING_KEY      = "Animation";
        const char* const BAKED_VIA_KEY         = "BakedVia";   // a clip baked through this retarget (BakeRetargetClip)
        const char* const SOUND_EXTENSION   = ".nmsound";

        /* Where an embedded image's source points: the glTF, then which image. */
        const char* const EMBEDDED_IMAGE_MARK = "#image";

        QStringList MeshSuffixes() { return { QStringLiteral("gltf"), QStringLiteral("glb") }; }

        /* What miniaudio decodes; none without the module. */
        QStringList SoundSuffixes()
        {
#if WITH_AUDIO
            return { QStringLiteral("wav"), QStringLiteral("flac"), QStringLiteral("mp3"), QStringLiteral("ogg") };
#else
            return {};
#endif
        }

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

        const QString ENGINE_CONTENT_PREFIX = QStringLiteral("Engine/Content/");

        /* SetImportEngineContentRoot's: absolute, forward slashes; empty: none. */
        QString& EngineContentRoot()
        {
            static QString Root;
            return Root;
        }

        bool IsEngineContent(const QString& InAbsolute)
        {
            const QString& Root = EngineContentRoot();
            return !Root.isEmpty() && QDir::fromNativeSeparators(InAbsolute).startsWith(Root + QLatin1Char('/'), Qt::CaseInsensitive);
        }

        /* "Engine/Content/..." for a file in the engine's content (IsEngineContent). */
        QString EngineContentRuntimePath(const QString& InAbsolute)
        {
            return ENGINE_CONTENT_PREFIX + QDir::fromNativeSeparators(QDir(EngineContentRoot()).relativeFilePath(InAbsolute));
        }

        /* InSource relative to the project when it is inside it ("Engine/Content/..."
         * in the engine's content), forward slashes. */
        RenderCore::AssetImportInfo MakeImportInfo(const QString& InSource, const QString& InProjectRoot, const QString& InSettings)
        {
            const QFileInfo Source(InSource.section(QString::fromLatin1(EMBEDDED_IMAGE_MARK), 0, 0));
            QString Path = QFileInfo(InSource).isAbsolute() ? InSource : Source.absoluteFilePath();
            if(IsEngineContent(Path))
            {
                Path = EngineContentRuntimePath(Path);
            }
            else if(!InProjectRoot.isEmpty())
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
            if(Path.startsWith(ENGINE_CONTENT_PREFIX) && !EngineContentRoot().isEmpty())
            {
                return QDir(EngineContentRoot()).absoluteFilePath(Path.mid(ENGINE_CONTENT_PREFIX.size()));
            }
            return QDir::isAbsolutePath(Path) ? Path : QDir(InProjectRoot).absoluteFilePath(Path);
        }

        /* A runtime path ("Engine/Content/...", project-relative, or absolute) back to a file. */
        QString AbsolutePathOfRuntime(const QString& InRuntime, const QString& InProjectRoot)
        {
            RenderCore::AssetImportInfo Info;
            Info.SourcePath = QDir::fromNativeSeparators(InRuntime).toStdString();
            return ResolveSource(Info, InProjectRoot);
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

        bool WriteSkeleton(const QString& InPath, RenderCore::SkeletonData& InSkeleton, QString& OutError)
        {
            EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
            RenderCore::SerializeSkeleton(Ar, InSkeleton);
            return WriteArchive(InPath, Ar, OutError);
        }

        bool WriteClip(const QString& InPath, RenderCore::AnimClipData& InClip, QString& OutError)
        {
            EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
            RenderCore::SerializeAnimClip(Ar, InClip);
            return WriteArchive(InPath, Ar, OutError);
        }

        /* What an asset names InAbsolute by: project-relative, as a
         * RenderComponent names a mesh, or "Engine/Content/..." in the
         * engine's content. Outside both, the absolute path -- which only
         * this machine's editor can follow. */
        QString RuntimePathOf(const QString& InAbsolute, const QString& InProjectRoot)
        {
            if(IsEngineContent(InAbsolute)) { return EngineContentRuntimePath(InAbsolute); }
            if(!InProjectRoot.isEmpty())
            {
                const QString Relative = QDir(InProjectRoot).relativeFilePath(InAbsolute);
                if(!Relative.startsWith(QStringLiteral(".."))) { return QDir::fromNativeSeparators(Relative); }
            }
            return QDir::fromNativeSeparators(QFileInfo(InAbsolute).absoluteFilePath());
        }

        /* InPath's file whole, read directly (not via the file provider, rooted at the exe). */
        EngineCore::Archive ReadAssetArchive(const QString& InPath)
        {
            QFile File(InPath);
            if(!File.open(QIODevice::ReadOnly))
            {
                return EngineCore::Archive::ForReading({});
            }
            const QByteArray Bytes = File.readAll();
            return EngineCore::Archive::ForReading(std::vector<uint8_t>(Bytes.begin(), Bytes.end()));
        }

        /* What the editor set on the clip already at InPath, kept over a fresh
         * import of it: its notifies, looping, root motion and root bone. */
        void KeepAuthoredClipFields(const QString& InPath, RenderCore::AnimClipData& InOutClip)
        {
            if(!QFileInfo::exists(InPath)) { return; }
            RenderCore::AnimClipData Old;
            EngineCore::Archive OldAr = ReadAssetArchive(InPath);
            if(RenderCore::SerializeAnimClip(OldAr, Old))
            {
                InOutClip.Notifies   = Old.Notifies;
                InOutClip.bLoop      = Old.bLoop;
                InOutClip.RootMotion = Old.RootMotion;
                InOutClip.RootBone   = Old.RootBone;
            }
        }

        /* A skeleton is written as <Base>_Skeleton, each clip as <Base>_<Clip>. */
        QString SkeletonFileFor(const QString& InFolder, const QString& InBase)
        {
            return QDir(InFolder).absoluteFilePath(InBase + QStringLiteral("_Skeleton") + QString::fromLatin1(SKELETON_EXTENSION));
        }

        bool ReadSkeleton(const QString& InPath, RenderCore::SkeletonData& OutSkeleton)
        {
            EngineCore::Archive Ar = ReadAssetArchive(InPath);
            OutSkeleton = RenderCore::SkeletonData();
            return Ar.GetSize() > 0 && RenderCore::SerializeSkeleton(Ar, OutSkeleton);
        }

        /* A mesh skinned to InFrom (the file's skeleton) skinned to InTo instead,
         * which has every bone of InFrom by name: joints renumbered, inverse binds
         * moved to InTo's order (InTo's reference pose for the bones InFrom lacks). */
        void RemapSkin(RenderCore::MeshData& InOutMesh, const RenderCore::SkeletonData& InFrom, const RenderCore::SkeletonData& InTo)
        {
            std::vector<uint16_t> Into(InFrom.Bones.size(), 0);
            for(size_t Bone = 0; Bone < InFrom.Bones.size(); ++Bone)
            {
                Into[Bone] = uint16_t(std::max(InTo.FindBone(InFrom.Bones[Bone].Name), 0));
            }
            for(uint16_t& Joint : InOutMesh.Joints)
            {
                if(Joint < Into.size()) { Joint = Into[Joint]; }
            }
            std::vector<RenderCore::AnimMatrix> Model;
            InTo.ComputeRefPoseModel(Model);
            std::vector<RenderCore::AnimMatrix> InverseBind(Model.size());
            for(size_t Bone = 0; Bone < Model.size(); ++Bone) { InverseBind[Bone] = RenderCore::AnimMath::Inverse(Model[Bone]); }
            for(size_t Bone = 0; Bone < InFrom.Bones.size() && Bone < InOutMesh.InverseBind.size(); ++Bone)
            {
                InverseBind[Into[Bone]] = InOutMesh.InverseBind[Bone];
            }
            InOutMesh.InverseBind = std::move(InverseBind);
            RenderCore::ComputeSkinnedMeshBounds(InOutMesh, InTo);
        }

        /* Create Retarget To: <Base>_to_<Target>.nmretarget in InFolder, from
         * InSource (InSourceRuntime) onto InTargetRuntime, auto-mapped and its arms
         * aligned; an existing file keeps its other sources. Its path, or empty
         * (warned) if the target cannot be read. OutNote: what the message adds. */
        QString WriteImportRetarget(const QString& InFolder, const QString& InBase, const QString& InSourceRuntime,
                                    const RenderCore::SkeletonData& InSource, const QString& InTargetRuntime, const QString& InProjectRoot,
                                    QStringList& OutWarnings, QString& OutNote)
        {
            RenderCore::SkeletonData Target;
            if(!ReadSkeleton(AbsolutePathOfRuntime(InTargetRuntime, InProjectRoot), Target))
            {
                OutWarnings << QStringLiteral("no retarget: the skeleton %1 cannot be read").arg(InTargetRuntime);
                return QString();
            }
            if(InTargetRuntime == InSourceRuntime)
            {
                OutWarnings << QStringLiteral("no retarget: the clips are already on %1").arg(InTargetRuntime);
                return QString();
            }
            QString TargetName = QFileInfo(InTargetRuntime).completeBaseName();
            if(TargetName.endsWith(QStringLiteral("_Skeleton"))) { TargetName.chop(9); }
            const QString Path = QDir(InFolder).absoluteFilePath(InBase + QStringLiteral("_to_") + SanitizeName(TargetName)
                                                                 + QString::fromLatin1(RenderCore::RETARGET_EXTENSION));

            RenderCore::RetargetData Retarget;
            if(QFileInfo::exists(Path))
            {
                EngineCore::Archive OldAr = ReadAssetArchive(Path);
                if(!RenderCore::SerializeRetarget(OldAr, Retarget) || Retarget.Target != InTargetRuntime.toStdString()) { Retarget = RenderCore::RetargetData(); }
            }
            Retarget.Target = InTargetRuntime.toStdString();
            RenderCore::RetargetSource Source;
            Source.Skeleton = InSourceRuntime.toStdString();
            const size_t Mapped = RenderCore::AutoMapRetarget(InSource, Target, Source);
            RenderCore::AlignRetargetArms(InSource, Target, Source);
            const auto Same = std::find_if(Retarget.Sources.begin(), Retarget.Sources.end(),
                                           [&](const RenderCore::RetargetSource& InEach) { return InEach.Skeleton == Source.Skeleton; });
            if(Same != Retarget.Sources.end()) { *Same = Source; }
            else                                 { Retarget.Sources.push_back(Source); }

            EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
            RenderCore::SerializeRetarget(Ar, Retarget);
            QString Error;
            if(!WriteArchive(Path, Ar, Error))
            {
                OutWarnings << QStringLiteral("no retarget: %1").arg(Error);
                return QString();
            }
            OutNote = QStringLiteral("; a retarget to %1, %2 of its %3 bones mapped").arg(InTargetRuntime).arg(Mapped).arg(Target.Bones.size());
            if(Mapped == 0) { OutWarnings << QStringLiteral("the retarget to %1 maps no bones: map them in the retarget editor").arg(InTargetRuntime); }
            return Path;
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
            /* Without an encoder, the compression is a setting only. */
            if(RenderCore::IsTextureCompressionAvailable(Texture.Compression))
            {
                std::string Error;
                if(!RenderCore::CompressTexture(Texture, Error))
                {
                    OutError = QStringLiteral("%1: %2").arg(QFileInfo(InPath).fileName(), QString::fromStdString(Error));
                    return false;
                }
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
            else if(InAssetPath.endsWith(QString::fromLatin1(SKELETON_EXTENSION), Qt::CaseInsensitive))
            {
                RenderCore::SkeletonData Skeleton;
                bRead   = RenderCore::SerializeSkeleton(Ar, Skeleton);
                OutInfo = Skeleton.ImportInfo;
                OutKind = EImportKind::Mesh;   // from a glTF, as its meshes are
            }
            else if(InAssetPath.endsWith(QString::fromLatin1(CLIP_EXTENSION), Qt::CaseInsensitive))
            {
                RenderCore::AnimClipData Clip;
                bRead   = RenderCore::SerializeAnimClip(Ar, Clip);
                OutInfo = Clip.ImportInfo;
                OutKind = EImportKind::Mesh;
            }
#if WITH_AUDIO
            else if(InAssetPath.endsWith(QString::fromLatin1(SOUND_EXTENSION), Qt::CaseInsensitive))
            {
                Audio::SoundData Sound;
                bRead = Audio::SerializeSound(Ar, Sound);
                OutInfo.SourcePath = Sound.Import.SourcePath;
                OutInfo.SourceTime = Sound.Import.SourceTime;
                OutInfo.Settings   = Sound.Import.Settings;
                OutKind = EImportKind::Sound;
            }
#endif
            if(!bRead)
            {
                OutError = QStringLiteral("%1: not a mesh, texture, skeleton, clip or sound this editor can read").arg(QFileInfo(InAssetPath).fileName());
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
        if(SoundSuffixes().contains(Suffix)) { return EImportKind::Sound; }
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
        if(SoundSuffixes().isEmpty())
        {
            return QStringLiteral("Importable Files (%1 %2);;glTF Meshes (%1);;Images (%2);;All Files (*)")
                   .arg(Patterns(MeshSuffixes()), Patterns(ImageSuffixes()));
        }
        return QStringLiteral("Importable Files (%1 %2 %3);;glTF Meshes (%1);;Images (%2);;Sounds (%3);;All Files (*)")
               .arg(Patterns(MeshSuffixes()), Patterns(ImageSuffixes()), Patterns(SoundSuffixes()));
    }

    QString GetAssetExtension(const EImportKind InKind)
    {
        switch(InKind)
        {
            case EImportKind::Mesh:    return QString::fromLatin1(MESH_EXTENSION);
            case EImportKind::Texture: return QString::fromLatin1(TEXTURE_EXTENSION);
            case EImportKind::Sound:   return QString::fromLatin1(SOUND_EXTENSION);
            case EImportKind::None:    break;
        }
        return QString();
    }

    // === Options ========================================================================

    QString MeshImportOptions::ToSettings() const
    {
        return QStringLiteral("Scale=%1\nConvertCoordinates=%2\nGenerateNormals=%3\nCombineMeshes=%4\nImportTextures=%5\n"
                              "GenerateLightmapUVs=%6\nImportAnimations=%7\nAnimationSampleRate=%8\nBakeMaterialColors=%9\n")
               .arg(double(Scale)).arg(int(bConvertCoordinates)).arg(int(bGenerateNormals))
               .arg(int(bCombineMeshes)).arg(int(bImportTextures)).arg(int(bGenerateLightmapUVs))
               .arg(int(bImportAnimations)).arg(double(AnimationSampleRate)).arg(int(bBakeMaterialColors))
             + QStringLiteral("BuildMeshlets=%1\n").arg(int(bBuildMeshlets))
             + (ExistingSkeleton.isEmpty() ? QString() : QStringLiteral("ExistingSkeleton=%1\n").arg(ExistingSkeleton))
             + (CreateRetargetTo.isEmpty() ? QString() : QStringLiteral("CreateRetargetTo=%1\n").arg(CreateRetargetTo));
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
        Out.bGenerateLightmapUVs = ReadBool(Map, QStringLiteral("GenerateLightmapUVs"), Out.bGenerateLightmapUVs);
        Out.bImportAnimations   = ReadBool(Map, QStringLiteral("ImportAnimations"), Out.bImportAnimations);
        Out.bBakeMaterialColors = ReadBool(Map, QStringLiteral("BakeMaterialColors"), false);   // absent: settings from before, as recorded
        Out.bBuildMeshlets      = ReadBool(Map, QStringLiteral("BuildMeshlets"), false);        // the same
        bool bRateOK = false;
        const float Rate = Map.value(QStringLiteral("AnimationSampleRate")).toFloat(&bRateOK);
        if(bRateOK && Rate >= 1.0f && Rate <= 240.0f) { Out.AnimationSampleRate = Rate; }
        Out.ExistingSkeleton = Map.value(QStringLiteral("ExistingSkeleton"));
        Out.CreateRetargetTo = Map.value(QStringLiteral("CreateRetargetTo"));
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

    void SetImportEngineContentRoot(const QString& InRoot)
    {
        EngineContentRoot() = InRoot.isEmpty() ? QString() : QDir::fromNativeSeparators(QDir(InRoot).absolutePath());
    }

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
        GltfAnimationSet Animation;
        QString Error;
        if(!LoadGltf(InSource, InOptions, Meshes, Result.Images, Result.Warnings, Error, &Animation))
        {
            Result.Message = Error;
            return Result;
        }

        const QString Base = SanitizeName(InBaseName);

        /* The skeleton before the meshes and clips, which name it: the file's,
         * or one its bones are already in. */
        QString SkeletonRuntime;
        RenderCore::SkeletonData SkeletonUsed;
        if(Animation.bHasSkeleton && !InOptions.ExistingSkeleton.isEmpty())
        {
            if(!ReadSkeleton(AbsolutePathOfRuntime(InOptions.ExistingSkeleton, InProjectRoot), SkeletonUsed))
            {
                Result.Message = QStringLiteral("%1: the existing skeleton %2 cannot be read").arg(QFileInfo(InSource).fileName(), InOptions.ExistingSkeleton);
                return Result;
            }
            QStringList Missing;
            for(const RenderCore::SkeletonBone& Bone : Animation.Skeleton.Bones)
            {
                if(SkeletonUsed.FindBone(Bone.Name) < 0) { Missing << QString::fromStdString(Bone.Name); }
            }
            if(!Missing.isEmpty())
            {
                Result.Message = QStringLiteral("%1: %2 of its bones are not in %3 (%4). Import its own skeleton and play its clips "
                                                "through a retarget instead (Create Retarget To).")
                                 .arg(QFileInfo(InSource).fileName()).arg(Missing.size()).arg(InOptions.ExistingSkeleton,
                                      Missing.mid(0, 8).join(QStringLiteral(", ")) + (Missing.size() > 8 ? QStringLiteral(", ...") : QString()));
                return Result;
            }
            for(GltfMesh& Mesh : Meshes)
            {
                if(Mesh.Data.IsSkinned()) { RemapSkin(Mesh.Data, Animation.Skeleton, SkeletonUsed); }
            }
            SkeletonRuntime = InOptions.ExistingSkeleton;
        }
        else if(Animation.bHasSkeleton)
        {
            SkeletonUsed = Animation.Skeleton;
            const QString SkeletonPath = SkeletonFileFor(InDestFolder, Base);
            Animation.Skeleton.ImportInfo = MakeImportInfo(InSource, InProjectRoot,
                                                           InOptions.ToSettings() + QString::fromLatin1(SKELETON_PART_SETTING));
            if(!WriteSkeleton(SkeletonPath, Animation.Skeleton, Error))
            {
                Result.Message = Error;
                return Result;
            }
            Result.Written << SkeletonPath;
            SkeletonRuntime = RuntimePathOf(SkeletonPath, InProjectRoot);
        }
        for(GltfClip& Clip : Animation.Clips)
        {
            const QString ClipPath = QDir(InDestFolder).absoluteFilePath(Base + QStringLiteral("_") + SanitizeName(Clip.Name)
                                                                         + QString::fromLatin1(CLIP_EXTENSION));
            KeepAuthoredClipFields(ClipPath, Clip.Data);
            Clip.Data.Skeleton   = SkeletonRuntime.toStdString();
            Clip.Data.ImportInfo = MakeImportInfo(InSource, InProjectRoot, InOptions.ToSettings()
                                                  + QStringLiteral("%1=%2\n").arg(QString::fromLatin1(CLIP_SETTING_KEY), Clip.Name));
            if(!WriteClip(ClipPath, Clip.Data, Error))
            {
                Result.Message = Error;
                return Result;
            }
            Result.Written << ClipPath;
        }

        /* A retarget from the skeleton the clips are on to the one they should play on. */
        QString RetargetNote;
        if(Animation.bHasSkeleton && !InOptions.CreateRetargetTo.isEmpty())
        {
            const QString RetargetPath = WriteImportRetarget(InDestFolder, Base, SkeletonRuntime, SkeletonUsed, InOptions.CreateRetargetTo,
                                                             InProjectRoot, Result.Warnings, RetargetNote);
            if(!RetargetPath.isEmpty()) { Result.Written << RetargetPath; }
        }

        uint64_t Triangles = 0;
        for(GltfMesh& Mesh : Meshes)
        {
            if(Mesh.Data.IsSkinned()) { Mesh.Data.Skeleton = SkeletonRuntime.toStdString(); }

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
        if(Animation.bHasSkeleton)
        {
            Result.Message += InOptions.ExistingSkeleton.isEmpty()
                            ? QStringLiteral("; a %1-bone skeleton and %2 animation clip(s)").arg(Animation.Skeleton.Bones.size()).arg(Animation.Clips.size())
                            : QStringLiteral("; %1 animation clip(s) on %2").arg(Animation.Clips.size()).arg(InOptions.ExistingSkeleton);
        }
        Result.Message += RetargetNote;
        return Result;
    }

    ImportResult BakeRetargetClip(const QString& InClipFile, const QString& InRetargetFile, const QString& InOutFile,
                                  const QString& InProjectRoot)
    {
        ImportResult Result;
        RenderCore::AnimClipData Clip;
        RenderCore::RetargetData Retarget;
        EngineCore::Archive ClipAr = ReadAssetArchive(InClipFile);
        EngineCore::Archive RetargetAr = ReadAssetArchive(InRetargetFile);
        if(ClipAr.GetSize() == 0 || !RenderCore::SerializeAnimClip(ClipAr, Clip))
        {
            Result.Message = QStringLiteral("%1: not a clip that can be read").arg(QFileInfo(InClipFile).fileName());
            return Result;
        }
        if(RetargetAr.GetSize() == 0 || !RenderCore::SerializeRetarget(RetargetAr, Retarget))
        {
            Result.Message = QStringLiteral("%1: not a retarget that can be read").arg(QFileInfo(InRetargetFile).fileName());
            return Result;
        }
        const RenderCore::RetargetSource* Map = Retarget.FindSource(Clip.Skeleton);
        if(!Map)
        {
            Result.Message = QStringLiteral("%1: %2 has no source for its skeleton %3").arg(QFileInfo(InClipFile).fileName(),
                             QFileInfo(InRetargetFile).fileName(), QString::fromStdString(Clip.Skeleton));
            return Result;
        }
        RenderCore::SkeletonData Source, Target;
        if(!ReadSkeleton(AbsolutePathOfRuntime(QString::fromStdString(Clip.Skeleton), InProjectRoot), Source)
           || !ReadSkeleton(AbsolutePathOfRuntime(QString::fromStdString(Retarget.Target), InProjectRoot), Target))
        {
            Result.Message = QStringLiteral("%1: the skeletons %2 and %3 cannot both be read").arg(QFileInfo(InClipFile).fileName(),
                             QString::fromStdString(Clip.Skeleton), QString::fromStdString(Retarget.Target));
            return Result;
        }
        RenderCore::RetargetBinding Binding;
        if(!RenderCore::BindClipRetargeted(Clip, Source, Target, *Map, Binding))
        {
            Result.Message = QStringLiteral("%1: it does not bind through the retarget").arg(QFileInfo(InClipFile).fileName());
            return Result;
        }
        RenderCore::AnimClipData Baked;
        RenderCore::BakeRetargetedClip(Clip, Binding, Target, Retarget.Target, Baked);
        KeepAuthoredClipFields(InOutFile, Baked);
        Baked.ImportInfo = MakeImportInfo(InClipFile, InProjectRoot, QStringLiteral("%1=%2\n").arg(QString::fromLatin1(BAKED_VIA_KEY),
                                          RuntimePathOf(QFileInfo(InRetargetFile).absoluteFilePath(), InProjectRoot)));
        QString Error;
        if(!WriteClip(InOutFile, Baked, Error))
        {
            Result.Message = Error;
            return Result;
        }
        Result.bOK = true;
        Result.Written << QFileInfo(InOutFile).absoluteFilePath();
        Result.Message = QStringLiteral("%1: baked onto %2, %3 tracks over %4 frames").arg(QFileInfo(InOutFile).fileName(),
                         QString::fromStdString(Retarget.Target)).arg(Baked.Tracks.size()).arg(Baked.FrameCount);
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

    ImportResult ImportSound(const QString& InSource, const QString& InDestFolder, const QString& InBaseName,
                             const QString& InProjectRoot)
    {
        ImportResult Result;
#if WITH_AUDIO
        const QString OutPath = QDir(InDestFolder).absoluteFilePath(SanitizeName(InBaseName) + QString::fromLatin1(SOUND_EXTENSION));

        Audio::SoundData Sound;
        std::string Error;
        if(!Audio::ImportSoundFile(QFileInfo(InSource).absoluteFilePath().toStdString(), Sound, Error))
        {
            Result.Message = QStringLiteral("%1: %2").arg(QFileInfo(InSource).fileName(), QString::fromStdString(Error));
            return Result;
        }

        /* Replacing a sound: its settings, cooked again from the new source. */
        bool bKeptSettings = false;
        if(QFile Existing(OutPath); Existing.open(QIODevice::ReadOnly))
        {
            const QByteArray Bytes = Existing.readAll();
            EngineCore::Archive Old = EngineCore::Archive::ForReading(std::vector<uint8_t>(Bytes.begin(), Bytes.end()));
            Audio::SoundData Previous;
            if(Audio::SerializeSound(Old, Previous))
            {
                Sound.Settings = Previous.Settings;
                if(!Audio::CookSound(Sound, Error))
                {
                    Result.Message = QStringLiteral("%1: %2").arg(QFileInfo(InSource).fileName(), QString::fromStdString(Error));
                    return Result;
                }
                bKeptSettings = true;
            }
        }

        const RenderCore::AssetImportInfo Info = MakeImportInfo(InSource, InProjectRoot, QString());
        Sound.Import.SourcePath = Info.SourcePath;
        Sound.Import.SourceTime = Info.SourceTime;
        Sound.Import.Settings   = Info.Settings;

        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        QString WriteError;
        if(!Audio::SerializeSound(Ar, Sound) || !WriteArchive(OutPath, Ar, WriteError))
        {
            Result.Message = WriteError.isEmpty() ? QStringLiteral("%1: cannot be written").arg(QFileInfo(OutPath).fileName()) : WriteError;
            return Result;
        }

        const Audio::CookedSound& Cooked = Sound.Cooked;
        Result.bOK = true;
        Result.Written << OutPath;
        Result.Message = QStringLiteral("%1: %2 s %3 sound, %4 channel(s), %5 Hz, %6%7")
                         .arg(QFileInfo(InSource).fileName())
                         .arg(Cooked.GetDuration(), 0, 'f', 2)
                         .arg(QString::fromLatin1(Audio::GetSoundSourceFormatName(Sound.SourceFormat)))
                         .arg(Cooked.Channels).arg(Cooked.SampleRate)
                         .arg(QString::fromLatin1(Audio::GetSoundLoadModeName(Cooked.Mode)))
                         .arg(bKeptSettings ? QStringLiteral(" (its settings kept)") : QString());
#else
        (void)InDestFolder;
        (void)InBaseName;
        (void)InProjectRoot;
        Result.Message = QStringLiteral("%1: sounds need the Audio module (NIMBLE_WITH_AUDIO=OFF)").arg(QFileInfo(InSource).fileName());
#endif
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

        /* A baked clip: from its clip and retarget again, not from a glTF. */
        const QString BakedVia = ParseSettings(Settings).value(QString::fromLatin1(BAKED_VIA_KEY));
        if(!BakedVia.isEmpty())
        {
            return BakeRetargetClip(Source, AbsolutePathOfRuntime(BakedVia, InProjectRoot), InAssetPath, InProjectRoot);
        }

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
        if(Kind == EImportKind::Sound)
        {
            return ImportSound(Source, Folder, Base, InProjectRoot);
        }

        /* Same options, over this one file (a split mesh finds its part
         * again). Textures reimport separately. */
        MeshImportOptions Options = MeshImportOptions::FromSettings(Settings);
        Options.bImportTextures = false;
        const QString Part = ParseSettings(Settings).value(QStringLiteral("Mesh"));

        QList<GltfMesh> Meshes;
        QList<ReferencedImage> Images;
        GltfAnimationSet Animation;
        if(!LoadGltf(Source, Options, Meshes, Images, Result.Warnings, Result.Message, &Animation))
        {
            return Result;
        }

        /* A skeleton or a clip: the same file's again, by part or name. */
        const QMap<QString, QString> Parsed = ParseSettings(Settings);
        if(InAssetPath.endsWith(QString::fromLatin1(SKELETON_EXTENSION), Qt::CaseInsensitive))
        {
            if(!Animation.bHasSkeleton)
            {
                Result.Message = QStringLiteral("%1 no longer has a skeleton").arg(QFileInfo(Source).fileName());
                return Result;
            }
            Animation.Skeleton.ImportInfo = MakeImportInfo(Source, InProjectRoot, Settings);
            if(!WriteSkeleton(InAssetPath, Animation.Skeleton, Result.Message))
            {
                return Result;
            }
            Result.bOK = true;
            Result.Written << QFileInfo(InAssetPath).absoluteFilePath();
            Result.Message = QStringLiteral("%1: reimported, %2 bones").arg(QFileInfo(InAssetPath).fileName())
                             .arg(Animation.Skeleton.Bones.size());
            return Result;
        }
        if(InAssetPath.endsWith(QString::fromLatin1(CLIP_EXTENSION), Qt::CaseInsensitive))
        {
            const QString Name = Parsed.value(QString::fromLatin1(CLIP_SETTING_KEY));
            for(GltfClip& Clip : Animation.Clips)
            {
                if(Clip.Name != Name) { continue; }

                /* What the editor set on the clip survives, and the skeleton it names. */
                RenderCore::AnimClipData Old;
                EngineCore::Archive OldAr = ReadAssetArchive(InAssetPath);
                if(RenderCore::SerializeAnimClip(OldAr, Old)) { Clip.Data.Skeleton = Old.Skeleton; }
                KeepAuthoredClipFields(InAssetPath, Clip.Data);
                Clip.Data.ImportInfo = MakeImportInfo(Source, InProjectRoot, Settings);
                if(!WriteClip(InAssetPath, Clip.Data, Result.Message))
                {
                    return Result;
                }
                Result.bOK = true;
                Result.Written << QFileInfo(InAssetPath).absoluteFilePath();
                Result.Message = QStringLiteral("%1: reimported, %2 frames").arg(QFileInfo(InAssetPath).fileName()).arg(Clip.Data.FrameCount);
                return Result;
            }
            Result.Message = QStringLiteral("%1 no longer has an animation named %2").arg(QFileInfo(Source).fileName(), Name);
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
        if(Chosen->Data.IsSkinned())
        {
            /* Its skeleton where the first import wrote it -- or where the mesh last said. */
            RenderCore::MeshData Old;
            EngineCore::Archive OldAr = ReadAssetArchive(InAssetPath);
            const bool bOld = RenderCore::SerializeMesh(OldAr, Old);
            Chosen->Data.Skeleton = bOld && !Old.Skeleton.empty()
                                  ? Old.Skeleton
                                  : RuntimePathOf(SkeletonFileFor(Folder, Base), InProjectRoot).toStdString();
            /* Imported onto an existing skeleton: skinned to it again. */
            RenderCore::SkeletonData Existing;
            if(!Options.ExistingSkeleton.isEmpty() && Animation.bHasSkeleton
               && ReadSkeleton(AbsolutePathOfRuntime(Options.ExistingSkeleton, InProjectRoot), Existing))
            {
                RemapSkin(Chosen->Data, Animation.Skeleton, Existing);
            }
        }
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
