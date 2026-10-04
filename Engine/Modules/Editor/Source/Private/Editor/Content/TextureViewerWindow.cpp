#if WITH_EDITOR

#include "Private/Editor/Content/TextureViewerWindow.h"

#include <algorithm>
#include <cmath>

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/TextureCompression.h"
#include "Private/Editor/Content/AssetImport.h"
#include "Private/Editor/Content/ContentPaths.h"

namespace
{
    QList<QPointer<TextureViewerWindow>>& OpenViewers()
    {
        static QList<QPointer<TextureViewerWindow>> Viewers;
        return Viewers;
    }

    /* What the preview shows of a texel. */
    enum class EChannels { RGBA, RGB, R, G, B, A };

    QString Bytes(const quint64 InBytes)
    {
        return QLocale().formattedDataSize(qint64(InBytes), 1, QLocale::DataSizeTraditionalFormat);
    }

    /* An RGBE8 (HDR) level as displayable RGBA8: decoded, Reinhard-mapped, 2.2 gamma. */
    EngineCore::Image DisplayRgbe(const EngineCore::Image& InLevel)
    {
        EngineCore::Image Out(InLevel.GetWidth(), InLevel.GetHeight(), EngineCore::EImageFormat::RGBA8);
        for(size_t At = 0; At + 3 < InLevel.GetSizeBytes(); At += 4)
        {
            float Rgb[3];
            RenderCore::DecodeRgbe(InLevel.GetData() + At, Rgb);
            for(int Channel = 0; Channel < 3; ++Channel)
            {
                const float Mapped = Rgb[Channel] / (1.0f + Rgb[Channel]);
                Out.GetData()[At + Channel] = uint8_t(std::lround(std::pow(Mapped, 1.0f / 2.2f) * 255.0f));
            }
            Out.GetData()[At + 3] = 255;
        }
        return Out;
    }

    /* InLevel as a QImage, showing InChannels: a single channel as grey. */
    QImage ToQImage(const EngineCore::Image& InLevel, const EChannels InChannels, const bool bInRgbe)
    {
        const EngineCore::Image Rgba = bInRgbe ? DisplayRgbe(InLevel) : InLevel.ConvertedTo(EngineCore::EImageFormat::RGBA8);
        QImage Out(Rgba.GetWidth(), Rgba.GetHeight(),
                   InChannels == EChannels::RGBA ? QImage::Format_RGBA8888 : QImage::Format_RGBX8888);
        for(int Y = 0; Y < Rgba.GetHeight(); ++Y)
        {
            const uint8_t* Src = Rgba.GetScanline(Y);
            uint8_t*       Dst = Out.scanLine(Y);
            for(int X = 0; X < Rgba.GetWidth() * 4; X += 4)
            {
                switch(InChannels)
                {
                    case EChannels::RGBA:
                    case EChannels::RGB:
                        Dst[X + 0] = Src[X + 0]; Dst[X + 1] = Src[X + 1]; Dst[X + 2] = Src[X + 2];
                        Dst[X + 3] = InChannels == EChannels::RGBA ? Src[X + 3] : 255;
                        break;
                    default:
                    {
                        const uint8_t Value = Src[X + (InChannels == EChannels::R ? 0 : InChannels == EChannels::G ? 1
                                                           : InChannels == EChannels::B ? 2 : 3)];
                        Dst[X + 0] = Dst[X + 1] = Dst[X + 2] = Value;
                        Dst[X + 3] = 255;
                        break;
                    }
                }
            }
        }
        return Out;
    }

    /* Whether level 0's alpha is all opaque, only 0 or 255 (1-bit), or blends. */
    QString DescribeAlpha(const EngineCore::Image& InPixels)
    {
        bool bAnyClear = false, bAnyPartial = false;
        for(int Y = 0; Y < InPixels.GetHeight() && !bAnyPartial; ++Y)
        {
            const uint8_t* Row = InPixels.GetScanline(Y);
            for(int X = 3; X < InPixels.GetWidth() * 4; X += 4)
            {
                if(Row[X] == 255) { continue; }
                if(Row[X] == 0) { bAnyClear = true; } else { bAnyPartial = true; break; }
            }
        }
        if(bAnyPartial) { return QStringLiteral("blended (BC3 or BC7 keeps it)"); }
        if(bAnyClear)   { return QStringLiteral("cut-out, 0 or 255 only"); }
        return QStringLiteral("opaque (BC1 is enough)");
    }

    struct CompressionChoice
    {
        RenderCore::ETextureCompression Value;
        const char*                     Label;
    };
    const CompressionChoice COMPRESSIONS[] = {
        { RenderCore::ETextureCompression::None, "None (RGBA8, 32 bpp)" },
        { RenderCore::ETextureCompression::BC1,  "BC1 (RGB, cut-out alpha, 4 bpp)" },
        { RenderCore::ETextureCompression::BC3,  "BC3 (RGBA, 8 bpp)" },
        { RenderCore::ETextureCompression::BC4,  "BC4 (one channel, 4 bpp)" },
        { RenderCore::ETextureCompression::BC5,  "BC5 (two channels: normal maps, 8 bpp)" },
        { RenderCore::ETextureCompression::BC7,  "BC7 (RGBA, high quality, 8 bpp)" },
    };
}

/*
 * The image, over a checkerboard where it is see-through: fitted to the view
 * until zoomed (the wheel, about the cursor), dragged to pan, double-clicked
 * to fit again. Texels stay square and sharp from 100% up.
 */
class TexturePreview final : public QWidget
{
public:
    explicit TexturePreview(QWidget* InParent) : QWidget(InParent)
    {
        setMinimumSize(240, 240);
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);
    }

    /* InImage at InLevel of the chain: zoom is kept in level-0 texels, so a
     * smaller level fills the same area. */
    void SetImage(const QImage& InImage, const int InLevel)
    {
        Image = InImage;
        Level = InLevel;
        update();
    }

    void SetBaseSize(const QSize InSize)
    {
        if(InSize != BaseSize) { BaseSize = InSize; bFit = true; }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter Painter(this);
        Painter.fillRect(rect(), palette().window());
        if(Image.isNull() || BaseSize.isEmpty()) { return; }

        const double Scale = CurrentScale();
        const QSizeF Shown(BaseSize.width() * Scale, BaseSize.height() * Scale);
        const QRectF Area(QPointF(width() / 2.0 - Shown.width() / 2.0 + Pan.x(), height() / 2.0 - Shown.height() / 2.0 + Pan.y()), Shown);

        /* The checkerboard, under the image only. */
        Painter.save();
        Painter.setClipRect(Area);
        const int Cell = 8;
        for(int Y = int(std::floor(Area.top() / Cell)) * Cell; Y < Area.bottom(); Y += Cell)
        {
            for(int X = int(std::floor(Area.left() / Cell)) * Cell; X < Area.right(); X += Cell)
            {
                Painter.fillRect(X, Y, Cell, Cell, ((X / Cell + Y / Cell) & 1) ? QColor(150, 150, 150) : QColor(105, 105, 105));
            }
        }
        Painter.restore();

        const double TexelScale = Shown.width() / std::max(Image.width(), 1);
        Painter.setRenderHint(QPainter::SmoothPixmapTransform, TexelScale < 1.0);
        Painter.drawImage(Area, Image);

        Painter.setPen(palette().color(QPalette::PlaceholderText));
        Painter.drawText(rect().adjusted(8, 4, -8, -4), Qt::AlignBottom | Qt::AlignLeft,
                         QStringLiteral("%1%  -  mip %2: %3 x %4").arg(int(std::lround(Scale * 100.0)))
                         .arg(Level).arg(Image.width()).arg(Image.height()));
    }

    void wheelEvent(QWheelEvent* InEvent) override
    {
        if(BaseSize.isEmpty()) { return; }
        const double Before = CurrentScale();
        const double After  = std::clamp(Before * std::pow(1.0015, InEvent->angleDelta().y()), 0.01, 64.0);
        /* About the cursor: the texel under it stays under it. */
        const QPointF Centre(width() / 2.0 + Pan.x(), height() / 2.0 + Pan.y());
        const QPointF Offset = InEvent->position() - Centre;
        Pan += Offset - Offset * (After / Before);
        Zoom = After;
        bFit = false;
        update();
    }

    void mousePressEvent(QMouseEvent* InEvent) override
    {
        DragFrom = InEvent->position();
        bDragging = true;
        setCursor(Qt::ClosedHandCursor);
    }

    void mouseMoveEvent(QMouseEvent* InEvent) override
    {
        if(!bDragging) { return; }
        Pan += InEvent->position() - DragFrom;
        DragFrom = InEvent->position();
        if(bFit) { Zoom = CurrentScale(); bFit = false; }
        update();
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        bDragging = false;
        unsetCursor();
    }

    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        bFit = true;
        Pan  = QPointF();
        update();
    }

private:
    double CurrentScale() const
    {
        if(!bFit) { return Zoom; }
        const double Fit = std::min((width() - 16.0) / std::max(BaseSize.width(), 1), (height() - 32.0) / std::max(BaseSize.height(), 1));
        return std::max(std::min(Fit, 8.0), 0.01);
    }

    QImage  Image;
    QSize   BaseSize;
    int     Level     = 0;
    bool    bFit      = true;
    double  Zoom      = 1.0;
    QPointF Pan;
    QPointF DragFrom;
    bool    bDragging = false;
};

TextureViewerWindow* TextureViewerWindow::Open(const QString& InPath, QWidget* InParent)
{
    const QString Path = QFileInfo(InPath).absoluteFilePath();
    for(const QPointer<TextureViewerWindow>& Each : OpenViewers())
    {
        if(Each && Each->GetPath().compare(Path, Qt::CaseInsensitive) == 0)
        {
            Each->show();
            Each->raise();
            Each->activateWindow();
            return Each;
        }
    }

    auto* Viewer = new TextureViewerWindow(Path);
    QString Error;
    if(!Viewer->Load(Error))
    {
        QMessageBox::warning(InParent, QStringLiteral("Open Texture"), QStringLiteral("Could not open %1\n\n%2")
                             .arg(QFileInfo(Path).fileName(), Error));
        delete Viewer;
        return nullptr;
    }
    Viewer->BuildUi();
    OpenViewers().removeAll(nullptr);
    OpenViewers() << Viewer;
    Viewer->resize(1040, 660);
    Viewer->show();
    return Viewer;
}

TextureViewerWindow::TextureViewerWindow(const QString& InPath)
    : QMainWindow(nullptr)
    , Path(InPath)
    , RuntimePath(EditorContent::ToRuntimePath(InPath))
    , bReadOnly(EditorContent::IsEngineContent(InPath))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("%1 - Texture").arg(QFileInfo(InPath).fileName()));
}

bool TextureViewerWindow::Load(QString& OutError)
{
    QFile File(Path);
    if(!File.open(QIODevice::ReadOnly))
    {
        OutError = File.errorString();
        return false;
    }
    const QByteArray Data = File.readAll();
    EngineCore::Archive Ar = EngineCore::Archive::ForReading(std::vector<uint8_t>(Data.begin(), Data.end()));
    RenderCore::TextureData Loaded;
    if(!RenderCore::SerializeTexture(Ar, Loaded))
    {
        OutError = QStringLiteral("Not a texture this editor can read (the log says why)");
        return false;
    }
    FileBytes = Data.size();
    Saved     = std::move(Loaded);
    Edited    = Saved;
    return true;
}

void TextureViewerWindow::BuildUi()
{
    Preview = new TexturePreview(this);

    auto* Side   = new QWidget(this);
    auto* Layout = new QVBoxLayout(Side);

    Info = new QLabel(Side);
    Info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Info->setWordWrap(true);
    Info->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    Layout->addWidget(Info);

    /* What the preview shows. */
    Layout->addWidget(new QLabel(QStringLiteral("<b>View</b>"), Side));
    auto* ViewForm = new QFormLayout();
    Level = new QComboBox(Side);
    Channels = new QComboBox(Side);
    Channels->addItem(QStringLiteral("RGBA"), int(EChannels::RGBA));
    Channels->addItem(QStringLiteral("RGB"), int(EChannels::RGB));
    Channels->addItem(QStringLiteral("Red"), int(EChannels::R));
    Channels->addItem(QStringLiteral("Green"), int(EChannels::G));
    Channels->addItem(QStringLiteral("Blue"), int(EChannels::B));
    Channels->addItem(QStringLiteral("Alpha"), int(EChannels::A));
    ViewForm->addRow(QStringLiteral("Mip level"), Level);
    ViewForm->addRow(QStringLiteral("Channels"), Channels);
    Layout->addLayout(ViewForm);
    connect(Level, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { UpdatePreview(); });
    connect(Channels, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { UpdatePreview(); });

    /* The texture's own settings. */
    Layout->addWidget(new QLabel(QStringLiteral("<b>Settings</b>"), Side));
    auto* Form = new QFormLayout();
    SRGB = new QCheckBox(QStringLiteral("Colour data (sRGB)"), Side);
    SRGB->setToolTip(QStringLiteral("Off for masks, normal maps and other data. Mips are filtered in linear light when on."));
    Filter = new QComboBox(Side);
    Filter->addItem(QStringLiteral("Linear (smooth; trilinear with mips)"), int(RenderCore::ETextureFilter::Linear));
    Filter->addItem(QStringLiteral("Nearest (pixelated)"), int(RenderCore::ETextureFilter::Nearest));
    Wrap = new QComboBox(Side);
    Wrap->addItem(QStringLiteral("Repeat"), int(RenderCore::ETextureWrap::Repeat));
    Wrap->addItem(QStringLiteral("Clamp to edge"), int(RenderCore::ETextureWrap::Clamp));
    Wrap->addItem(QStringLiteral("Mirror"), int(RenderCore::ETextureWrap::Mirror));
    MipGen = new QComboBox(Side);
    MipGen->addItem(QStringLiteral("None"), int(RenderCore::ETextureMipGen::None));
    MipGen->addItem(QStringLiteral("Box (2x2 average)"), int(RenderCore::ETextureMipGen::Box));
    MipGen->addItem(QStringLiteral("Kaiser (sharper)"), int(RenderCore::ETextureMipGen::Kaiser));
    MipGen->setToolTip(QStringLiteral("The mip chain baked into the file, down to 1x1. Its edges follow Wrap."));
    Compression = new QComboBox(Side);
    for(const CompressionChoice& Each : COMPRESSIONS) { Compression->addItem(QString::fromLatin1(Each.Label), int(Each.Value)); }
    Form->addRow(QString(), SRGB);
    Form->addRow(QStringLiteral("Filter"), Filter);
    Form->addRow(QStringLiteral("Wrap"), Wrap);
    Form->addRow(QStringLiteral("Mip generation"), MipGen);
    Form->addRow(QStringLiteral("Compression"), Compression);
    Layout->addLayout(Form);
    connect(SRGB, &QCheckBox::toggled, this, [this](bool) { SettingChanged(); });
    for(QComboBox* Each : { Filter, Wrap, MipGen, Compression })
    {
        connect(Each, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { SettingChanged(); });
    }

    CompressNote = new QLabel(Side);
    CompressNote->setWordWrap(true);
    CompressNote->setForegroundRole(QPalette::PlaceholderText);
    Layout->addWidget(CompressNote);

    if(bReadOnly)
    {
        auto* Note = new QLabel(QStringLiteral("Engine content: changes here only preview."), Side);
        Note->setForegroundRole(QPalette::PlaceholderText);
        Layout->addWidget(Note);
    }
    Layout->addStretch(1);

    SaveButton   = new QPushButton(QStringLiteral("Save"), Side);
    RevertButton = new QPushButton(QStringLiteral("Revert"), Side);
    SaveButton->setToolTip(bReadOnly ? QStringLiteral("The engine's textures are read-only here")
                                     : QStringLiteral("Write these settings and the mips into the texture (Ctrl+S)"));
    connect(SaveButton, &QPushButton::clicked, this, &TextureViewerWindow::Save);
    connect(RevertButton, &QPushButton::clicked, this, &TextureViewerWindow::Revert);
    auto* SaveRow = new QHBoxLayout();
    SaveRow->addStretch(1);
    SaveRow->addWidget(RevertButton);
    SaveRow->addWidget(SaveButton);
    Layout->addLayout(SaveRow);

    Status = new QLabel(Side);
    Status->setWordWrap(true);
    Status->setForegroundRole(QPalette::PlaceholderText);
    Layout->addWidget(Status);

    auto* SaveKey = new QAction(this);
    SaveKey->setShortcut(QKeySequence::Save);
    connect(SaveKey, &QAction::triggered, this, &TextureViewerWindow::Save);
    addAction(SaveKey);

    auto* Split = new QSplitter(Qt::Horizontal, this);
    Split->addWidget(Preview);
    Split->addWidget(Side);
    Split->setStretchFactor(0, 1);
    Split->setStretchFactor(1, 0);
    Split->setSizes({ 680, 360 });
    setCentralWidget(Split);

    FillControls();
    FillMipLevels();
    UpdateInfo();
    UpdatePreview();
    UpdateState();
}

void TextureViewerWindow::FillControls()
{
    bFilling = true;
    SRGB->setChecked(Edited.bSRGB);
    Filter->setCurrentIndex(Filter->findData(int(Edited.Filter)));
    Wrap->setCurrentIndex(Wrap->findData(int(Edited.Wrap)));
    MipGen->setCurrentIndex(MipGen->findData(int(Edited.MipGen)));
    Compression->setCurrentIndex(Compression->findData(int(Edited.Compression)));
    /* An HDRI is decoded by its shader, point sampled: no sRGB, mips or block compression. */
    const bool bRgbe = Edited.Format == RenderCore::ETexturePixelFormat::RGBE8;
    for(QWidget* Each : std::initializer_list<QWidget*>{ SRGB, MipGen, Compression }) { Each->setEnabled(!bRgbe); }
    bFilling = false;
}

void TextureViewerWindow::FillMipLevels()
{
    const int Was = Level->currentIndex();
    const QSignalBlocker Block(Level);
    Level->clear();
    for(int Each = 0; Each < Edited.GetMipCount(); ++Each)
    {
        const EngineCore::Image* Mip = Edited.GetMip(Each);
        Level->addItem(QStringLiteral("%1: %2 x %3").arg(Each).arg(Mip->GetWidth()).arg(Mip->GetHeight()));
    }
    Level->setCurrentIndex(std::clamp(Was, 0, Level->count() - 1));
    Level->setEnabled(Level->count() > 1);
}

void TextureViewerWindow::UpdateInfo()
{
    const int  Width  = Edited.Pixels.GetWidth();
    const int  Height = Edited.Pixels.GetHeight();
    const int  Mips   = Edited.GetMipCount();
    const bool bPow2  = (Width & (Width - 1)) == 0 && (Height & (Height - 1)) == 0;

    const bool bRgbe = Edited.Format == RenderCore::ETexturePixelFormat::RGBE8;
    QString Text = QStringLiteral("<b>%1</b><br>%2 x %3%4, %6, %5<br>")
                   .arg(RuntimePath.isEmpty() ? QFileInfo(Path).fileName().toHtmlEscaped() : RuntimePath.toHtmlEscaped())
                   .arg(Width).arg(Height)
                   .arg(bPow2 ? QString() : QStringLiteral(" (not a power of two)"))
                   .arg(Edited.bSRGB ? QStringLiteral("sRGB") : QStringLiteral("linear"))
                   .arg(bRgbe ? QStringLiteral("RGBE8 (HDR, shown tone mapped)") : QStringLiteral("RGBA8"));
    Text += Mips > 1 ? QStringLiteral("%1 mip levels (%2), down to 1 x 1<br>").arg(Mips)
                       .arg(QString::fromLatin1(RenderCore::GetTextureMipGenName(Edited.MipGen)))
                     : QStringLiteral("No mips%1<br>").arg(RenderCore::FullMipCount(Width, Height) > 1
                                                          ? QStringLiteral(" (a full chain is %1 levels)").arg(RenderCore::FullMipCount(Width, Height))
                                                          : QString());
    Text += QStringLiteral("On disk %1%2<br>GPU memory %3")
            .arg(Bytes(quint64(FileBytes)))
            .arg(IsDirty() ? QStringLiteral(" (as saved)") : QString())
            .arg(Bytes(RenderCore::TextureGpuBytes(Edited)));
    if(Edited.Compression != RenderCore::ETextureCompression::None)
    {
        Text += QStringLiteral(", %1 once %2").arg(Bytes(RenderCore::TextureCompressedBytes(Width, Height, Mips, Edited.Compression)))
                .arg(QString::fromLatin1(RenderCore::GetTextureCompressionName(Edited.Compression)));
    }
    if(!bRgbe) { Text += QStringLiteral("<br>Alpha: %1").arg(DescribeAlpha(Edited.Pixels)); }
    if(!Edited.ImportInfo.IsEmpty())
    {
        Text += QStringLiteral("<br>From %1").arg(QString::fromStdString(Edited.ImportInfo.SourcePath).toHtmlEscaped());
        if(Edited.ImportInfo.SourceTime > 0)
        {
            Text += QStringLiteral(" (%1)").arg(QLocale().toString(QDateTime::fromSecsSinceEpoch(Edited.ImportInfo.SourceTime),
                                                                   QLocale::ShortFormat));
        }
    }
    Info->setText(Text);

    if(Edited.Compression == RenderCore::ETextureCompression::None)
    {
        CompressNote->clear();
    }
    else if(RenderCore::IsTextureCompressionAvailable(Edited.Compression))
    {
        CompressNote->setText(QStringLiteral("Encoded by %1.").arg(QString::fromLatin1(RenderCore::GetTextureCompressor()->GetName())));
    }
    else
    {
        CompressNote->setText(QStringLiteral("Saved as a setting only: no block encoder is integrated yet (NVIDIA Texture "
                                             "Tools to come), so the texture is still stored and drawn as RGBA8."));
    }
}

void TextureViewerWindow::UpdatePreview()
{
    const int Index = std::max(Level->currentIndex(), 0);
    const EngineCore::Image* Mip = Edited.GetMip(Index);
    if(!Mip) { return; }
    Preview->SetBaseSize(QSize(Edited.Pixels.GetWidth(), Edited.Pixels.GetHeight()));
    Preview->SetImage(ToQImage(*Mip, EChannels(Channels->currentData().toInt()),
                               Edited.Format == RenderCore::ETexturePixelFormat::RGBE8), Index);
}

bool TextureViewerWindow::IsDirty() const
{
    return Edited.bSRGB != Saved.bSRGB || Edited.Filter != Saved.Filter || Edited.Wrap != Saved.Wrap
        || Edited.MipGen != Saved.MipGen || Edited.Compression != Saved.Compression;
}

void TextureViewerWindow::UpdateState()
{
    const bool bDirty = IsDirty();
    SaveButton->setEnabled(bDirty && !bReadOnly);
    RevertButton->setEnabled(bDirty);
    setWindowModified(bDirty && !bReadOnly);
    setWindowTitle(QStringLiteral("%1%2 - Texture%3").arg(QFileInfo(Path).fileName(),
                                                          bDirty && !bReadOnly ? QStringLiteral("*") : QString(),
                                                          bReadOnly ? QStringLiteral(" (engine, read-only)") : QString()));
}

void TextureViewerWindow::SettingChanged()
{
    if(bFilling) { return; }
    const bool                       bWasSRGB  = Edited.bSRGB;
    const RenderCore::ETextureWrap   WasWrap   = Edited.Wrap;
    const RenderCore::ETextureMipGen WasMipGen = Edited.MipGen;
    Edited.bSRGB       = SRGB->isChecked();
    Edited.Filter      = RenderCore::ETextureFilter(Filter->currentData().toInt());
    Edited.Wrap        = RenderCore::ETextureWrap(Wrap->currentData().toInt());
    Edited.MipGen      = RenderCore::ETextureMipGen(MipGen->currentData().toInt());
    Edited.Compression = RenderCore::ETextureCompression(Compression->currentData().toInt());

    /* What the chain depends on: rebuilt to show, and to save. */
    if(Edited.bSRGB != bWasSRGB || Edited.Wrap != WasWrap || Edited.MipGen != WasMipGen)
    {
        if(Edited.bSRGB == Saved.bSRGB && Edited.Wrap == Saved.Wrap && Edited.MipGen == Saved.MipGen)
        {
            Edited.Mips = Saved.Mips;   // back as saved: the file's own chain
        }
        else
        {
            RenderCore::BuildTextureMips(Edited);
        }
        FillMipLevels();
        UpdatePreview();
    }
    Status->clear();
    UpdateInfo();
    UpdateState();
}

bool TextureViewerWindow::Save()
{
    if(bReadOnly || !IsDirty()) { return true; }

    RenderCore::TextureData Updated = Edited;
    /* Recorded in the import options too, so a Reimport keeps them. */
    if(!Updated.ImportInfo.IsEmpty())
    {
        EditorContent::TextureImportOptions Options =
            EditorContent::TextureImportOptions::FromSettings(QString::fromStdString(Updated.ImportInfo.Settings));
        Options.bSRGB       = Updated.bSRGB;
        Options.Filter      = Updated.Filter;
        Options.Wrap        = Updated.Wrap;
        Options.MipGen      = Updated.MipGen;
        Options.Compression = Updated.Compression;
        Updated.ImportInfo.Settings = Options.ToSettings().toStdString();
    }
    if(!RenderCore::SaveTextureFile(QFileInfo(Path).absoluteFilePath().toStdString(), Updated))
    {
        QMessageBox::warning(this, QStringLiteral("Save Texture"), QStringLiteral("Could not write %1 -- see the log").arg(Path));
        return false;
    }

    Saved     = Updated;
    Edited    = std::move(Updated);
    FileBytes = QFileInfo(Path).size();
    EditorContent::StageAsset(Path);   // the level loads it afresh
    Status->setText(QStringLiteral("Saved"));
    UpdateInfo();
    UpdateState();
    return true;
}

void TextureViewerWindow::Revert()
{
    Edited = Saved;
    FillControls();
    FillMipLevels();
    Status->clear();
    UpdateInfo();
    UpdatePreview();
    UpdateState();
}

void TextureViewerWindow::closeEvent(QCloseEvent* InEvent)
{
    if(!bReadOnly && IsDirty())
    {
        const QMessageBox::StandardButton Answer = QMessageBox::question(
            this, QStringLiteral("Texture"), QStringLiteral("Save the changes to %1?").arg(QFileInfo(Path).fileName()),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if(Answer == QMessageBox::Cancel || (Answer == QMessageBox::Save && !Save()))
        {
            InEvent->ignore();
            return;
        }
    }
    InEvent->accept();
}

#endif // WITH_EDITOR
