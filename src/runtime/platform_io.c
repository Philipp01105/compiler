#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <sys/types.h>
#endif

/* Source open flags follow Linux/POSIX values, independent of the host CRT. */
int64_t __dmm_rt_sys_open(const char *path, int64_t flags, int64_t mode) {
    if (path == NULL) return -EINVAL;
#ifdef _WIN32
    (void) mode;
    int native = (int) (flags & 3) | _O_BINARY;
    if ((flags & 64) != 0) native |= _O_CREAT;
    if ((flags & 512) != 0) native |= _O_TRUNC;
    if ((flags & 1024) != 0) native |= _O_APPEND;
    int result = _open(path, native, 384);
    return result;
#else
    int result = open(path, (int) flags, (mode_t) mode);
    return result < 0 ? -(int64_t) errno : result;
#endif
}

int64_t __dmm_rt_sys_close(int64_t descriptor) {
#ifdef _WIN32
    return _close((int) descriptor);
#else
    int result = close((int) descriptor);
    return result < 0 ? -(int64_t) errno : result;
#endif
}

int64_t __dmm_rt_sys_read(int64_t descriptor, void *buffer, int64_t count) {
    (void) fflush(NULL);
    if (count < 0) return -EINVAL;
#ifdef _WIN32
    return _read((int) descriptor, buffer, (unsigned int) count);
#else
    ssize_t result = read((int) descriptor, buffer, (size_t) count);
    return result < 0 ? -(int64_t) errno : (int64_t) result;
#endif
}

int64_t __dmm_rt_sys_write(int64_t descriptor, const void *buffer, int64_t count) {
    (void) fflush(NULL);
    if (count < 0) return -EINVAL;
#ifdef _WIN32
    return _write((int) descriptor, buffer, (unsigned int) count);
#else
    ssize_t result = write((int) descriptor, buffer, (size_t) count);
    return result < 0 ? -(int64_t) errno : (int64_t) result;
#endif
}
