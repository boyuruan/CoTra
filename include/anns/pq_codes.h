#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

/**
 * PQ codes of every base vector, held by every machine so a neighbor on
 * another machine gets a distance estimate without communication.
 *
 * <prefix>_pivots.bin is DiskANN's pivots file: a 4-entry size_t offset
 * table, then 256 x dim pivots, the dim-entry centroid, and n_chunks + 1
 * chunk offsets, each as a bin (int32 rows, int32 cols, data).
 * <prefix>_codes.bin is uint32 npts, uint32 n_chunks, then uint8 codes in
 * base-file (label) order. Codes are kept in graph (pos) order.
 */
class PQCodes {
 public:
  enum DataType { U8, I8, F32 };

  uint32_t dim{0}, n_chunks{0};
  DataType type{U8};
  // 256 x dim, centroid added back, so a code is compared to the raw query.
  std::vector<float> pivots;
  std::vector<uint32_t> chunk_ofs;
  std::vector<uint8_t> codes;

  void load(
      const std::string &prefix, const std::string &data_type,
      const int *pos_label_map, size_t npts) {
    if (data_type == "uint8") {
      type = U8;
    } else if (data_type == "int8") {
      type = I8;
    } else {
      type = F32;
    }

    std::string pivots_path = prefix + "_pivots.bin";
    std::ifstream pf(pivots_path, std::ios::binary);
    if (!pf) fail("cannot open " + pivots_path);
    int32_t nr, nc;
    pf.read((char *)&nr, 4);
    pf.read((char *)&nc, 4);
    if (nr != 4 || nc != 1) fail("pivots offset table is not 4 x 1");
    uint64_t ofs[4];
    pf.read((char *)ofs, sizeof(ofs));

    pf.seekg(ofs[0]);
    pf.read((char *)&nr, 4);
    pf.read((char *)&nc, 4);
    if (nr != 256) fail("pivots file does not have 256 centers");
    dim = nc;
    pivots.resize((size_t)256 * dim);
    pf.read((char *)pivots.data(), pivots.size() * sizeof(float));

    pf.seekg(ofs[1]);
    pf.read((char *)&nr, 4);
    pf.read((char *)&nc, 4);
    if ((uint32_t)nr != dim || nc != 1) fail("centroid is not dim x 1");
    std::vector<float> centroid(dim);
    pf.read((char *)centroid.data(), dim * sizeof(float));

    pf.seekg(ofs[2]);
    pf.read((char *)&nr, 4);
    pf.read((char *)&nc, 4);
    if (nr < 2 || nc != 1) fail("chunk offsets are malformed");
    n_chunks = nr - 1;
    chunk_ofs.resize(nr);
    pf.read((char *)chunk_ofs.data(), nr * sizeof(uint32_t));
    if (!pf || chunk_ofs[n_chunks] != dim) fail("pivots file is truncated");

    for (size_t c = 0; c < 256; c++) {
      for (uint32_t d = 0; d < dim; d++) {
        pivots[c * dim + d] += centroid[d];
      }
    }

    std::string codes_path = prefix + "_codes.bin";
    std::ifstream cf(codes_path, std::ios::binary);
    if (!cf) fail("cannot open " + codes_path);
    uint32_t cn, cc;
    cf.read((char *)&cn, 4);
    cf.read((char *)&cc, 4);
    if (cn != npts || cc != n_chunks) {
      fail(
          "codes file is " + std::to_string(cn) + " x " + std::to_string(cc) +
          ", expected " + std::to_string(npts) + " x " +
          std::to_string(n_chunks));
    }
    std::vector<uint8_t> by_label((size_t)npts * n_chunks);
    cf.read((char *)by_label.data(), by_label.size());
    if (!cf) fail("codes file is truncated");

    codes.resize(by_label.size());
    for (size_t pos = 0; pos < npts; pos++) {
      size_t label = pos_label_map[pos];
      memcpy(
          codes.data() + pos * n_chunks, by_label.data() + label * n_chunks,
          n_chunks);
    }
    printf(
        "PQ codes: %zu vectors, dim %u, %u chunks, %zu bytes\n", npts, dim,
        n_chunks, codes.size());
  }

  // Squared L2 between the query and the PQ reconstruction of pos.
  float dist(const char *query, uint32_t pos) const {
    switch (type) {
      case U8:
        return adc((const uint8_t *)query, pos);
      case I8:
        return adc((const int8_t *)query, pos);
      default:
        return adc((const float *)query, pos);
    }
  }

 private:
  template <typename T>
  float adc(const T *q, uint32_t pos) const {
    const uint8_t *c = codes.data() + (size_t)pos * n_chunks;
    float s = 0;
    for (uint32_t k = 0; k < n_chunks; k++) {
      const float *p = pivots.data() + (size_t)c[k] * dim;
      for (uint32_t d = chunk_ofs[k]; d < chunk_ofs[k + 1]; d++) {
        float x = (float)q[d] - p[d];
        s += x * x;
      }
    }
    return s;
  }

  static void fail(const std::string &msg) {
    fprintf(stderr, "PQCodes: %s\n", msg.c_str());
    abort();
  }
};
