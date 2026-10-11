#ifndef DMM_IDE_H
#define DMM_IDE_H
#include <stdio.h>
void driver_ide_capabilities(FILE *output);
int driver_ide_request(const char *request_path, const char *response_path);
#endif
