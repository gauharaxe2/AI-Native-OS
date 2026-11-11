#ifndef AIOS_SHA256_H
#define AIOS_SHA256_H

#include <stddef.h>

void sha256_hash(const void *data, size_t len, unsigned char out[32]);
void sha256_hex(const void *data, size_t len, char out[65]);

#endif
