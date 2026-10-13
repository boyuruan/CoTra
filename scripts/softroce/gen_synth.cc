// Write a uint8 base, held-out queries, and id-only ground truth.
// Points are an 8-d uniform latent mapped linearly into 32-d, so k-means
// parts are adjacent regions and 10-NN lists cross part boundaries.
// Ground truth is nq, k, then nq*k uint32 ids and no distances; ties break
// by smaller id.
#include <algorithm>
#include <cmath>
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
  const uint32_t n = 20000;
  const uint32_t dim = 32;
  const uint32_t latent = 8;
  const uint32_t nq = 200;
  const uint32_t k = 10;

  std::mt19937 rng(1);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  std::vector<double> mix(static_cast<size_t>(dim) * latent);
  std::vector<double> row_sum(dim, 0.0);
  for (uint32_t d = 0; d < dim; d++) {
    for (uint32_t j = 0; j < latent; j++) {
      mix[d * latent + j] = unit(rng);
      row_sum[d] += mix[d * latent + j];
    }
  }

  auto sample = [&](uint32_t count) {
    std::vector<uint8_t> out(static_cast<size_t>(count) * dim);
    std::vector<double> z(latent);
    for (uint32_t i = 0; i < count; i++) {
      for (uint32_t j = 0; j < latent; j++) z[j] = unit(rng);
      for (uint32_t d = 0; d < dim; d++) {
        double v = 0.0;
        for (uint32_t j = 0; j < latent; j++) v += mix[d * latent + j] * z[j];
        long q = std::lround(255.0 * v / row_sum[d]);
        out[static_cast<size_t>(i) * dim + d] =
            static_cast<uint8_t>(std::clamp(q, 0L, 255L));
      }
    }
    return out;
  };
  std::vector<uint8_t> base = sample(n);
  std::vector<uint8_t> query = sample(nq);

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
