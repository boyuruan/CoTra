#pragma once

#include <cstdint>

namespace cotra {
inline uint32_t seed_target(
    uint32_t owner, uint32_t leader, const uint32_t* is_core_machine) {
  return is_core_machine[owner] ? owner : leader;
}
}  // namespace cotra
