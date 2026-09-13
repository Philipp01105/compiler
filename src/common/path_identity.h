#ifndef DMM_PATH_IDENTITY_H
#define DMM_PATH_IDENTITY_H

/* 1 for aliases, 0 for distinct paths, -1 if identity cannot be established. */
int path_identity_equal(const char *first, const char *second);

#endif
