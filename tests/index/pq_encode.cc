// Train PQ pivots on a random sample of a base file and encode every vector,
// for the scala_v3 PQ gate (--pq_prefix). Writes <prefix>_pivots.bin and
// <prefix>_codes.bin in DiskANN's format, codes in base-file order.
//
// usage: pq_encode <uint8|int8|float> <base.bin> <prefix> <chunks> [train_n]
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "partition.h"
#include "pq.h"
#include "pq_common.h"

template <typename T>
int run(
    const std::string &base, const std::string &prefix, uint32_t chunks,
    size_t train_n) {
  std::ifstream in(base, std::ios::binary);
  if (!in) {
    fprintf(stderr, "cannot open %s\n", base.c_str());
    return 1;
  }
  uint32_t npts, dim;
  in.read((char *)&npts, 4);
  in.read((char *)&dim, 4);
  in.close();
  printf("base %s: %u x %u, %u chunks\n", base.c_str(), npts, dim, chunks);

  std::string pivots = prefix + "_pivots.bin";
  std::string codes = prefix + "_codes.bin";
  // generate_pq_pivots keeps an existing file with the same shape.
  std::filesystem::remove(pivots);

  double p = std::min(1.0, (double)train_n / npts);
  float *train = nullptr;
  size_t n_train, train_dim;
  gen_random_slice<T>(base, p, train, n_train, train_dim);
  printf("training on %zu sampled vectors\n", n_train);
  if (diskann::generate_pq_pivots(
          train, n_train, dim, NUM_PQ_CENTROIDS, chunks, NUM_KMEANS_REPS_PQ,
          pivots, true) != 0) {
    fprintf(stderr, "generate_pq_pivots failed\n");
    return 1;
  }
  delete[] train;
  if (diskann::generate_pq_data_from_pivots<T>(
          base, NUM_PQ_CENTROIDS, chunks, pivots, codes) != 0) {
    fprintf(stderr, "generate_pq_data_from_pivots failed\n");
    return 1;
  }
  printf("wrote %s and %s\n", pivots.c_str(), codes.c_str());
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(
        stderr,
        "usage: %s <uint8|int8|float> <base.bin> <prefix> <chunks> "
        "[train_n=100000]\n",
        argv[0]);
    return 1;
  }
  std::string type = argv[1];
  uint32_t chunks = atoi(argv[4]);
  size_t train_n = argc > 5 ? atol(argv[5]) : 100000;
  if (type == "uint8") return run<uint8_t>(argv[2], argv[3], chunks, train_n);
  if (type == "int8") return run<int8_t>(argv[2], argv[3], chunks, train_n);
  if (type == "float") return run<float>(argv[2], argv[3], chunks, train_n);
  fprintf(stderr, "unknown data type %s\n", type.c_str());
  return 1;
}
