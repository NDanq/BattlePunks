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
 * THE SPIKE for Play-in-Editor input: can the game's own SDL input path run
 * inside the Qt editor, so PIE needs no second (Qt -> game) input layer?
 *
 * Wraps the viewport's native window with GameWindowSystem::WrapNativeWindow
 * -- SDL's window procedure chained in front of Qt's -- with Qt left owning the
 * message loop (SetHostOwnsMessageLoop). Each tick drains SDL's queue through
 * the ordinary GameWindowSystem::PumpEvents(), and reports what arrives on the
 * very GameWindowDelegates a game subscribes to. Nothing here is PIE; it only
 * answers whether the events arrive, with focus and capture intact, while Qt
 * keeps working.
 *
 * Enabled from View > SDL Input Probe, or --sdl-input-probe. Must be stopped
 * before the viewport's native window is destroyed (EditorMainWindow does).
 */
class SDLInputProbe : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit SDLInputProbe(QObject* InParent = nullptr);
    ~SDLInputProbe() override;

    /* Wraps InViewport's native window. False (with the reason logged) if
     * SDL refused. InViewport must have a native window (WA_NativeWindow). */
    bool Start(QWidget* InViewport);

    /* Unwraps and restores Qt's window procedure. Idempotent. */
    void Stop();

    bool IsRunning() const { return Window != nullptr; }

protected:
    /* Focus-in and mouse-press on the viewport: hand the Win32 focus over. */
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;

    /*
     * TEXT. Qt pulls each WM_CHAR off the queue while translating the
     * WM_KEYDOWN before it, and never dispatches it -- measured: every WM_CHAR
     * is fetched, none reaches the viewport's window procedure -- so SDL's
     * chained procedure makes no text. The native filter still sees each one
     * as Qt fetches it, and hands the character to
     * GameWindowSystem::InjectTextInput, which queues an ordinary SDL text
     * event: the game still receives it through PumpEvents and OnText.
     */
    bool nativeEventFilter(const QByteArray& InEventType, void* InMessage, qintptr* OutResult) override;

signals:
    /* One line per discrete event (key, button, wheel, focus); mouse motion
     * is summarised once per second rather than per event. */
    void EventReported(const QString& InLine);

private:
    void Pump();

    /*
     * KEYBOARD FOCUS, the one thing wrapping alone does not give. Qt keeps
     * the Win32 focus on the TOP-LEVEL window and routes key events to widgets
     * internally, so no WM_KEYDOWN ever reaches the viewport's own HWND -- and
     * SDL's chained procedure, which lives on that HWND, sees none. Mouse
     * messages are hit-tested to the child and arrive regardless.
     *
     * So whenever Qt gives the viewport focus, the Win32 focus follows it to
     * the viewport's HWND. Key messages then pass SDL's procedure first and
     * continue to Qt's procedure for that same window, which still delivers
     * them to the focused widget -- Qt keeps working.
     */
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
