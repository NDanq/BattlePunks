#ifndef NIMBLEEDITOR_TEXTUREVIEWERWINDOW_H
#define NIMBLEEDITOR_TEXTUREVIEWERWINDOW_H

#if WITH_EDITOR

#include <QMainWindow>

#include "Public/RenderCore/TextureAsset.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class TexturePreview;

/*
 * A .nmtexture, opened from the Content Browser: the image at any mip level
 * and channel, zoomed and panned; its statistics (size, levels, memory on
 * disk and on the GPU, alpha, source); and its settings -- sRGB, filter,
 * wrap, mip generation and compression. A changed setting shows at once (the
 * mip chain is rebuilt in memory); Save writes the file, records the settings
 * in its import options so Reimport keeps them, and restages it, so the level
 * shows them; a compression is encoded then (RenderCore's
 * TextureCompression.h), or at once to preview it as compressed. The
 * engine's textures view only.
 *
 * One window per texture; opening it again brings that window forward.
 */
class TextureViewerWindow final : public QMainWindow
{
    Q_OBJECT

public:
    /* Shows InPath's viewer, making it if need be; nullptr (warned) if the
     * file is not a texture this editor can read. */
    static TextureViewerWindow* Open(const QString& InPath, QWidget* InParent);

    const QString& GetPath() const { return Path; }

protected:
    void closeEvent(QCloseEvent* InEvent) override;

private:
    explicit TextureViewerWindow(const QString& InPath);

    bool Load(QString& OutError);
    void BuildUi();
    void FillMipLevels();
    void FillControls();
    void UpdateInfo();
    void UpdatePreview();
    void UpdateState();
    void SettingChanged();
    bool EncodeEdited();
    bool IsDirty() const;
    bool Save();
    void Revert();

    QString                 Path;           // absolute, on disk
    QString                 RuntimePath;    // as a material names it
    bool                    bReadOnly = false;
    qint64                  FileBytes = 0;
    RenderCore::TextureData Saved;          // as on disk
    RenderCore::TextureData Edited;         // with the settings picked here, its mips rebuilt; its blocks
                                            // Saved's while those settings are, else made on demand

    TexturePreview* Preview      = nullptr;
    QLabel*         Info         = nullptr;
    QComboBox*      Level        = nullptr;
    QComboBox*      Channels     = nullptr;
    QCheckBox*      ShowEncoded  = nullptr;
    QCheckBox*      SRGB         = nullptr;
    QComboBox*      Filter       = nullptr;
    QComboBox*      Wrap         = nullptr;
    QComboBox*      MipGen       = nullptr;
    QComboBox*      Compression  = nullptr;
    QLabel*         CompressNote = nullptr;
    QLabel*         Status       = nullptr;
    QPushButton*    SaveButton   = nullptr;
    QPushButton*    RevertButton = nullptr;
    bool            bFilling     = false;   // FillControls: no SettingChanged
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_TEXTUREVIEWERWINDOW_H
