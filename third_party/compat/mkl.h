// Names this tree includes from Intel MKL, implemented with LP64 OpenBLAS.
// MKL_INT is 32-bit. A GEMM or LAPACK dimension must fit in int.
// This is not the cluster's ILP64 MKL, so partitions will not match a
// graph built there.
#pragma once

#include <cblas.h>
#include <lapacke.h>

typedef int MKL_INT;

extern "C" void openblas_set_num_threads(int num_threads);

static inline void mkl_set_num_threads(int num_threads) {
  openblas_set_num_threads(num_threads);
}
