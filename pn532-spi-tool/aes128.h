#ifndef AES128_H
#define AES128_H

#include <stdint.h>

/* AES-128 ECB, no external dependencies.
 * key  = 16 bytes
 * data = 16 bytes in-place encrypt/decrypt */
void aes128_encrypt(const uint8_t key[16], uint8_t data[16]);
void aes128_decrypt(const uint8_t key[16], uint8_t data[16]);

#endif
