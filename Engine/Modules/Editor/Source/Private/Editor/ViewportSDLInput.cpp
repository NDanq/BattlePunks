#if WITH_EDITOR

#include "Private/Editor/ViewportSDLInput.h"

#include <QApplication>
#include <QEvent>
#include <QString>
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
#include "Public/GameWindow/GameWindowSystem.h"

namespace
{
    constexpr int AutoPumpIntervalMs = 8;   // twice per viewport frame
}

ViewportSDLInput::ViewportSDLInput(QObject* InParent)
    : QObject(InParent)
{
    PumpTimer.setTimerType(Qt::PreciseTimer);
    PumpTimer.setInterval(AutoPumpIntervalMs);
    connect(&PumpTimer, &QTimer::timeout, this, &ViewportSDLInput::Pump);
}

ViewportSDLInput::~ViewportSDLInput()
{
    Stop();
}

bool ViewportSDLInput::Start(QWidget* InViewport)
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
        CORE_LOG_ERROR("[Editor] SDL failed to initialise for the viewport's input")
        return false;
    }
    bSystemInitialized = true;

    Window = GameWindowSystem::WrapNativeWindow(EngineCore::WindowHandle(reinterpret_cast<void*>(Viewport->winId())));
    if(!Window)
    {
        CORE_LOG_ERROR("[Editor] SDL could not wrap the viewport for input")
        Stop();
        return false;
    }

    connect(qApp, &QApplication::focusChanged, this, &ViewportSDLInput::OnQtFocusChanged);
    Viewport->installEventFilter(this);
    qApp->installNativeEventFilter(this);
    if(Viewport->hasFocus())
    {
        GiveNativeFocusToViewport();
    }
    return true;
}

void ViewportSDLInput::Stop()
{
    PumpTimer.stop();
    disconnect(qApp, &QApplication::focusChanged, this, &ViewportSDLInput::OnQtFocusChanged);
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
    }
    if(bSystemInitialized)
    {
        GameWindowSystem::Shutdown();
        bSystemInitialized = false;
    }
}

void ViewportSDLInput::Pump()
{
    /* Only drains what SDL already queued; the return value (quit) is the
     * editor's to decide, not SDL's. */
    if(Window)
    {
        GameWindowSystem::PumpEvents();
    }
}

void ViewportSDLInput::SetAutoPump(const bool bInAutoPump)
{
    bInAutoPump ? PumpTimer.start() : PumpTimer.stop();
}

bool ViewportSDLInput::nativeEventFilter(const QByteArray& /*InEventType*/, void* InMessage, qintptr* /*OutResult*/)
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

void ViewportSDLInput::OnQtFocusChanged(QWidget* /*InOld*/, QWidget* InNow)
{
    if(Window && InNow == Viewport)
    {
        GiveNativeFocusToViewport();
    }
}

bool ViewportSDLInput::eventFilter(QObject* InWatched, QEvent* InEvent)
{
    if(Window && InWatched == Viewport
       && (InEvent->type() == QEvent::FocusIn || InEvent->type() == QEvent::MouseButtonPress))
    {
        GiveNativeFocusToViewport();
    }
    return QObject::eventFilter(InWatched, InEvent);
}

void ViewportSDLInput::GiveNativeFocusToViewport()
{
#if defined(_WIN32)
    const HWND Hwnd = reinterpret_cast<HWND>(Viewport->winId());
    if(GetFocus() != Hwnd)
    {
        SetFocus(Hwnd);
    }
#endif
}

#endif // WITH_EDITOR
