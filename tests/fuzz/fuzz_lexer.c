#include "errorHandler.h"
#include "lexer.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static ErrorHandler *handler;
    if (handler == NULL) {
        handler = error_handler_init();
        if (handler == NULL) return 0;
        error_handler_set_buffered(handler, 1);
        error_handler_set_max_errors(handler, 1000);
        error_handler_set_global(handler);
    }

    TokenStream *tokens = tokenize_source((const char *) data, size, "<fuzz>");
    free_token_stream(tokens);
    error_handler_reset(handler);
    return 0;
}
