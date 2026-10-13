// Write a small uint8 base, query file, and id-only ground truth.
// Query i is an exact copy of a base vector, so brute-force L2 ranks that
// id first. Ground truth is nq, k, then nq*k uint32 ids and no distances.
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <utility>
#include <vector>

static void write_u32(std::ofstream &out, uint32_t v) {
  out.write(reinterpret_cast<char *>(&v), sizeof(v));
}

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : "/tmp/cotra-synth";
  const uint32_t n = 4096;
  const uint32_t dim = 32;
  const uint32_t nq = 32;
  const uint32_t k = 10;

  std::mt19937 rng(1);
  std::vector<uint8_t> base(static_cast<size_t>(n) * dim);
  for (uint32_t i = 0; i < n; i++) {
    int center = (i < n / 2) ? 32 : 220;
    for (uint32_t d = 0; d < dim; d++) {
      int v = center + static_cast<int>(rng() % 17) - 8;
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      base[static_cast<size_t>(i) * dim + d] = static_cast<uint8_t>(v);
    }
  }

  std::vector<uint32_t> qid(nq);
  std::vector<uint8_t> query(static_cast<size_t>(nq) * dim);
  for (uint32_t q = 0; q < nq; q++) {
    qid[q] = (q * n) / nq;
    for (uint32_t d = 0; d < dim; d++) {
      query[static_cast<size_t>(q) * dim + d] =
          base[static_cast<size_t>(qid[q]) * dim + d];
    }
  }

  std::vector<uint32_t> gt(static_cast<size_t>(nq) * k);
  std::vector<std::pair<uint32_t, uint32_t>> dist(n);
  for (uint32_t q = 0; q < nq; q++) {
    for (uint32_t i = 0; i < n; i++) {
      uint32_t sum = 0;
      for (uint32_t d = 0; d < dim; d++) {
        int diff = static_cast<int>(query[static_cast<size_t>(q) * dim + d]) -
                   static_cast<int>(base[static_cast<size_t>(i) * dim + d]);
        sum += static_cast<uint32_t>(diff * diff);
      }
      dist[i] = {sum, i};
    }
    std::partial_sort(dist.begin(), dist.begin() + k, dist.end());
    if (dist[0].second != qid[q] || dist[0].first != 0) {
      std::cerr << "synthetic gt did not rank the source id first\n";
      return 1;
    }
    for (uint32_t j = 0; j < k; j++) {
      gt[static_cast<size_t>(q) * k + j] = dist[j].second;
    }
  }

  std::ofstream base_out(dir + "/base.bin", std::ios::binary);
  std::ofstream query_out(dir + "/query.bin", std::ios::binary);
  std::ofstream gt_out(dir + "/gt.bin", std::ios::binary);
  std::ofstream meta_out(dir + "/meta.txt");
  if (!base_out || !query_out || !gt_out || !meta_out) {
    std::cerr << "cannot write synthetic files under " << dir << "\n";
    return 1;
  }
  write_u32(base_out, n);
  write_u32(base_out, dim);
  base_out.write(reinterpret_cast<char *>(base.data()), base.size());
  write_u32(query_out, nq);
  write_u32(query_out, dim);
  query_out.write(reinterpret_cast<char *>(query.data()), query.size());
  write_u32(gt_out, nq);
  write_u32(gt_out, k);
  gt_out.write(reinterpret_cast<char *>(gt.data()),
               gt.size() * sizeof(uint32_t));
  meta_out << n << " " << dim << " " << nq << " " << k << "\n";
  std::cout << "synthetic n=" << n << " dim=" << dim << " nq=" << nq
            << " k=" << k << "\n";
  return 0;
}
