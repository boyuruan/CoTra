// Read the N <prefix><m>_final_scala.data files and the id-only ground truth.
// Fails unless every id 0..n-1 is served by exactly one machine. Prints the
// share of GT ids not on the machine that issues the query under scala_v3
// (q / (nq / N)), and not on the machine holding most of that query's GT.
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 5) {
    std::cerr << "usage: check_remote <index_prefix> <N> <gt.bin> <dim>\n";
    return 2;
  }
  const std::string prefix = argv[1];
  const uint32_t parts = std::atoi(argv[2]);
  const std::string gt_path = argv[3];
  const uint32_t dim = std::atoi(argv[4]);
  const size_t rec = dim + sizeof(uint64_t);

  std::vector<int> owner;
  uint64_t all_num = 0;
  std::cout << "parts";
  for (uint32_t m = 0; m < parts; m++) {
    std::string path = prefix + std::to_string(m) + "_final_scala.data";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      std::cerr << "cannot open " << path << "\n";
      return 1;
    }
    uint32_t pid, pnum, pvec;
    uint64_t all;
    in.read(reinterpret_cast<char *>(&pid), 4);
    in.read(reinterpret_cast<char *>(&pnum), 4);
    in.read(reinterpret_cast<char *>(&pvec), 4);
    in.read(reinterpret_cast<char *>(&all), 8);
    if (pid != m || pnum != parts || (m > 0 && all != all_num)) {
      std::cerr << "\n" << path << ": header pid " << pid << " pnum " << pnum
                << " all " << all << "\n";
      return 1;
    }
    if (m == 0) {
      all_num = all;
      owner.assign(all_num, -1);
    }
    std::vector<char> buf(rec * (static_cast<size_t>(pvec) + 1));
    in.read(buf.data(), buf.size());
    if (!in) {
      std::cerr << "\n" << path << ": short read\n";
      return 1;
    }
    // Record 0 is the entry point copy; the partition starts at record 1.
    for (uint32_t i = 1; i <= pvec; i++) {
      uint64_t label =
          *reinterpret_cast<uint64_t *>(buf.data() + rec * i + dim);
      if (label >= all_num || owner[label] != -1) {
        std::cerr << "\n" << path << ": label " << label
                  << (label >= all_num ? " out of range" : " repeated")
                  << "\n";
        return 1;
      }
      owner[label] = static_cast<int>(m);
    }
    std::cout << " " << pvec;
  }
  std::cout << "\n";
  for (uint64_t v = 0; v < all_num; v++) {
    if (owner[v] < 0) {
      std::cerr << "id " << v << " is on no machine\n";
      return 1;
    }
  }

  std::ifstream gin(gt_path, std::ios::binary);
  uint32_t nq, k;
  gin.read(reinterpret_cast<char *>(&nq), 4);
  gin.read(reinterpret_cast<char *>(&k), 4);
  std::vector<uint32_t> gt(static_cast<size_t>(nq) * k);
  gin.read(reinterpret_cast<char *>(gt.data()), gt.size() * 4);
  if (!gin || nq % parts != 0) {
    std::cerr << "bad gt file or nq " << nq << " not divisible by " << parts
              << "\n";
    return 1;
  }

  const uint32_t per = nq / parts;
  size_t off_issuer = 0, off_majority = 0, multi = 0;
  for (uint32_t q = 0; q < nq; q++) {
    std::vector<uint32_t> cnt(parts, 0);
    for (uint32_t j = 0; j < k; j++) cnt[owner[gt[q * k + j]]]++;
    uint32_t issuer = q / per, best = 0, spread = 0;
    for (uint32_t m = 0; m < parts; m++) {
      if (cnt[m] > best) best = cnt[m];
      if (cnt[m]) spread++;
    }
    off_issuer += k - cnt[issuer];
    off_majority += k - best;
    if (spread > 1) multi++;
  }
  const double total = static_cast<double>(nq) * k;
  std::cout << "remote_gt_share_off_issuer " << off_issuer / total << "\n";
  std::cout << "remote_gt_share_off_majority " << off_majority / total << "\n";
  std::cout << "queries_with_gt_on_several_machines " << multi << " of " << nq
            << "\n";
  return 0;
}
