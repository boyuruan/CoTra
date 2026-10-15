#pragma once

// Override with -DMACHINE_NUM=N. The value is also the partition count.
#ifndef MACHINE_NUM
#define MACHINE_NUM (16)
#endif
#define MAX_THREAD_NUM (16)
#define GROUP_SIZE (16)
// NOTE: MAX_READ_NUM is the max read num one thread can send to one machine.
#define MAX_READ_NUM (2 * GROUP_SIZE)
#define MAX_VEC_LEN (2560)
#define MAX_QUERYBUFFER_SIZE (24576)
#define MAX_SEND_SGE (1)
#define MAX_RECV_SGE (1)
#define MAX_SGE_NUM (16)
// WARN: this may not be enough on large datasets.
#define MAX_CACHE_VEC (2000)
#define MAX_WRITE_NUM (512)
#define MAX_RECV_NUM (64)
#define RELEASE_BLOCK (MAX_WRITE_NUM >> 2)
#define READ_MARK_OFF (25)
#define MAX_WC_NUM (64)
#define LOW_16BIT_MASK (0x0000ffff)

#define OFF (0)
#define ON (1)
#define DEF_ITERS (1000)
#define MAX_SIZE (8388608)
#define LAT_MEASURE_TAIL (2)
#define DEF_CACHE_LINE_SIZE (64)
#define SUCCESS (0)
#define FAILURE (1)
