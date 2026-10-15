#pragma once

#include "anns/anns_param.h"
#include "index_param.h"
#include "scala_graph.h"
#include "top_index.h"

/**
 * Graph index wrapper for CoTra (ScalaGraph V3 + HNSW top index).
 */
class GraphIndex {
 public:
  ScalaGraphIndex* scala_graph = nullptr;
  HNSWTopIndex* top_index = nullptr;

  IndexParameter& index_param;

  GraphIndex(IndexParameter& _index_param) : index_param(_index_param) {
    if (index_param.graph_type == SCALA_V3) {
      scala_graph = new ScalaGraphIndex(index_param);
      top_index = new HNSWTopIndex(index_param);
    } else {
      printf("Error: unrecognized graph storage type.\n");
      abort();
    }
  }

  ~GraphIndex() {}

  char* get_partition_ptr() { return scala_graph->get_partition_ptr(); }

  size_t get_partition_size() { return scala_graph->get_partition_size(); }
};
