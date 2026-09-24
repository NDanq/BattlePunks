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
    constexpr int PumpIntervalMs = 8;   // twice per viewport frame; latency is what is being judged

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
    PumpTimer.setTimerType(Qt::PreciseTimer);
    PumpTimer.setInterval(PumpIntervalMs);
    connect(&PumpTimer, &QTimer::timeout, this, &SDLInputProbe::Pump);

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
    if(!InViewport)
    {
        return false;
    }
    Viewport = InViewport;

    /* Before Initialize: Qt dispatches the Win32 messages; SDL must not run a
     * second message loop inside its own pump. */
    GameWindowSystem::SetHostOwnsMessageLoop(true);
    if(!GameWindowSystem::Initialize())
    {
        Report(QStringLiteral("SDL failed to initialise -- see the log"));
        return false;
    }
    bSystemInitialized = true;

    Window = GameWindowSystem::WrapNativeWindow(EngineCore::WindowHandle(reinterpret_cast<void*>(Viewport->winId())));
    if(!Window)
    {
        Report(QStringLiteral("SDL could not wrap the viewport -- see the log"));
        Stop();
        return false;
    }

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

    connect(qApp, &QApplication::focusChanged, this, &SDLInputProbe::OnQtFocusChanged);
    Viewport->installEventFilter(this);
    qApp->installNativeEventFilter(this);
    if(Viewport->hasFocus())
    {
        GiveNativeFocusToViewport();
    }

    PumpTimer.start();
    MotionSummaryTimer.start();
    Report(QStringLiteral("SDL input probe running on the viewport (window id %1)").arg(Window->GetID()));
    return true;
}

void SDLInputProbe::Stop()
{
    PumpTimer.stop();
    MotionSummaryTimer.stop();
    disconnect(qApp, &QApplication::focusChanged, this, &SDLInputProbe::OnQtFocusChanged);
    if(Viewport)
    {
        Viewport->removeEventFilter(this);
    }

    qApp->removeNativeEventFilter(this);
    HighSurrogate = 0;

    if(Window)
    {
        Window->ClearDelegates();
        GameWindowSystem::DestroyGameWindow(Window);   // detaches SDL; the HWND stays Qt's
        Window = nullptr;
        Report(QStringLiteral("SDL input probe stopped"));
    }
    if(bSystemInitialized)
    {
        GameWindowSystem::Shutdown();
        bSystemInitialized = false;
    }
}

void SDLInputProbe::Pump()
{
    /* The game loop's own call. With the host owning the message loop this
     * only drains what SDL's chained window procedure has already queued. The
     * return value ("keep running") is the host's business, not the probe's. */
    GameWindowSystem::PumpEvents();
}

bool SDLInputProbe::nativeEventFilter(const QByteArray& /*InEventType*/, void* InMessage, qintptr* /*OutResult*/)
{
#if defined(_WIN32)
    const MSG* Msg = static_cast<const MSG*>(InMessage);
    if(!Window || Msg->message != WM_CHAR || Msg->hwnd != reinterpret_cast<HWND>(Viewport->winId()))
    {
        return false;
    }

    /* UTF-16 code units, one per WM_CHAR; a character outside the BMP comes
     * as a surrogate pair across two messages. */
    const char16_t Unit = char16_t(Msg->wParam);
    if(Unit >= 0xD800 && Unit <= 0xDBFF)
    {
        HighSurrogate = Unit;
        return false;
    }

    QString Character;
    if(Unit >= 0xDC00 && Unit <= 0xDFFF && HighSurrogate != 0)
    {
        const QChar Pair[2] = { QChar(HighSurrogate), QChar(Unit) };
        Character = QString(Pair, 2);
    }
    else
    {
        Character = QString(QChar(Unit));
    }
    HighSurrogate = 0;

    GameWindowSystem::InjectTextInput(Window, Character.toUtf8().constData());
#else
    (void)InMessage;
#endif
    return false;   // Qt still handles its own key event
}

void SDLInputProbe::OnQtFocusChanged(QWidget* /*InOld*/, QWidget* InNow)
{
    if(Window && InNow == Viewport)
    {
        GiveNativeFocusToViewport();
    }
}

bool SDLInputProbe::eventFilter(QObject* InWatched, QEvent* InEvent)
{
    if(Window && InWatched == Viewport
       && (InEvent->type() == QEvent::FocusIn || InEvent->type() == QEvent::MouseButtonPress))
    {
        GiveNativeFocusToViewport();
    }
    return QObject::eventFilter(InWatched, InEvent);
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

void SDLInputProbe::GiveNativeFocusToViewport()
{
#if defined(_WIN32)
    const HWND Hwnd = reinterpret_cast<HWND>(Viewport->winId());
    if(GetFocus() != Hwnd)
    {
        SetFocus(Hwnd);
    }
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
