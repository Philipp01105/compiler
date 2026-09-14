#ifndef DMM_TEST_SOURCE_H
#define DMM_TEST_SOURCE_H
#include "frontend.h"
#include <stdlib.h>
#include <string.h>
/* Unit cases exercise language fragments inside an explicit executable package. */
static inline AstProgram *test_parse_source(const char *source,size_t length,const char *name,const FrontendOptions *options) {
    const char header[]="package main;\n";
    char *whole=malloc(sizeof(header)+length);
    if (!whole) return NULL;
    memcpy(whole,header,sizeof(header)-1); memcpy(whole+sizeof(header)-1,source,length);
    whole[sizeof(header)-1+length]='\0';
    AstProgram *program=frontend_parse_source(whole,sizeof(header)-1+length,name,options);
    free(whole); return program;
}
#endif
