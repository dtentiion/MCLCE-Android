#pragma once

// os_proc_available_memory from MemAvailable

#include <stddef.h>
#include <stdio.h>

static inline size_t os_proc_available_memory(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[128];
    unsigned long long kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
    }
    fclose(f);
    return (size_t)(kb * 1024ull);
}
