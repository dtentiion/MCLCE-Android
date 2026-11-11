#pragma once

// The task_info calls the memory log makes. Resident and virtual size come
// from /proc/self/statm, and the footprint is the resident size.

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "mach_time.h"

typedef int kern_return_t;
typedef unsigned int mach_msg_type_number_t;
typedef unsigned int mach_port_t;
typedef uintptr_t vm_address_t;
typedef int *task_info_t;

#define KERN_SUCCESS 0
#define KERN_FAILURE 5

#define MACH_TASK_BASIC_INFO 20
#define TASK_VM_INFO 22

struct mach_task_basic_info {
    uint64_t virtual_size;
    uint64_t resident_size;
};
typedef struct task_vm_info {
    uint64_t phys_footprint;
} task_vm_info_data_t;

#define MACH_TASK_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(struct mach_task_basic_info) / sizeof(int)))
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(int)))

static inline mach_port_t mach_task_self(void) { return 0; }

static inline kern_return_t task_info(mach_port_t, int flavor, task_info_t out, mach_msg_type_number_t *) {
    unsigned long long pages = 0, resident = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return KERN_FAILURE;
    int n = fscanf(f, "%llu %llu", &pages, &resident);
    fclose(f);
    if (n != 2) return KERN_FAILURE;
    const uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    if (flavor == MACH_TASK_BASIC_INFO) {
        struct mach_task_basic_info *info = (struct mach_task_basic_info *)out;
        info->virtual_size = pages * page;
        info->resident_size = resident * page;
        return KERN_SUCCESS;
    }
    if (flavor == TASK_VM_INFO) {
        ((task_vm_info_data_t *)out)->phys_footprint = resident * page;
        return KERN_SUCCESS;
    }
    return KERN_FAILURE;
}
