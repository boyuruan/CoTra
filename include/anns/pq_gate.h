#pragma once

#include "anns/pq_codes.h"

struct PqGate {
  const PQCodes *codes{nullptr};
  float scale{1.0f};
  bool shadow{false};

  bool drop(
      const char *query, uint32_t id, bool ef_fill, float bound, int channel,
      uint64_t *gated, uint64_t *distance_count) const {
    if (codes == nullptr || ef_fill) return false;
    if (distance_count != nullptr) (*distance_count)++;
    if (codes->dist(query, id) <= bound) return false;
    if (gated != nullptr) gated[channel]++;
    return !shadow;
  }
};
