#include "index/scala_build.h"

void ScalaBuild::encode_intermediate_graph() {
  std::cout << "Start transfer index to b1graph.\n";
  size_t read_blk_size = 64 * 1024 * 1024;
  if (!std::filesystem::exists(index_param.final_index_path)) {
    std::cerr << "Error: final_index_path does not exist.\n";
    abort();
  }

  cached_ifstream reader(index_param.final_index_path, read_blk_size);
  size_t fsize = reader.get_file_size();
  if (fsize == 0) {
    std::stringstream stream;
    stream << "Error, new file specified is empty. Not appending. "
           << std::endl;
    throw diskann::ANNException(stream.str(), -1);
  }

  char *final_idx_ptr = (char *)malloc(fsize);
  size_t num_blocks = DIV_ROUND_UP(fsize, read_blk_size);
  for (uint64_t i = 0; i < num_blocks; i++) {
    size_t cur_block_size = read_blk_size > fsize - (i * read_blk_size)
                                ? fsize - (i * read_blk_size)
                                : read_blk_size;
    reader.read(final_idx_ptr + i * read_blk_size, cur_block_size);
  }

  uint64_t vec_num =
      index_param.workspace.partition_size[index_param.machine_id];
  uint64_t b1_ele_size = sizeof(uint32_t) +
                         index_param.max_degree * sizeof(uint32_t) +
                         index_param.vec_size + sizeof(uint64_t);
  uint64_t merge_graph_size = sizeof(uint64_t) +
                              sizeof(index_param.workspace.medoid) +
                              b1_ele_size * vec_num;

  size_t merge_meta_offset = 0;
  char *merge_graph = (char *)malloc(merge_graph_size);
  if (merge_graph == nullptr) {
    std::cerr << "Error: failed to malloc b1graph mem.\n";
    abort();
  }

  memcpy(merge_graph + merge_meta_offset, &vec_num, sizeof(vec_num));
  merge_meta_offset += sizeof(vec_num);

  memcpy(
      merge_graph + merge_meta_offset, &index_param.workspace.medoid,
      sizeof(index_param.workspace.medoid));
  merge_meta_offset += sizeof(index_param.workspace.medoid);
  printf(
      "b1graph vnum: %llu medoid %u size %llu\n", vec_num,
      index_param.workspace.medoid, merge_graph_size);

  std::ifstream base_reader(
      index_param.local_noreplica_base_file.c_str(), std::ios::binary);
  std::cout << "read base data from: " << index_param.local_noreplica_base_file
            << std::endl;
  uint32_t dummy_size, basedim32;
  base_reader.read((char *)&dummy_size, sizeof(uint32_t));
  base_reader.read((char *)&basedim32, sizeof(uint32_t));

  size_t offset = 0;
  size_t index_size;
  memcpy(&index_size, final_idx_ptr + offset, sizeof(index_size));
  offset += sizeof(index_size);
  uint32_t max_deg;
  memcpy(&max_deg, final_idx_ptr + offset, sizeof(max_deg));
  offset += sizeof(max_deg);
  if (max_deg != index_param.max_degree) {
    std::cerr << "Error: Mismatch of max degree.\n";
    abort();
  }
  uint32_t medoid;
  memcpy(&medoid, final_idx_ptr + offset, sizeof(medoid));
  offset += sizeof(medoid);

  index_param.workspace.medoid = medoid;

  uint64_t index_frozen;
  memcpy(&index_frozen, final_idx_ptr + offset, sizeof(index_frozen));
  offset += sizeof(index_frozen);

  uint32_t *ngh_ptr = (uint32_t *)(final_idx_ptr + offset);
  size_t ngh_offset = 0;
  size_t ele_num = 0;
  size_t partition_ofs = 0;
  for (uint32_t m = 0; m < index_param.machine_id; m++) {
    partition_ofs += index_param.workspace.partition_size[m];
  }
  std::cout << "partition_ofs: " << partition_ofs << std::endl;
  while (offset + ngh_offset * sizeof(uint32_t) < index_size) {
    char *ele_ptr =
        (char *)(merge_graph + merge_meta_offset + ele_num * b1_ele_size);
    size_t ele_ofs = 0;
    uint32_t degree = ngh_ptr[ngh_offset];
    uint32_t remap_ngh[degree + 1];
    remap_ngh[0] = degree;
    for (uint32_t i = 1; i < degree + 1; i++) {
      remap_ngh[i] =
          index_param.workspace.label2pos_map[ngh_ptr[ngh_offset + i]];
    }
    memcpy(
        ele_ptr + ele_ofs, (char *)remap_ngh, (degree + 1) * sizeof(uint32_t));
    ele_ofs += (index_param.max_degree + 1) * sizeof(uint32_t);
    base_reader.read(ele_ptr + ele_ofs, index_param.vec_size);
    ele_ofs += index_param.vec_size;
    size_t pos = partition_ofs + ele_num;
    size_t label = index_param.workspace.pos2label_map[pos];
    memcpy(ele_ptr + ele_ofs, (char *)&label, sizeof(size_t));
    ngh_offset += degree + 1;
    ele_num++;
  }
  std::cout << "max deg: " << max_deg << " medoid: " << medoid
            << " index frozen: " << index_frozen << std::endl;
  std::cout << ele_num << " nodes read.\n";

  std::cout << "start write index\n";
  cached_ofstream output(
      index_param.final_b1_index_path, BUFFER_SIZE_FOR_CACHED_IO);
  size_t write_blk_size = 64 * 1024 * 1024;
  num_blocks = DIV_ROUND_UP(merge_graph_size, write_blk_size);
  for (uint64_t i = 0; i < num_blocks; i++) {
    size_t cur_block_size =
        write_blk_size > merge_graph_size - (i * write_blk_size)
            ? merge_graph_size - (i * write_blk_size)
            : write_blk_size;
    output.write(merge_graph + i * write_blk_size, cur_block_size);
  }
  free(final_idx_ptr);
  free(merge_graph);
  std::cout << " b1 graph vnum num:" << ele_num << "\n";
  std::cout << "Transfer index to b1graph over: "
            << index_param.final_b1_index_path << "\n";
}
