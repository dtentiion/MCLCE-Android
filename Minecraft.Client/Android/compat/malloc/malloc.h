#pragma once

// The one malloc zone the memory log walks, filled from mallinfo

#include <malloc.h>
#include <stddef.h>

#include "../mach/mach.h"

typedef struct _malloc_zone_t {
    int unused;
} malloc_zone_t;

typedef struct malloc_statistics_t {
    unsigned blocks_in_use;
    size_t size_in_use;
    size_t max_size_in_use;
    size_t size_allocated;
} malloc_statistics_t;

static inline kern_return_t malloc_get_all_zones(mach_port_t, void *, vm_address_t **zones, unsigned *count) {
    static malloc_zone_t zone;
    static vm_address_t list[1];
    list[0] = (vm_address_t)&zone;
    *zones = list;
    *count = 1;
    return KERN_SUCCESS;
}

static inline void malloc_zone_statistics(malloc_zone_t *, malloc_statistics_t *stats) {
    struct mallinfo info = mallinfo();
    stats->blocks_in_use = (unsigned)info.ordblks;
    stats->size_in_use = (size_t)info.uordblks;
    stats->max_size_in_use = (size_t)info.usmblks;
    stats->size_allocated = (size_t)info.arena;
}
