#ifndef PN532_SPI_H
#define PN532_SPI_H

#include <stdint.h>
#include <stddef.h>
#include "pn532_proto.h"

typedef struct {
    int fd;
    int speed_hz;
    int sw_lsb_first;   /* 1 = software bit-reversal (hardware flag not supported) */
} pn532_t;

/* Open /dev/spidevX.Y, configure SPI MODE0, attempt hardware LSB_FIRST,
   fall back to software bit-reversal, send wake-up sequence.
   Returns 0 on success, -errno on failure. */
int  pn532_open(pn532_t *dev, const char *spidev_path);
void pn532_close(pn532_t *dev);

int pn532_get_firmware_version(pn532_t *dev,
                               uint8_t *ic, uint8_t *ver, uint8_t *rev);

/* Returns target number (1-based) on success, <0 on error */
int pn532_detect_card(pn532_t *dev, uint8_t *uid, uint8_t *uid_len);

int pn532_mifare_auth(pn532_t *dev, uint8_t tg, uint8_t block,
                      uint8_t key_type, const uint8_t *key,
                      const uint8_t *uid, uint8_t uid_len);

int pn532_mifare_read(pn532_t *dev, uint8_t tg, uint8_t block,
                      uint8_t out[MIFARE_BLOCK_SIZE]);

int pn532_mifare_write(pn532_t *dev, uint8_t tg, uint8_t block,
                       const uint8_t data[MIFARE_BLOCK_SIZE]);

int pn532_release(pn532_t *dev, uint8_t tg);

/*
 * Enroll: write AES-encrypted token to card block.
 * Token layout (16 bytes): UID[4] | user_id[4] | 0x00...0x00[8]
 * Encrypted with AES-128-ECB using secret_key.
 * Also changes sector key from default FFFFFFFFFFFF to sector_key.
 * Returns 0 on success, <0 on error.
 */
int pn532_enroll(pn532_t *dev,
                 uint32_t user_id,
                 const uint8_t secret_key[16],
                 const uint8_t new_sector_key[6]);

/*
 * Verify: read block, decrypt, check UID matches.
 * Returns user_id on success, -EACCES if UID mismatch, <0 on other error.
 */
int pn532_verify(pn532_t *dev,
                 const uint8_t secret_key[16],
                 const uint8_t sector_key[6]);
int pn532_verify_timeout(pn532_t *dev,
                 const uint8_t secret_key[16],
                 const uint8_t sector_key[6],
                 int timeout_ms);

#endif /* PN532_SPI_H */
