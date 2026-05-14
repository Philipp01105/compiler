#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "path_identity.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif

static char *canonical_path(const char *path) {
#ifdef _WIN32
    HANDLE file = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD size = GetFinalPathNameByHandleA(file, NULL, 0, FILE_NAME_NORMALIZED);
        char *result = size == 0 ? NULL : malloc((size_t) size + 1);
        if (result != NULL && GetFinalPathNameByHandleA(file, result, size + 1,
                                                       FILE_NAME_NORMALIZED) == 0) {
            free(result); result = NULL;
        }
        CloseHandle(file);
        return result;
    }
    DWORD failure = GetLastError();
    if (failure != ERROR_FILE_NOT_FOUND && failure != ERROR_PATH_NOT_FOUND) return NULL;
    DWORD attributes = GetFileAttributesA(path);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return NULL;
    char *absolute = _fullpath(NULL, path, 0);
#else
    char *existing = realpath(path, NULL);
    if (existing != NULL) return existing;
    if (errno != ENOENT && errno != ENOTDIR) return NULL;
    struct stat link;
    if (lstat(path, &link) == 0 && S_ISLNK(link.st_mode)) return NULL;
    char *absolute = NULL;
    if (path[0] == '/') {
        size_t size = strlen(path) + 1;
        absolute = malloc(size);
        if (absolute != NULL) memcpy(absolute, path, size);
    } else {
        char *cwd = realpath(".", NULL);
        if (cwd == NULL) return NULL;
        size_t n = strlen(cwd), m = strlen(path);
        if (n <= SIZE_MAX - m - 2) absolute = malloc(n + m + 2);
        if (absolute != NULL) {
            memcpy(absolute, cwd, n); absolute[n] = '/';
            memcpy(absolute + n + 1, path, m + 1);
        }
        free(cwd);
    }
#endif
    if (absolute == NULL) return NULL;
    size_t length = strlen(absolute);
    while (length > 1 && (absolute[length - 1] == '/' || absolute[length - 1] == '\\'))
        absolute[--length] = '\0';
    char *leaf = strrchr(absolute, '/');
#ifdef _WIN32
    char *backslash = strrchr(absolute, '\\');
    if (backslash != NULL && (leaf == NULL || backslash > leaf)) leaf = backslash;
#endif
    if (leaf == NULL || leaf[1] == '\0') { free(absolute); return NULL; }
    size_t offset = (size_t) (leaf - absolute);
    /* Keep a volume/filesystem root intact while resolving the parent. */
    char saved = leaf[1];
    leaf[1] = '\0';
    char *parent;
    if (offset == 0 || (offset == 2 && absolute[1] == ':')) parent = canonical_path(absolute);
    else {
        *leaf = '\0'; parent = canonical_path(absolute); *leaf = '/';
    }
    leaf[1] = saved;
    if (parent == NULL) { free(absolute); return NULL; }
    const char *name = leaf + 1;
    size_t n = strlen(parent), m = strlen(name);
    char *result = n <= SIZE_MAX - m - 2 ? malloc(n + m + 2) : NULL;
    if (result != NULL) {
        if (strcmp(name, ".") == 0) memcpy(result, parent, n + 1);
        else if (strcmp(name, "..") == 0) {
            memcpy(result, parent, n + 1);
            char *end = strrchr(result, '/');
            if (end != NULL && end != result) *end = '\0';
        } else {
            memcpy(result, parent, n); result[n] = '/';
            memcpy(result + n + 1, name, m + 1);
        }
    }
    free(parent); free(absolute);
    return result;
}

int path_identity_equal(const char *first, const char *second) {
    if (first == NULL || second == NULL) return 0;
    if (strcmp(first, second) == 0) return 1;
#ifdef _WIN32
    HANDLE a = CreateFileA(first, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    HANDLE b = CreateFileA(second, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    BY_HANDLE_FILE_INFORMATION ai, bi;
    int same = a != INVALID_HANDLE_VALUE && b != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(a, &ai) && GetFileInformationByHandle(b, &bi) &&
        ai.dwVolumeSerialNumber == bi.dwVolumeSerialNumber &&
        ai.nFileIndexHigh == bi.nFileIndexHigh && ai.nFileIndexLow == bi.nFileIndexLow;
    if (a != INVALID_HANDLE_VALUE) CloseHandle(a);
    if (b != INVALID_HANDLE_VALUE) CloseHandle(b);
    if (same) return 1;
#else
    struct stat a, b;
    if (stat(first, &a) == 0 && stat(second, &b) == 0 &&
        a.st_dev == b.st_dev && a.st_ino == b.st_ino) return 1;
#endif
    char *a_path = canonical_path(first), *b_path = canonical_path(second);
    int result = -1;
    if (a_path != NULL && b_path != NULL) {
#ifdef _WIN32
        for (char *p = a_path; *p; p++) if (*p == '\\') *p = '/';
        for (char *p = b_path; *p; p++) if (*p == '\\') *p = '/';
        result = _stricmp(a_path, b_path) == 0;
#else
        result = strcmp(a_path, b_path) == 0;
#endif
    }
    free(a_path); free(b_path);
    return result;
}
