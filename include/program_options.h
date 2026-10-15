#pragma once

#include <string>

// DiskANN's options header defines external symbols. Include its definitions
// once in program_options.cc and share declarations between the CLI parsers.
namespace program_options_utils {
const std::string make_program_description(
    const char *executable_name, const char *description);
extern const char *DATA_TYPE_DESCRIPTION;
extern const char *DISTANCE_FUNCTION_DESCRIPTION;
extern const char *INDEX_PATH_PREFIX_DESCRIPTION;
extern const char *INPUT_DATA_PATH;
extern const char *NUMBER_THREADS_DESCRIPTION;
extern const char *MAX_BUILD_DEGREE;
extern const char *GRAPH_BUILD_COMPLEXITY;
extern const char *BUIlD_GRAPH_PQ_BYTES;
extern const char *USE_OPQ;
extern const char *LABEL_FILE;
extern const char *UNIVERSAL_LABEL;
extern const char *FILTERED_LBUILD;
extern const char *LABEL_TYPE_DESCRIPTION;
}  // namespace program_options_utils
