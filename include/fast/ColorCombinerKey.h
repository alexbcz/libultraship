#pragma once

#include <compare>
#include <cstdint>

struct ColorCombinerKey {
    uint64_t combine_mode = 0;
    uint64_t options = 0;
    uint64_t shader_id = 0;

    auto operator<=>(const ColorCombinerKey&) const = default;
};
