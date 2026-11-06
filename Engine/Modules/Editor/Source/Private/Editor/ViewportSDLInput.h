#ifndef NIMBLEEDITOR_VIEWPORTSDLINPUT_H
#define NIMBLEEDITOR_VIEWPORTSDLINPUT_H

#if WITH_EDITOR

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QTimer>

class GameWindow;
class QWidget;

/*
 * The viewport as an SDL window, so the game's input path -- GameWindow, SDL --
 * runs inside the Qt editor exactly as in the launcher. Qt keeps the window
 * and the message loop (WrapNativeWindow, SetHostOwnsMessageLoop); SDL reads
 * the messages Qt dispatches, and Qt still sees every key after SDL.
 *
 * Two things Qt gets in the way of, handled here:
 *
 *   FOCUS  Qt keeps Win32 focus on the top-level window, so key messages never
 *          reach the viewport's HWND. When Qt focuses the viewport (or it is
 *          clicked), the Win32 focus is moved there.
 *   TEXT   Qt consumes WM_CHAR without dispatching it, so SDL sees no text;
 *          each one is forwarded to GameWindowSystem::InjectTextInput.
 *
 * Used by Play-in-Editor (EditorContext) and the SDL Input Probe. One at a
 * time: SDL wraps a native window once. Stop before the viewport's native
 * window is destroyed.
 */
class ViewportSDLInput : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit ViewportSDLInput(QObject* InParent = nullptr);
    ~ViewportSDLInput() override;

    /* InViewport needs WA_NativeWindow. False, logged, if SDL refused. */
    bool Start(QWidget* InViewport);

    /* Unwraps (the HWND stays Qt's) and shuts SDL's video down. Idempotent. */
    void Stop();

    bool        IsRunning() const { return Window != nullptr; }
    GameWindow* GetWindow() const { return Window; }

    /* Drains what SDL has queued into the window's delegates. */
    void Pump();

    /* Pump on a timer (the probe) rather than on demand (Play pumps once a
     * frame, right before it latches). Off by default. */
    void SetAutoPump(bool bInAutoPump);

protected:
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;
    bool nativeEventFilter(const QByteArray& InEventType, void* InMessage, qintptr* OutResult) override;

private:
    void OnQtFocusChanged(QWidget* InOld, QWidget* InNow);
    void GiveNativeFocusToViewport();

    QWidget*    Viewport = nullptr;
    GameWindow* Window   = nullptr;
    bool        bSystemInitialized = false;
    QTimer      PumpTimer;

    /* The first half of a UTF-16 surrogate pair, awaiting its second WM_CHAR. */
    char16_t    HighSurrogate = 0;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_VIEWPORTSDLINPUT_H
