#include "rdma/rdma_param.h"

static int get_cache_line_size() {
  int size = 0;
#if !defined(__FreeBSD__)
  size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
  if (size == 0) {
    std::string file_name =
        "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size";
    FILE *fp = fopen(file_name.c_str(), "r");
    if (fp == NULL) {
      return DEF_CACHE_LINE_SIZE;
    }
    char line[10];
    if (fgets(line, 10, fp) != NULL) {
      size = atoi(line);
    }
    fclose(fp);
  }
#endif
  if (size <= 0) size = DEF_CACHE_LINE_SIZE;
  return size;
}

RdmaParameter::RdmaParameter() {
  cache_line_size = get_cache_line_size();
  cpu_freq_f = ON;
  ib_devname = NULL;
  thread_num = 1;
  machine_num = MACHINE_NUM;
  machine_id = 0;
  machine = LEADER;
}

RdmaParameter::RdmaParameter(commandLine &cmd) : RdmaParameter() {
  parser(cmd);
}

int RdmaParameter::parser(commandLine &cmd) {
  auto ib_devname_str = cmd.getOptionValue("-d", "shm");
  ALLOCATE(ib_devname, char, (ib_devname_str.size() + 1));
  strcpy(ib_devname, ib_devname_str.c_str());

  machine_num = MACHINE_NUM;
  int machine_id_flag = cmd.getOptionIntValue("-m", -1);
  if (machine_id_flag < 0 || machine_id_flag >= MACHINE_NUM) {
    fprintf(
        stderr, "machine id (-m %d) must be in [0, %d)\n", machine_id_flag,
        MACHINE_NUM);
    abort();
  }
  machine_id = machine_id_flag;

  thread_num = cmd.getOptionIntValue("-t", 1);
  printf("qp num per machine = %d\n", machine_num);

  machine = machine_id == 0 ? LEADER : MEMBER;
  return 0;
}
