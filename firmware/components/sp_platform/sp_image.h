#ifndef SP_IMAGE_H
#define SP_IMAGE_H
#include <stdbool.h>
#include <stdint.h>
bool sp_image_header(const uint8_t prefix[128], unsigned profile, char version[32]);
#endif
