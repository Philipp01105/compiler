#include "resource_fault.h"
#include <errno.h>
static size_t fail_allocation, fail_io, allocations, io_calls;
static int triggered;

void dmm_fault_reset(size_t allocation, size_t io) {
    fail_allocation = allocation;
    fail_io = io;
    allocations = io_calls = 0;
    triggered = 0;
}

size_t dmm_fault_allocations(void) { return allocations; }
size_t dmm_fault_io_calls(void) { return io_calls; }
int dmm_fault_triggered(void) { return triggered; }

static int allocation_failure(void) {
    allocations++;
    if (fail_allocation != 0 && allocations == fail_allocation) {
        triggered = 1;
        errno = ENOMEM;
        return 1;
    }
    return 0;
}

static int io_failure(int number) {
    io_calls++;
    if (fail_io != 0 && io_calls == fail_io) {
        triggered = 1;
        errno = number;
        return 1;
    }
    return 0;
}

void *dmm_fault_malloc(size_t size) { return size != 0 && allocation_failure() ? NULL : malloc(size); }

void *dmm_fault_calloc(size_t count, size_t size) {
    return count != 0 && size != 0 && allocation_failure() ? NULL : calloc(count, size);
}

void *dmm_fault_realloc(void *pointer, size_t size) {
    return size != 0 && allocation_failure() ? NULL : realloc(pointer, size);
}

FILE *dmm_fault_fopen(const char *path, const char *mode) {
    return io_failure(EACCES) ? NULL : fopen(path, mode);
}

size_t dmm_fault_fwrite(const void *data, size_t size, size_t count, FILE *file) {
    if (io_failure(ENOSPC)) return 0;
    return fwrite(data, size, count, file);
}

int dmm_fault_fclose(FILE *file) {
    int failed = io_failure(EIO);
    int result = fclose(file);
    if (failed) {
        errno = EIO;
        return EOF;
    }
    return result;
}
