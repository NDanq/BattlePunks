#ifndef NIMBLEEDITOR_SDLINPUTPROBE_H
#define NIMBLEEDITOR_SDLINPUTPROBE_H

#if WITH_EDITOR

#include <cstdint>

#include <QObject>
#include <QString>
#include <QTimer>

#include "Public/Core/CoreTypes.h"
#include "Private/Editor/ViewportSDLInput.h"

class GameWindow;
class QWidget;
struct KeyEvent;
struct MouseButtonEvent;
struct MouseMoveEvent;
struct MouseWheelEvent;

/*
 * Reports what the game's SDL input path delivers from the viewport: every
 * event reaching the GameWindowDelegates, through the same ViewportSDLInput
 * Play-in-Editor uses. A diagnostic; not usable while playing (one SDL wrap
 * per window).
 *
 * View > SDL Input Probe or --sdl-input-probe. Stop before the viewport's
 * native window is destroyed.
 */
class SDLInputProbe : public QObject
{
    Q_OBJECT

public:
    explicit SDLInputProbe(QObject* InParent = nullptr);
    ~SDLInputProbe() override;

    /* InViewport needs WA_NativeWindow. False, logged, if SDL refused. */
    bool Start(QWidget* InViewport);

    /* Unwraps the viewport. Idempotent. */
    void Stop();

    bool IsRunning() const { return Window != nullptr; }

signals:
    /* One line per discrete event; mouse motion summarised per second. */
    void EventReported(const QString& InLine);

private:
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

    ViewportSDLInput Input;
    QWidget*         Viewport = nullptr;
    GameWindow*      Window   = nullptr;   // Input's, while running

    QTimer MotionSummaryTimer;

    /* Motion is too chatty to report per event. */
    uint32_t          MotionEventsThisSecond = 0;
    QString           LastNativeFocus;

    EngineCore::Point LastMotionPos;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_SDLINPUTPROBE_H
