#ifndef NIMBLEEDITOR_COMPONENTDOCS_H
#define NIMBLEEDITOR_COMPONENTDOCS_H

#if WITH_EDITOR

#include <QString>

#include "Public/GameCore/ComponentType.h"

/*
 * The reference guide of a component: Engine/Docs/Components/<Name>.md, the
 * page behind the ? in a Details component header. A component without one
 * gets it written the first time it is asked for, from what the engine knows
 * of it at run time -- its editor properties (type, default, choices, asset
 * kind) and its Lua bindings -- in C++ and Lua halves. A file that exists is
 * never overwritten: edit a generated page and the edits stay.
 *
 * Only Engine/Docs is documentation (DocumentationWindow.h).
 */
namespace EditorDocs
{
    /* Engine/Docs, absolute; empty if the engine tree is not known (an
     * installed editor). A test may point it elsewhere with SetDocsRoot. */
    QString GetDocsRoot();
    void    SetDocsRoot(const QString& InRoot);

    /* Engine/Docs/Components/<name without spaces>.md ("Audio Source" ->
     * AudioSource.md); empty if there is no docs root. */
    QString GetComponentDocPath(const QString& InTypeName);

    /* The Markdown generated for InType: C++ and Lua API. */
    QString GenerateComponentMarkdown(GameCore::ComponentTypeID InType);

    /* The page's path, writing it first if need be; empty if it cannot be
     * (no docs root, unknown type, unwritable folder). */
    QString EnsureComponentDoc(GameCore::ComponentTypeID InType);

    /* EnsureComponentDoc for every registered component the editor offers
     * (those with a property list); the number written. */
    int GenerateMissingComponentDocs();
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_COMPONENTDOCS_H
