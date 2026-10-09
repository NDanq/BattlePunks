#ifndef NIMBLEEDITOR_QTINPUTNAMES_H
#define NIMBLEEDITOR_QTINPUTNAMES_H

#if WITH_EDITOR

#include <Qt>

/*
 * Qt's keys and mouse buttons as the engine's input key names
 * (NimbleEngine/Input.h): Play-in-Editor's fallback when SDL could not take
 * the viewport, and Project Settings > Input's "press a key". Qt keys are
 * layout-dependent, unlike SDL's physical ones -- one reason SDL is the
 * normal path in play. nullptr for one the engine has no name for.
 */
const char* QtKeyToInputName(int InKey);
const char* QtButtonToInputName(Qt::MouseButton InButton);

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_QTINPUTNAMES_H
