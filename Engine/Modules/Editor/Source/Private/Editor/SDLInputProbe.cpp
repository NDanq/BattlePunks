#if WITH_EDITOR

#include "Private/Editor/SDLInputProbe.h"

#include <QApplication>
#include <QEvent>
#include <QWidget>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

#include "Public/Core/CoreLog.h"
#include "Public/GameWindow/GameWindow.h"
#include "Public/GameWindow/GameWindowDelegates.h"
#include "Public/GameWindow/GameWindowInput.h"
#include "Public/GameWindow/GameWindowSystem.h"

namespace
{
    QString DescribeKey(const EKeyCode InKey)
    {
        if(InKey >= KC_A && InKey <= KC_Z) { return QString(QChar('A' + (InKey - KC_A))); }
        if(InKey >= KC_0 && InKey <= KC_9) { return QString(QChar('0' + (InKey - KC_0))); }
        switch(InKey)
        {
            case KC_Escape: return QStringLiteral("Escape");
            case KC_Space:  return QStringLiteral("Space");
            case KC_Return: return QStringLiteral("Return");
            case KC_Tab:    return QStringLiteral("Tab");
            default:        return QStringLiteral("key %1").arg(int(InKey));
        }
    }

    QString DescribeModifiers(const uint16_t InModifiers)
    {
        QString Out;
        if(InModifiers & MOD_Ctrl)  { Out += QStringLiteral("Ctrl+"); }
        if(InModifiers & MOD_Alt)   { Out += QStringLiteral("Alt+"); }
        if(InModifiers & MOD_Shift) { Out += QStringLiteral("Shift+"); }
        return Out;
    }
}

SDLInputProbe::SDLInputProbe(QObject* InParent)
    : QObject(InParent)
{
    MotionSummaryTimer.setInterval(1000);
    connect(&MotionSummaryTimer, &QTimer::timeout, this, [this]()
    {
        const QString NativeFocus = DescribeNativeFocus();
        if(NativeFocus != LastNativeFocus)
        {
            Report(QStringLiteral("win32 focus: %1").arg(NativeFocus));
            LastNativeFocus = NativeFocus;
        }
        if(MotionEventsThisSecond > 0)
        {
            Report(QStringLiteral("mouse move x%1 (last at %2,%3)")
                   .arg(MotionEventsThisSecond).arg(LastMotionPos.X).arg(LastMotionPos.Y));
            MotionEventsThisSecond = 0;
        }
    });
}

SDLInputProbe::~SDLInputProbe()
{
    Stop();
}

bool SDLInputProbe::Start(QWidget* InViewport)
{
    if(Window)
    {
        return true;
    }
    Viewport = InViewport;
    if(!Input.Start(InViewport))
    {
        Report(QStringLiteral("SDL could not take the viewport's input -- see the log"));
        return false;
    }
    Window = Input.GetWindow();

    GameWindowDelegates Delegates;
    Delegates.UserData            = this;
    Delegates.OnKey               = &SDLInputProbe::OnKey;
    Delegates.OnText              = &SDLInputProbe::OnText;
    Delegates.OnMouseButton       = &SDLInputProbe::OnMouseButton;
    Delegates.OnMouseMove         = &SDLInputProbe::OnMouseMove;
    Delegates.OnMouseWheel        = &SDLInputProbe::OnMouseWheel;
    Delegates.OnFocusChanged      = &SDLInputProbe::OnFocusChanged;
    Delegates.OnMouseFocusChanged = &SDLInputProbe::OnMouseFocusChanged;
    Window->SetDelegates(Delegates);

    /* Text events only flow while text input is on -- a game turns it on for a
     * chat box; the probe wants to see that they can. */
    Window->SetTextInputActive(true);

    Input.SetAutoPump(true);
    MotionSummaryTimer.start();
    Report(QStringLiteral("SDL input probe running on the viewport (window id %1)").arg(Window->GetID()));
    return true;
}

void SDLInputProbe::Stop()
{
    MotionSummaryTimer.stop();
    const bool bWasRunning = Window != nullptr;
    Window = nullptr;
    Input.Stop();
    if(bWasRunning)
    {
        Report(QStringLiteral("SDL input probe stopped"));
    }
}

QString SDLInputProbe::DescribeNativeFocus() const
{
#if defined(_WIN32)
    const HWND Focus = GetFocus();
    if(!Focus)                                              { return QStringLiteral("none"); }
    if(Focus == reinterpret_cast<HWND>(Viewport->winId()))  { return QStringLiteral("viewport"); }
    if(Focus == reinterpret_cast<HWND>(Viewport->window()->winId())) { return QStringLiteral("top-level"); }
    return QStringLiteral("other");
#else
    return QStringLiteral("n/a");
#endif
}

void SDLInputProbe::Report(const QString& InLine)
{
    CORE_LOG("[SDLInputProbe] " + InLine.toStdString())
    emit EventReported(InLine);
}

// === Delegates ================================================================

void SDLInputProbe::OnKey(GameWindow*, const KeyEvent& InEvent, void* InUserData)
{
    auto* Self = static_cast<SDLInputProbe*>(InUserData);
    Self->Report(QStringLiteral("key %1%2 %3%4")
                 .arg(DescribeModifiers(InEvent.Modifiers), DescribeKey(InEvent.Key),
                      InEvent.bPressed ? QStringLiteral("down") : QStringLiteral("up"),
                      InEvent.bRepeat ? QStringLiteral(" (repeat)") : QString()));
}

void SDLInputProbe::OnText(GameWindow*, const char* InUtf8, void* InUserData)
{
    static_cast<SDLInputProbe*>(InUserData)->Report(QStringLiteral("text \"%1\"").arg(QString::fromUtf8(InUtf8)));
}

void SDLInputProbe::OnMouseButton(GameWindow*, const MouseButtonEvent& InEvent, void* InUserData)
{
    static const char* const Names[] = { "left", "middle", "right", "x1", "x2" };
    const int Index = int(InEvent.Button);
    static_cast<SDLInputProbe*>(InUserData)->Report(
        QStringLiteral("mouse %1%2 %3 at %4,%5")
        .arg(DescribeModifiers(InEvent.Modifiers),
             QString::fromLatin1(Index >= 0 && Index < 5 ? Names[Index] : "?"),
             InEvent.bPressed ? QStringLiteral("down") : QStringLiteral("up"))
        .arg(InEvent.Pos.X).arg(InEvent.Pos.Y));
}

void SDLInputProbe::OnMouseMove(GameWindow*, const MouseMoveEvent& InEvent, void* InUserData)
{
    auto* Self = static_cast<SDLInputProbe*>(InUserData);
    ++Self->MotionEventsThisSecond;
    Self->LastMotionPos = InEvent.Pos;
}

void SDLInputProbe::OnMouseWheel(GameWindow*, const MouseWheelEvent& InEvent, void* InUserData)
{
    static_cast<SDLInputProbe*>(InUserData)->Report(QStringLiteral("wheel %1").arg(double(InEvent.DeltaY)));
}

void SDLInputProbe::OnFocusChanged(GameWindow*, const bool bInHasFocus, void* InUserData)
{
    static_cast<SDLInputProbe*>(InUserData)->Report(bInHasFocus ? QStringLiteral("keyboard focus gained")
                                                                : QStringLiteral("keyboard focus lost"));
}

void SDLInputProbe::OnMouseFocusChanged(GameWindow*, const bool bInHasMouse, void* InUserData)
{
    static_cast<SDLInputProbe*>(InUserData)->Report(bInHasMouse ? QStringLiteral("mouse entered")
                                                                : QStringLiteral("mouse left"));
}

#endif // WITH_EDITOR
