/**
* VIBECODE CERTIFIED
*/
#if WITH_EDITOR

#include "Private/Editor/EditorIcons.h"

#include <array>
#include <utility>

#include <QApplication>
#include <QFile>
#include <QStyle>

#if NIMBLEEDITOR_HAS_QTSVG
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSvgRenderer>
#endif

namespace
{
    struct FIconInfo
    {
        const char*             Resource;   // under :/lucide/
        QStyle::StandardPixmap  Fallback;
    };

    /* In EIcon's order. */
    constexpr std::array<FIconInfo, size_t(EditorIcons::EIcon::Count)> Icons =
    {{
        { "play.svg",            QStyle::SP_MediaPlay },
        { "pause.svg",           QStyle::SP_MediaPause },
        { "square.svg",          QStyle::SP_MediaStop },
        { "undo-2.svg",          QStyle::SP_ArrowBack },
        { "redo-2.svg",          QStyle::SP_ArrowForward },
        { "save.svg",            QStyle::SP_DialogSaveButton },
        { "skip-back.svg",       QStyle::SP_MediaSkipBackward },
        { "skip-forward.svg",    QStyle::SP_MediaSkipForward },
        { "step-back.svg",       QStyle::SP_MediaSeekBackward },
        { "step-forward.svg",    QStyle::SP_MediaSeekForward },
        { "repeat.svg",          QStyle::SP_BrowserReload },
        { "circle-dot.svg",      QStyle::SP_DialogApplyButton },
        { "key-round.svg",       QStyle::SP_DialogApplyButton },
        { "clapperboard.svg",    QStyle::SP_DesktopIcon },
        { "plane.svg",           QStyle::SP_ArrowUp },
        { "magnet.svg",          QStyle::SP_DialogApplyButton },
        { "plus.svg",            QStyle::SP_FileDialogNewFolder },
        { "zoom-in.svg",         QStyle::SP_ArrowUp },
        { "zoom-out.svg",        QStyle::SP_ArrowDown },
        { "scan.svg",            QStyle::SP_TitleBarMaxButton },
        { "volume-2.svg",        QStyle::SP_MediaVolume },
        { "audio-waveform.svg",  QStyle::SP_MediaVolume },
        { "captions.svg",        QStyle::SP_FileDialogDetailedView },
        { "eye.svg",             QStyle::SP_FileDialogContentsView },
        { "circle-question-mark.svg", QStyle::SP_TitleBarContextHelpButton },
        { "terrain.svg",              QStyle::SP_DriveHDIcon },
        { "terrain-raise.svg",        QStyle::SP_ArrowUp },
        { "terrain-lower.svg",        QStyle::SP_ArrowDown },
        { "terrain-smooth.svg",       QStyle::SP_BrowserReload },
        { "terrain-flatten.svg",      QStyle::SP_ToolBarHorizontalExtensionButton },
        { "terrain-noise.svg",        QStyle::SP_MessageBoxWarning },
        { "terrain-set-height.svg",   QStyle::SP_ArrowRight },
        { "terrain-paint.svg",        QStyle::SP_DialogApplyButton },
        { "terrain-smooth-paint.svg", QStyle::SP_DialogResetButton },
        { "foliage.svg",              QStyle::SP_DialogYesButton },
        { "foliage-erase.svg",        QStyle::SP_DialogNoButton },
        { "bell.svg",                 QStyle::SP_MessageBoxInformation },
        { "bell-off.svg",             QStyle::SP_MediaVolumeMuted },
        { "circle-x.svg",             QStyle::SP_MessageBoxCritical },
        { "circle-check.svg",         QStyle::SP_DialogApplyButton },
        { "triangle-alert.svg",       QStyle::SP_MessageBoxWarning },
        { "info.svg",                 QStyle::SP_MessageBoxInformation },
        { "x.svg",                    QStyle::SP_TitleBarCloseButton },
        { "git-branch.svg",           QStyle::SP_DirLinkIcon },
        { "refresh-cw.svg",           QStyle::SP_BrowserReload },
        { "arrow-up.svg",             QStyle::SP_ArrowUp },
        { "arrow-down.svg",           QStyle::SP_ArrowDown },
        { "cloud-download.svg",       QStyle::SP_ArrowDown },
        { "upload.svg",               QStyle::SP_ArrowUp },
        { "git-commit-horizontal.svg", QStyle::SP_DialogSaveButton },
        { "lock.svg",                 QStyle::SP_MessageBoxWarning },
        { "git-merge.svg",            QStyle::SP_MessageBoxWarning },
    }};

    QIcon FallbackIcon(const EditorIcons::EIcon InIcon)
    {
        QStyle* Style = QApplication::style();
        return Style ? Style->standardIcon(Icons[size_t(InIcon)].Fallback) : QIcon();
    }

#if NIMBLEEDITOR_HAS_QTSVG
    /* One Lucide SVG, drawn in the palette's colour for the mode. */
    class LucideIconEngine final : public QIconEngine
    {
    public:
        /* InColor: drawn in it instead of the palette's; invalid for the palette's. */
        LucideIconEngine(QByteArray InSvg, const QColor& InColor) : Svg(std::move(InSvg)), Fixed(InColor) {}

        void paint(QPainter* InPainter, const QRect& InRect, const QIcon::Mode InMode, QIcon::State) override
        {
            QSvgRenderer Renderer(Tinted(ColorFor(InMode)));
            Renderer.render(InPainter, QRectF(InRect));
        }

        QPixmap pixmap(const QSize& InSize, const QIcon::Mode InMode, const QIcon::State InState) override
        {
            return scaledPixmap(InSize, InMode, InState, 1.0);
        }

        QPixmap scaledPixmap(const QSize& InSize, const QIcon::Mode InMode, QIcon::State, const qreal InScale) override
        {
            const QColor Color = ColorFor(InMode);
            const QSize  Pixels = InSize * InScale;
            const QString Key = QStringLiteral("%1/%2x%3/%4").arg(Color.rgba()).arg(Pixels.width()).arg(Pixels.height()).arg(InScale);
            if(const auto Found = Cache.constFind(Key); Found != Cache.constEnd())
            {
                return *Found;
            }

            QPixmap Pixmap(Pixels);
            Pixmap.fill(Qt::transparent);
            {
                QPainter Painter(&Pixmap);
                Painter.setRenderHint(QPainter::Antialiasing);
                QSvgRenderer Renderer(Tinted(Color));
                Renderer.render(&Painter, QRectF(QPointF(0, 0), QSizeF(Pixels)));
            }
            Pixmap.setDevicePixelRatio(InScale);

            /* A theme switch adds a colour; a handful of sizes per colour at most. */
            if(Cache.size() > 32)
            {
                Cache.clear();
            }
            Cache.insert(Key, Pixmap);
            return Pixmap;
        }

        QIconEngine* clone() const override { return new LucideIconEngine(Svg, Fixed); }
        QString key() const override { return QStringLiteral("NimbleLucideIconEngine"); }

    private:
        QColor ColorFor(const QIcon::Mode InMode) const
        {
            const QPalette Palette = QApplication::palette();
            if(Fixed.isValid() && InMode != QIcon::Disabled)
            {
                return Fixed;
            }
            switch(InMode)
            {
                case QIcon::Disabled: return Palette.color(QPalette::Disabled, QPalette::ButtonText);
                case QIcon::Selected: return Palette.color(QPalette::Active,   QPalette::HighlightedText);
                default:              return Palette.color(QPalette::Active,   QPalette::ButtonText);
            }
        }

        QByteArray Tinted(const QColor& InColor) const
        {
            QByteArray Out = Svg;
            Out.replace("currentColor", InColor.name(QColor::HexRgb).toLatin1());
            return Out;
        }

        QByteArray              Svg;
        QColor                  Fixed;
        QHash<QString, QPixmap> Cache;
    };
#endif // NIMBLEEDITOR_HAS_QTSVG
}

namespace EditorIcons
{
    QIcon Get(const EIcon InIcon)
    {
        return Get(InIcon, QColor());
    }

    QIcon Get(const EIcon InIcon, const QColor& InColor)
    {
        if(size_t(InIcon) >= Icons.size())
        {
            return QIcon();
        }
#if NIMBLEEDITOR_HAS_QTSVG
        QFile File(QStringLiteral(":/lucide/") + QLatin1String(Icons[size_t(InIcon)].Resource));
        if(File.open(QIODevice::ReadOnly))
        {
            return QIcon(new LucideIconEngine(File.readAll(), InColor));
        }
#else
        (void)InColor;
#endif
        return FallbackIcon(InIcon);
    }
}

#endif // WITH_EDITOR
