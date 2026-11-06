#ifndef UI_UIFILE_H
#define UI_UIFILE_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/UI/UIAPI.h"
#include "Public/UI/UIDocument.h"

/*
 * .nmui files: XML as authored (the editor's), binary as cooked (a game's).
 * One extension; the first bytes tell them apart, as .nmworld's do. A game
 * build has no XML parser and reads only the cooked form -- `cook ui` in the
 * editor's console writes it.
 *
 * The cooked form is the asset with its styles applied: the root's settings,
 * then each widget's type and properties as text, then the reserved sections
 * as written, each in a chunk (CoreArchive) a newer build may extend.
 */
namespace UI
{
    enum class EUIFileFormat : uint8_t { Unknown, Text, Binary };

    UI_API EUIFileFormat DetectUIFileFormat(const std::vector<uint8_t>& InBytes);

    /* InPath through the file provider, either form (text in editor builds
     * only). False with OutError. */
    UI_API bool LoadUIDocument(const std::string& InPath, UIDocumentAsset& OutAsset, std::string* OutError = nullptr);
    UI_API bool ReadUIDocument(const std::vector<uint8_t>& InBytes, UIDocumentAsset& OutAsset, std::string* OutError = nullptr);

    /* The cooked form; InAsset's styles applied on the way. Deterministic. */
    UI_API std::vector<uint8_t> WriteUIBinary(const UIDocumentAsset& InAsset);
    UI_API bool ReadUIBinary(const std::vector<uint8_t>& InBytes, UIDocumentAsset& OutAsset, std::string* OutError = nullptr);

#if WITH_UNCOOKED
    /* The XML -- the editor's, and a Debug or RelWithDebInfo game's run
     * uncooked: read, and written deterministically -- the same asset always
     * writes the same bytes, attributes in the order read. */
    UI_API bool        ParseUIXml(const std::string& InText, UIDocumentAsset& OutAsset, std::string* OutError = nullptr);
    UI_API std::string WriteUIXml(const UIDocumentAsset& InAsset);

    /* To InDiskPath through a .tmp file and a rename. */
    UI_API bool SaveUIXml(const std::string& InDiskPath, const UIDocumentAsset& InAsset, std::string* OutError = nullptr);

    /* Reads InSourceDiskPath (XML) and writes its cooked form to InOutDiskPath. */
    UI_API bool CookUIFile(const std::string& InSourceDiskPath, const std::string& InOutDiskPath, std::string* OutError = nullptr);

    /* InIndex's subtree as XML (the editor's copy), and back: the widgets of
     * a fragment, in document order, the first's Parent -1. */
    UI_API std::string WriteUIXmlFragment(const UIDocumentAsset& InAsset, int32_t InIndex);
    UI_API bool        ParseUIXmlFragment(const std::string& InText, std::vector<UIWidgetDesc>& OutWidgets, std::string* OutError = nullptr);
#endif
}

#endif //UI_UIFILE_H
