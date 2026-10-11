#ifndef DMM_PATH_IDENTITY_H
#define DMM_PATH_IDENTITY_H

/* 1 for aliases, 0 for distinct paths, -1 if identity cannot be established. */
int path_identity_equal(const char *first, const char *second);
/* Caller owns the result. Existing parents are canonicalized for new files. */
char *path_identity_canonical(const char *path);

#endif
