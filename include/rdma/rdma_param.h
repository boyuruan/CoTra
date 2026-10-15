#pragma once
#include <getopt.h>
#include <inttypes.h>
#include <malloc.h>
#include <math.h>
#include <omp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <vector>

#include "coromem/include/utils.h"
#include "get_clock.h"
#include "rdma_config.h"

#define ROUND_UP(value, alignment)        \
  (((value) % (alignment) == 0) ? (value) \
                                : ((alignment) * ((value) / (alignment) + 1)))

#define ALLOCATE(var, type, size)                                \
  {                                                              \
    if ((var = (type *)malloc(sizeof(type) * (size))) == NULL) { \
      fprintf(stderr, " Cannot Allocate\n");                     \
      exit(1);                                                   \
    }                                                            \
  }

#define INC(size, cache_line_size)                            \
  ((size > cache_line_size) ? ROUND_UP(size, cache_line_size) \
                            : (cache_line_size))

typedef enum { LEADER, MEMBER, UNCHOSEN } MachineType;
typedef enum { SEND, WRITE, WRITE_IMM, READ, ATOMIC } VerbType;
typedef enum {
  PART_INFO,      // for build
  BUILD_SYNC,     // for build
  DISPATCH_META,  // for build
  DISPATCH_ID,    // for build
  DISPATCH_NGH,   // for build
  SCALA_META,     // for scala
  SCALA_NGH,      // for scala
  TASK,
  RESULT,
  NODE_RESULT,  // for node task
  NODE_SYNC,    // for node task
  COMPUTE,
  QUERY,
  CORE_INFO,
  SCHEDULE,
  SYNC,
  RELEASE,
  TERMINATION,
  CLEAR
} ControlType;

// scala_v3 paths that bring an exact distance from another machine.
// CH_READ: inline neighbor on a non-core machine, fetched by RDMA READ.
// CH_MICRO: inline neighbor id sent to a core machine in SYNC/NODE_SYNC.
// CH_RESULT, CH_NODE_RESULT: distances returned for a TASK / node task.
typedef enum {
  CH_READ,
  CH_MICRO,
  CH_RESULT,
  CH_NODE_RESULT,
  CH_NUM
} CommChannel;

class RdmaParameter {
 public:
  char *ib_devname;
  MachineType machine;
  int cache_line_size;
  int cpu_freq_f;
  int thread_num;
  int machine_num;
  int machine_id;

  RdmaParameter();
  RdmaParameter(commandLine &cmd);
  int parser(commandLine &cmd);
  bool is_shm() const { return true; }
};
