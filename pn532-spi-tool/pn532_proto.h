#ifndef PN532_PROTO_H
#define PN532_PROTO_H

#include <stdint.h>
#include <stddef.h>

/* HSU frame TFI values */
#define PN532_TFI_HOST_TO_PN532   0xD4
#define PN532_TFI_PN532_TO_HOST   0xD5

/* PN532 commands */
#define PN532_CMD_GET_FIRMWARE_VERSION    0x02
#define PN532_CMD_IN_LIST_PASSIVE_TARGET  0x4A
#define PN532_CMD_IN_DATA_EXCHANGE        0x40
#define PN532_CMD_IN_RELEASE              0x52

/* MIFARE commands (sent via InDataExchange) */
#define MIFARE_CMD_AUTH_KEY_A  0x60
#define MIFARE_CMD_AUTH_KEY_B  0x61
#define MIFARE_CMD_READ        0x30
#define MIFARE_CMD_WRITE       0xA0

#define PN532_UID_MAX_LEN    10
#define MIFARE_BLOCK_SIZE    16
#define PN532_MAX_FRAME_LEN  280

/* SPI direction bytes (PN532 SPI protocol) */
#define PN532_SPI_WRITE       0x01
#define PN532_SPI_STATUS_READ 0x02
#define PN532_SPI_READ        0x03
#define PN532_SPI_READY_FLAG  0x01

int pn532_build_frame(uint8_t *buf, size_t bufsz,
                      uint8_t cmd, const uint8_t *data, size_t dlen);

int pn532_parse_frame(const uint8_t *buf, size_t len,
                      uint8_t expected_cmd,
                      uint8_t *out, size_t *out_len);

#endif
