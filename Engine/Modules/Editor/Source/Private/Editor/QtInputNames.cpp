#if WITH_EDITOR

#include "Private/Editor/QtInputNames.h"

const char* QtKeyToInputName(const int InKey)
{
    static const char* const Letters[] = { "A","B","C","D","E","F","G","H","I","J","K","L","M",
                                           "N","O","P","Q","R","S","T","U","V","W","X","Y","Z" };
    static const char* const Digits[]  = { "0","1","2","3","4","5","6","7","8","9" };
    static const char* const FKeys[]   = { "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12" };
    if(InKey >= Qt::Key_A && InKey <= Qt::Key_Z)   { return Letters[InKey - Qt::Key_A]; }
    if(InKey >= Qt::Key_0 && InKey <= Qt::Key_9)   { return Digits[InKey - Qt::Key_0]; }
    if(InKey >= Qt::Key_F1 && InKey <= Qt::Key_F12) { return FKeys[InKey - Qt::Key_F1]; }
    switch(InKey)
    {
        case Qt::Key_Space:     return "Space";
        case Qt::Key_Return:    return "Return";
        case Qt::Key_Enter:     return "KP_Enter";
        case Qt::Key_Escape:    return "Escape";
        case Qt::Key_Tab:       return "Tab";
        case Qt::Key_Backspace: return "Backspace";
        case Qt::Key_Left:      return "Left";
        case Qt::Key_Right:     return "Right";
        case Qt::Key_Up:        return "Up";
        case Qt::Key_Down:      return "Down";
        case Qt::Key_Shift:     return "LeftShift";
        case Qt::Key_Control:   return "LeftCtrl";
        case Qt::Key_Alt:       return "LeftAlt";
        case Qt::Key_Insert:    return "Insert";
        case Qt::Key_Delete:    return "Delete";
        case Qt::Key_Home:      return "Home";
        case Qt::Key_End:       return "End";
        case Qt::Key_PageUp:    return "PageUp";
        case Qt::Key_PageDown:  return "PageDown";
        default:                return nullptr;
    }
}

const char* QtButtonToInputName(const Qt::MouseButton InButton)
{
    switch(InButton)
    {
        case Qt::LeftButton:   return "Mouse.Left";
        case Qt::MiddleButton: return "Mouse.Middle";
        case Qt::RightButton:  return "Mouse.Right";
        case Qt::BackButton:   return "Mouse.X1";
        case Qt::ForwardButton:return "Mouse.X2";
        default:               return nullptr;
    }
}

#endif // WITH_EDITOR
