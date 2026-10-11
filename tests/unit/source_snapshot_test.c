#include "errorHandler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, "Snapshot check failed at line %d: %s\n", __LINE__, #condition);                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static int write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return 0;
    int result = fwrite(text, 1, strlen(text), file) == strlen(text);
    return fclose(file) == 0 && result;
}

static int reads_as(const char *path, const char *expected) {
    FILE *file = error_handler_open_source(path);
    if (!file)
        return expected == NULL;
    char text[128] = {0};
    size_t length = fread(text, 1, sizeof(text) - 1, file);
    int result = !ferror(file) && expected && length == strlen(expected) && !strcmp(text, expected);
    fclose(file);
    return result;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    const char *path = argv[1];
    remove(path);
    ErrorHandler *handler = error_handler_init();
    CHECK(handler);
    error_handler_set_global(handler);
    handler->source_snapshot_enabled = 1;
    CHECK(reads_as(path, NULL));
    CHECK(write_text(path, "first"));
    CHECK(reads_as(path, NULL)); /* Creating a previously missing file cannot change this query. */
    error_handler_clear_source_snapshot(handler);
    handler->source_snapshot_enabled = 1;
    CHECK(reads_as(path, "first"));
    CHECK(write_text(path, "second"));
    CHECK(reads_as(path, "first"));
    DmmSourceOverlay overlay = {path, "unsaved", 7};
    handler->overlays = &overlay;
    handler->overlay_count = 1;
    CHECK(reads_as(path, "unsaved"));
    handler->overlays = NULL;
    handler->overlay_count = 0;
    CHECK(reads_as(path, "first"));
    error_handler_clear_source_snapshot(handler);
    CHECK(reads_as(path, "second"));
    handler->source_snapshot_enabled = 1;
    handler->source_snapshot_bytes = 64u * 1024u * 1024u;
    CHECK(reads_as(path, NULL));
    CHECK(handler->source_snapshot_failed);
    error_handler_free(handler);
    error_handler_set_global(NULL);
    CHECK(remove(path) == 0);
    return 0;
}
