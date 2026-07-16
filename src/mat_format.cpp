#include "mat_format.h"

#include <stdexcept>

namespace dolby {

std::vector<BYTE> UnswapMatTransportWords(const BYTE* data, const std::size_t bytes) {
    std::vector<BYTE> logical(bytes);
    std::size_t offset = 0;
    for (; offset + 1 < bytes; offset += 2) {
        logical[offset] = data[offset + 1];
        logical[offset + 1] = data[offset];
    }
    if (offset < bytes) logical[offset] = data[offset];
    return logical;
}

std::array<unsigned, 6> DecodeMatObjectFields(
    const std::vector<BYTE>& payload,
    const std::size_t tableOffset,
    const std::size_t objectIndex) {
    constexpr std::size_t fieldsPerObject = 6;
    constexpr std::size_t bitsPerField = 6;
    const std::size_t firstBit =
        (tableOffset + kMatPositionTablePrefix.size()) * 8 +
        objectIndex * fieldsPerObject * bitsPerField;
    if (firstBit + fieldsPerObject * bitsPerField > payload.size() * 8) {
        throw std::runtime_error("MAT payload is too short for the object table");
    }

    std::array<unsigned, fieldsPerObject> fields{};
    for (std::size_t field = 0; field < fields.size(); ++field) {
        unsigned value = 0;
        for (std::size_t bit = 0; bit < bitsPerField; ++bit) {
            const std::size_t position = firstBit + field * bitsPerField + bit;
            value = (value << 1) |
                    ((payload[position >> 3] >> (7 - (position & 7))) & 1U);
        }
        fields[field] = value;
    }
    return fields;
}

} // namespace dolby
