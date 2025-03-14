#ifndef __FEMU_MEM_BACKEND
#define __FEMU_MEM_BACKEND

#include <stdint.h>

/* DRAM backend SSD address space */
typedef struct SsdDramBackend {
    void    *logical_space; // 模拟SSD存储的逻辑空间的起始地址模拟SSD存储的逻辑空间的起始地址
    int64_t size; /* in bytes */ // 模拟SSD存储空间的大小，单位是字节
    int     femu_mode; // 模拟环境的工作模式（nossd、black box、ocssd、zns、hybrid SSD）
} SsdDramBackend; // 用于定义和管理一个在DRAM中模拟的SSD存储空间的基本属性和行为。

int init_dram_backend(SsdDramBackend **mbe, int64_t nbytes);
void free_dram_backend(SsdDramBackend *);

int backend_rw(SsdDramBackend *, QEMUSGList *, uint64_t *, bool);

#endif
