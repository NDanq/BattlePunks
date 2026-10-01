#ifndef NIMBLEEDITOR_SDLINPUTPROBE_H
#define NIMBLEEDITOR_SDLINPUTPROBE_H

#if WITH_EDITOR

#include <cstdint>

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QString>
#include <QTimer>

#include "Public/Core/CoreTypes.h"

class GameWindow;
class QWidget;
struct KeyEvent;
struct MouseButtonEvent;
struct MouseMoveEvent;
struct MouseWheelEvent;

/*
 * SPIKE: can the game's SDL input path run inside the Qt editor? Wraps the
 * viewport (WrapNativeWindow, Qt owning the loop), pumps SDL each tick and
 * reports what reaches the GameWindowDelegates.
 *
 * View > SDL Input Probe or --sdl-input-probe. Stop before the viewport's
 * native window is destroyed.
 */
class SDLInputProbe : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit SDLInputProbe(QObject* InParent = nullptr);
    ~SDLInputProbe() override;

    /* InViewport needs WA_NativeWindow. False, logged, if SDL refused. */
    bool Start(QWidget* InViewport);

    /* Unwraps and restores Qt's window procedure. Idempotent. */
    void Stop();

    bool IsRunning() const { return Window != nullptr; }

protected:
    /* Focus-in and mouse-press on the viewport: hand the Win32 focus over. */
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;

    /* TEXT. Qt consumes WM_CHAR without dispatching it, so SDL sees no text.
     * This filter forwards each one to GameWindowSystem::InjectTextInput. */
    bool nativeEventFilter(const QByteArray& InEventType, void* InMessage, qintptr* OutResult) override;

signals:
    /* One line per discrete event; mouse motion summarised per second. */
    void EventReported(const QString& InLine);

private:
    void Pump();

    /* KEYBOARD FOCUS. Qt keeps Win32 focus on the top-level window, so key
     * messages never reach the viewport HWND. When Qt focuses the viewport,
     * move the Win32 focus there; Qt still receives the keys after SDL. */
    void OnQtFocusChanged(QWidget* InOld, QWidget* InNow);
    void GiveNativeFocusToViewport();

    /* Which window holds the Win32 keyboard focus, for the report. */
    QString DescribeNativeFocus() const;

    static void OnKey(GameWindow*, const KeyEvent& InEvent, void* InUserData);
    static void OnText(GameWindow*, const char* InUtf8, void* InUserData);
    static void OnMouseButton(GameWindow*, const MouseButtonEvent& InEvent, void* InUserData);
    static void OnMouseMove(GameWindow*, const MouseMoveEvent& InEvent, void* InUserData);
    static void OnMouseWheel(GameWindow*, const MouseWheelEvent& InEvent, void* InUserData);
    static void OnFocusChanged(GameWindow*, bool bInHasFocus, void* InUserData);
    static void OnMouseFocusChanged(GameWindow*, bool bInHasMouse, void* InUserData);

    void Report(const QString& InLine);

    QWidget*    Viewport = nullptr;
    GameWindow* Window   = nullptr;
    bool        bSystemInitialized = false;

    QTimer PumpTimer;
    QTimer MotionSummaryTimer;

    /* Motion is too chatty to report per event. */
    uint32_t          MotionEventsThisSecond = 0;
    QString           LastNativeFocus;

    /* The first half of a UTF-16 surrogate pair, awaiting its second WM_CHAR. */
    char16_t          HighSurrogate = 0;
    EngineCore::Point LastMotionPos;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_SDLINPUTPROBE_H
