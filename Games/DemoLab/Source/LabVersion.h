#ifndef DEMOLAB_LABVERSION_H
#define DEMOLAB_LABVERSION_H

#include <cstdint>

/* The serializer version of every Demo Lab component. */
namespace LabVersion {
    enum class EVersion : uint16_t {
        ELV_Initial = 0x00,
        //Release Version Bump, declare new releases above this line.
        ELV_Current
    };

    inline uint16_t GetCurrentVersion() { return static_cast<uint16_t>(EVersion::ELV_Current); }
}

#endif //DEMOLAB_LABVERSION_H
