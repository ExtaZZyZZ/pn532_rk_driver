#ifndef PN532_H
#define PN532_H

#include <stdint.h>
#include <stddef.h>

/* PN532 HSU frame TFI values */
#define PN532_TFI_HOST_TO_PN532  0xD4
#define PN532_TFI_PN532_TO_HOST  0xD5

/* PN532 commands */
#define PN532_CMD_GET_FIRMWARE_VERSION  0x02
#define PN532_CMD_IN_LIST_PASSIVE_TARGET 0x4A
#define PN532_CMD_IN_DATA_EXCHANGE      0x40
#define PN532_CMD_IN_RELEASE            0x52

/* MIFARE commands (sent via InDataExchange) */
#define MIFARE_CMD_AUTH_KEY_A  0x60
#define MIFARE_CMD_AUTH_KEY_B  0x61
#define MIFARE_CMD_READ        0x30
#define MIFARE_CMD_WRITE       0xA0

#define PN532_UID_MAX_LEN  10
#define MIFARE_BLOCK_SIZE  16

typedef struct {
    int     fd;
} pn532_t;

/* Open /dev/pn532, send wake-up, return 0 on success */
int pn532_open(pn532_t *dev, const char *path);
void pn532_close(pn532_t *dev);

/* Firmware version: fills ic/ver/rev, returns 0 on success */
int pn532_get_firmware_version(pn532_t *dev,
                               uint8_t *ic, uint8_t *ver, uint8_t *rev);

/* Detect card: fills uid/uid_len, returns target number (1-based) or <0 */
int pn532_detect_card(pn532_t *dev, uint8_t *uid, uint8_t *uid_len);

/* Authenticate a sector block, key_type = MIFARE_CMD_AUTH_KEY_A/B */
int pn532_mifare_auth(pn532_t *dev, uint8_t tg, uint8_t block,
                      uint8_t key_type, const uint8_t *key,
                      const uint8_t *uid, uint8_t uid_len);

/* Read 16-byte block (must authenticate sector first) */
int pn532_mifare_read(pn532_t *dev, uint8_t tg, uint8_t block,
                      uint8_t out[MIFARE_BLOCK_SIZE]);

/* Write 16-byte block (must authenticate sector first) */
int pn532_mifare_write(pn532_t *dev, uint8_t tg, uint8_t block,
                       const uint8_t data[MIFARE_BLOCK_SIZE]);

/* Release target */
int pn532_release(pn532_t *dev, uint8_t tg);

#endif /* PN532_H */
