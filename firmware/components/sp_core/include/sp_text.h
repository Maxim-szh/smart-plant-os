#ifndef SP_TEXT_H
#define SP_TEXT_H
#include <stdbool.h>
#include <stddef.h>
/* Strict UTF-8, no control characters; byte limits match wire and NVS fields. */
bool sp_text_valid(const char *, size_t minimum, size_t maximum);
#endif
