#ifndef SEQUENCER_SEQUENCEFILE_H
#define SEQUENCER_SEQUENCEFILE_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/Sequencer/SequenceAsset.h"
#include "Public/Sequencer/SequencerAPI.h"

/*
 * .nmseq files: XML as authored (the editor's, and readable in a diff),
 * binary as cooked (a game's). One extension; the first bytes tell them
 * apart, as .nmui's do. A Shipping build has no XML parser and reads only the
 * cooked form.
 *
 * Every read returns a normalized asset (SequenceAsset::Normalize).
 */
namespace Sequencer
{
    enum class ESequenceFileFormat : uint8_t { Unknown, Text, Binary };

    SEQUENCER_API ESequenceFileFormat DetectSequenceFileFormat(const std::vector<uint8_t>& InBytes);

    /* InPath through the file provider, either form (text in uncooked builds
     * only). False with OutError. */
    SEQUENCER_API bool LoadSequence(const std::string& InPath, SequenceAsset& OutAsset, std::string* OutError = nullptr);
    SEQUENCER_API bool ReadSequence(const std::vector<uint8_t>& InBytes, SequenceAsset& OutAsset, std::string* OutError = nullptr);

    /* The cooked form. Deterministic. */
    SEQUENCER_API std::vector<uint8_t> WriteSequenceBinary(const SequenceAsset& InAsset);
    SEQUENCER_API bool ReadSequenceBinary(const std::vector<uint8_t>& InBytes, SequenceAsset& OutAsset, std::string* OutError = nullptr);

#if WITH_UNCOOKED
    /* The XML: read, and written deterministically -- the same asset always
     * writes the same bytes. */
    SEQUENCER_API bool        ParseSequenceXml(const std::string& InText, SequenceAsset& OutAsset, std::string* OutError = nullptr);
    SEQUENCER_API std::string WriteSequenceXml(const SequenceAsset& InAsset);

    /* To InDiskPath through a .tmp file and a rename. */
    SEQUENCER_API bool SaveSequenceXml(const std::string& InDiskPath, const SequenceAsset& InAsset, std::string* OutError = nullptr);

    /* Reads InSourceDiskPath (XML) and writes its cooked form to InOutDiskPath. */
    SEQUENCER_API bool CookSequenceFile(const std::string& InSourceDiskPath, const std::string& InOutDiskPath, std::string* OutError = nullptr);
#endif
}

#endif //SEQUENCER_SEQUENCEFILE_H
