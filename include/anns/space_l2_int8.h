#pragma once

#include <cstddef>
#include <cstdint>

// Include after either the search or vendored HNSW SpaceInterface definition.
// Both paths use the same signed-int8 distance implementation.
namespace hnswlib {

static int L2SqrII4x(
    const void *__restrict pVect1, const void *__restrict pVect2,
    const void *__restrict qty_ptr) {
  size_t qty = *((size_t *)qty_ptr);
  int res = 0;
  int8_t *a = (int8_t *)pVect1;
  int8_t *b = (int8_t *)pVect2;

  qty = qty >> 2;
  for (size_t i = 0; i < qty; i++) {
    res += ((*a) - (*b)) * ((*a) - (*b));
    a++;
    b++;
    res += ((*a) - (*b)) * ((*a) - (*b));
    a++;
    b++;
    res += ((*a) - (*b)) * ((*a) - (*b));
    a++;
    b++;
    res += ((*a) - (*b)) * ((*a) - (*b));
    a++;
    b++;
  }
  return (res);
}

static int L2SqrII(
    const void *__restrict pVect1, const void *__restrict pVect2,
    const void *__restrict qty_ptr) {
  size_t qty = *((size_t *)qty_ptr);
  int res = 0;
  int8_t *a = (int8_t *)pVect1;
  int8_t *b = (int8_t *)pVect2;

  for (size_t i = 0; i < qty; i++) {
    res += ((*a) - (*b)) * ((*a) - (*b));
    a++;
    b++;
  }
  return (res);
}

class L2SpaceII : public SpaceInterface<int> {
  DISTFUNC<int> fstdistfunc_;
  size_t data_size_;
  size_t dim_;

 public:
  L2SpaceII(size_t dim) {
    if (dim % 4 == 0) {
      fstdistfunc_ = L2SqrII4x;
    } else {
      fstdistfunc_ = L2SqrII;
    }
    dim_ = dim;
    data_size_ = dim * sizeof(int8_t);
  }

  size_t get_data_size() { return data_size_; }

  DISTFUNC<int> get_dist_func() { return fstdistfunc_; }

  void *get_dist_func_param() { return &dim_; }

  ~L2SpaceII() {}
};

}  // namespace hnswlib
