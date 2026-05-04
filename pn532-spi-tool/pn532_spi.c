#include "pn532_spi.h"
#include "aes128.h"
#include "users.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>

/* Reverse all 8 bits (PN532 SPI is LSB-first) */
static uint8_t bit_reverse(uint8_t b)
{
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
    return b;
}

static inline uint8_t spi_byte(const pn532_t *dev, uint8_t b)
{
    return dev->sw_lsb_first ? bit_reverse(b) : b;
}

static int spi_xfer(const pn532_t *dev,
                    const uint8_t *tx, uint8_t *rx, size_t len)
{
    struct spi_ioc_transfer xfer = {
        .tx_buf        = (unsigned long)tx,
        .rx_buf        = (unsigned long)rx,
        .len           = (uint32_t)len,
        .speed_hz      = (uint32_t)dev->speed_hz,
        .bits_per_word = 8,
        .cs_change     = 0,
    };
    if (ioctl(dev->fd, SPI_IOC_MESSAGE(1), &xfer) < 0)
        return -errno;
    return 0;
}

/* Returns 1 if PN532 has data ready, 0 if not, <0 on error */
static int spi_check_ready(const pn532_t *dev)
{
    uint8_t tx[2] = { spi_byte(dev, PN532_SPI_STATUS_READ), 0x00 };
    uint8_t rx[2] = { 0 };

    int ret = spi_xfer(dev, tx, rx, 2);
    if (ret < 0)
        return ret;

    uint8_t status = dev->sw_lsb_first ? bit_reverse(rx[1]) : rx[1];
    return (status & PN532_SPI_READY_FLAG) ? 1 : 0;
}

static int spi_wait_ready(const pn532_t *dev, int timeout_ms)
{
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int r = spi_check_ready(dev);
        if (r > 0) return 0;
        if (r < 0)  return r;
        usleep(5000);
        elapsed += 5;
    }
    return -ETIMEDOUT;
}

/*
 * Write direction byte 0x01 + HSU frame in one SPI_IOC_MESSAGE.
 * CS stays low for the entire transaction.
 */
static int spi_write_frame(const pn532_t *dev,
                           const uint8_t *frame, size_t flen)
{
    if (flen + 1 > PN532_MAX_FRAME_LEN + 1)
        return -EINVAL;

    uint8_t tx[PN532_MAX_FRAME_LEN + 1];
    uint8_t rx[PN532_MAX_FRAME_LEN + 1];

    tx[0] = spi_byte(dev, PN532_SPI_WRITE);
    for (size_t i = 0; i < flen; i++)
        tx[1 + i] = spi_byte(dev, frame[i]);

    return spi_xfer(dev, tx, rx, flen + 1);
}

/*
 * Send direction byte 0x03 and read up to maxlen bytes from PN532.
 * Returns payload byte count (excluding direction byte) or <0.
 */
static ssize_t spi_read_frame(const pn532_t *dev,
                              uint8_t *buf, size_t maxlen)
{
    size_t read_len = maxlen + 1;
    if (read_len > PN532_MAX_FRAME_LEN + 2)
        read_len = PN532_MAX_FRAME_LEN + 2;

    uint8_t tx[PN532_MAX_FRAME_LEN + 2];
    uint8_t rx[PN532_MAX_FRAME_LEN + 2];

    memset(tx, 0xFF, read_len);
    tx[0] = spi_byte(dev, PN532_SPI_READ);

    int ret = spi_xfer(dev, tx, rx, read_len);
    if (ret < 0)
        return ret;

    size_t data_len = read_len - 1;
    if (data_len > maxlen)
        data_len = maxlen;

    for (size_t i = 0; i < data_len; i++)
        buf[i] = dev->sw_lsb_first ? bit_reverse(rx[1 + i]) : rx[1 + i];

    return (ssize_t)data_len;
}

/*
 * SPI protocol per PN532 UM section 6.2.5:
 *   1. Write 0x01 + HSU frame
 *   2. Poll status until ACK ready
 *   3. Read ACK (6 bytes: 00 00 FF 00 FF 00)
 *   4. Poll status until response ready
 *   5. Read response frame
 */
static int spi_send_recv(pn532_t *dev,
                         uint8_t cmd,
                         const uint8_t *params, size_t plen,
                         uint8_t *resp, size_t *rlen)
{
    uint8_t txframe[PN532_MAX_FRAME_LEN];
    uint8_t rxbuf[PN532_MAX_FRAME_LEN + 2];

    int flen = pn532_build_frame(txframe, sizeof(txframe), cmd, params, plen);
    if (flen < 0)
        return -EINVAL;

    int ret = spi_write_frame(dev, txframe, (size_t)flen);
    if (ret < 0)
        return ret;

    ret = spi_wait_ready(dev, 1000);
    if (ret < 0)
        return ret;

    spi_read_frame(dev, rxbuf, 16);  /* ACK frame: 6 bytes */

    ret = spi_wait_ready(dev, 5000);
    if (ret < 0)
        return ret;

    ssize_t nread = spi_read_frame(dev, rxbuf, sizeof(rxbuf));
    if (nread < 0)
        return (int)nread;

    int r = pn532_parse_frame(rxbuf, (size_t)nread, cmd, resp, rlen);
    if (r == 1)
        return -EIO;  /* got ACK instead of data frame */
    return r;
}

static int spi_wakeup(pn532_t *dev)
{
    uint8_t tx[20], rx[20];
    memset(tx, spi_byte(dev, 0x55), sizeof(tx));
    spi_xfer(dev, tx, rx, sizeof(tx));
    usleep(2000);
    return 0;
}

int pn532_open(pn532_t *dev, const char *spidev_path)
{
    dev->speed_hz     = 2000000;
    dev->sw_lsb_first = 1;

    dev->fd = open(spidev_path, O_RDWR);
    if (dev->fd < 0)
        return -errno;

    uint8_t  mode  = SPI_MODE_0;
    uint8_t  bits  = 8;
    uint32_t speed = (uint32_t)dev->speed_hz;

    if (ioctl(dev->fd, SPI_IOC_WR_MODE, &mode)          < 0) goto err;
    if (ioctl(dev->fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) goto err;
    if (ioctl(dev->fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) goto err;

    /* Try hardware LSB_FIRST; if unsupported, software reversal stays active */
    uint8_t lsb_mode = SPI_MODE_0 | SPI_LSB_FIRST;
    if (ioctl(dev->fd, SPI_IOC_WR_MODE, &lsb_mode) == 0)
        dev->sw_lsb_first = 0;

    int ret = spi_wakeup(dev);
    if (ret < 0)
        goto err;

    /* SAMConfiguration: normal mode, no IRQ */
    uint8_t sam_params[] = { 0x01, 0x00, 0x00 };
    uint8_t sam_resp[4];
    size_t sam_rlen = sizeof(sam_resp);
    spi_send_recv(dev, 0x14, sam_params, sizeof(sam_params),
                  sam_resp, &sam_rlen);

    return 0;

err:
    close(dev->fd);
    dev->fd = -1;
    return -errno;
}

void pn532_close(pn532_t *dev)
{
    if (dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
    }
}

int pn532_get_firmware_version(pn532_t *dev,
                               uint8_t *ic, uint8_t *ver, uint8_t *rev)
{
    uint8_t resp[8];
    size_t rlen = sizeof(resp);

    int ret = spi_send_recv(dev, PN532_CMD_GET_FIRMWARE_VERSION,
                            NULL, 0, resp, &rlen);
    if (ret < 0) return ret;
    if (rlen < 3) return -EIO;

    if (ic)  *ic  = resp[0];
    if (ver) *ver = resp[1];
    if (rev) *rev = resp[2];
    return 0;
}

int pn532_detect_card(pn532_t *dev, uint8_t *uid, uint8_t *uid_len)
{
    static const uint8_t params[] = { 0x01, 0x00 };
    uint8_t resp[32];
    size_t rlen = sizeof(resp);

    int ret = spi_send_recv(dev, PN532_CMD_IN_LIST_PASSIVE_TARGET,
                            params, sizeof(params), resp, &rlen);
    if (ret < 0) return ret;
    if (rlen < 6) return -EIO;
    if (resp[0] == 0) return -ENODEV;

    uint8_t tg        = resp[1];
    uint8_t nfcid_len = resp[5];

    if (rlen < (size_t)(6 + nfcid_len))
        return -EIO;

    if (uid && uid_len) {
        *uid_len = nfcid_len;
        memcpy(uid, &resp[6], nfcid_len);
    }
    return (int)tg;
}

int pn532_mifare_auth(pn532_t *dev, uint8_t tg, uint8_t block,
                      uint8_t key_type, const uint8_t *key,
                      const uint8_t *uid, uint8_t uid_len)
{
    uint8_t params[3 + 6 + 4] = {0};
    size_t  plen = 0;

    params[plen++] = tg;
    params[plen++] = key_type;
    params[plen++] = block;
    memcpy(&params[plen], key, 6);  plen += 6;
    memcpy(&params[plen], uid, (uid_len < 4) ? uid_len : 4);  plen += 4;

    uint8_t resp[4];
    size_t rlen = sizeof(resp);

    int ret = spi_send_recv(dev, PN532_CMD_IN_DATA_EXCHANGE,
                            params, plen, resp, &rlen);
    if (ret < 0) return ret;
    if (rlen < 1 || resp[0] != 0x00) return -EACCES;
    return 0;
}

int pn532_mifare_read(pn532_t *dev, uint8_t tg, uint8_t block,
                      uint8_t out[MIFARE_BLOCK_SIZE])
{
    uint8_t params[3] = { tg, MIFARE_CMD_READ, block };
    uint8_t resp[1 + MIFARE_BLOCK_SIZE];
    size_t rlen = sizeof(resp);

    int ret = spi_send_recv(dev, PN532_CMD_IN_DATA_EXCHANGE,
                            params, sizeof(params), resp, &rlen);
    if (ret < 0) return ret;
    if (rlen < 1 + MIFARE_BLOCK_SIZE || resp[0] != 0x00) return -EIO;

    memcpy(out, &resp[1], MIFARE_BLOCK_SIZE);
    return 0;
}

int pn532_mifare_write(pn532_t *dev, uint8_t tg, uint8_t block,
                       const uint8_t data[MIFARE_BLOCK_SIZE])
{
    uint8_t params[3 + MIFARE_BLOCK_SIZE];
    params[0] = tg;
    params[1] = MIFARE_CMD_WRITE;
    params[2] = block;
    memcpy(&params[3], data, MIFARE_BLOCK_SIZE);

    uint8_t resp[2];
    size_t rlen = sizeof(resp);

    int ret = spi_send_recv(dev, PN532_CMD_IN_DATA_EXCHANGE,
                            params, sizeof(params), resp, &rlen);
    if (ret < 0) return ret;
    if (rlen < 1 || resp[0] != 0x00) return -EIO;
    return 0;
}

int pn532_release(pn532_t *dev, uint8_t tg)
{
    uint8_t params[1] = { tg };
    uint8_t resp[2];
    size_t rlen = sizeof(resp);

    return spi_send_recv(dev, PN532_CMD_IN_RELEASE,
                         params, sizeof(params), resp, &rlen);
}

/* Default MIFARE factory key */
static const uint8_t KEY_DEFAULT[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

/* Data block and sector trailer for enroll (sector 1: blocks 4-7) */
#define ENROLL_DATA_BLOCK    4
#define ENROLL_SECTOR_BLOCK  4   /* first block of sector = used for auth */
#define ENROLL_TRAILER_BLOCK 7   /* sector trailer of sector 1 */

int pn532_enroll(pn532_t *dev,
                 uint32_t user_id,
                 const uint8_t secret_key[16],
                 const uint8_t new_sector_key[6])
{
    uint8_t uid[PN532_UID_MAX_LEN];
    uint8_t uid_len = sizeof(uid);

    int tg = pn532_detect_card(dev, uid, &uid_len);
    if (tg < 0) return tg;

    printf("Card UID: ");
    for (int i = 0; i < uid_len; i++) printf("%02X ", uid[i]);
    printf("\n");

    /* Authenticate with default key */
    int ret = pn532_mifare_auth(dev, (uint8_t)tg, ENROLL_SECTOR_BLOCK,
                                MIFARE_CMD_AUTH_KEY_A, KEY_DEFAULT,
                                uid, uid_len);
    if (ret < 0) {
        /* Re-detect card after failed auth (card resets auth state) */
        pn532_release(dev, (uint8_t)tg);
        usleep(100000);  /* 100ms */
        tg = pn532_detect_card(dev, uid, &uid_len);
        if (tg < 0) {
            fprintf(stderr, "Error: card lost after auth failure\n");
            return tg;
        }
        /* Try with new_sector_key (card already enrolled) */
        ret = pn532_mifare_auth(dev, (uint8_t)tg, ENROLL_SECTOR_BLOCK,
                                MIFARE_CMD_AUTH_KEY_A, new_sector_key,
                                uid, uid_len);
        if (ret < 0) {
            fprintf(stderr, "Error: authentication failed\n");
            pn532_release(dev, (uint8_t)tg);
            return ret;
        }
    }

    /* Build token: UID[4] | user_id[4] | 0x00[8] */
    uint8_t token[16] = {0};
    memcpy(token, uid, uid_len < 4 ? uid_len : 4);
    token[4] = (user_id >> 24) & 0xFF;
    token[5] = (user_id >> 16) & 0xFF;
    token[6] = (user_id >>  8) & 0xFF;
    token[7] = (user_id      ) & 0xFF;

    /* Encrypt with AES-128-ECB */
    aes128_encrypt(secret_key, token);

    /* Write encrypted token to data block */
    ret = pn532_mifare_write(dev, (uint8_t)tg, ENROLL_DATA_BLOCK, token);
    if (ret < 0) {
        fprintf(stderr, "Error: write token failed\n");
        pn532_release(dev, (uint8_t)tg);
        return ret;
    }

    /* Write sector trailer: new Key A | access bits | new Key B(=Key A) */
    uint8_t trailer[16] = {0};
    memcpy(&trailer[0],  new_sector_key, 6);  /* Key A */
    trailer[6] = 0xFF; trailer[7] = 0x07;     /* access bits: default */
    trailer[8] = 0x80;
    memcpy(&trailer[10], new_sector_key, 6);  /* Key B */

    ret = pn532_mifare_write(dev, (uint8_t)tg, ENROLL_TRAILER_BLOCK, trailer);
    if (ret < 0) {
        fprintf(stderr, "Error: write sector trailer failed\n");
        pn532_release(dev, (uint8_t)tg);
        return ret;
    }

    pn532_release(dev, (uint8_t)tg);
    return 0;
}

int pn532_verify(pn532_t *dev,
                 const uint8_t secret_key[16],
                 const uint8_t sector_key[6])
{
    uint8_t uid[PN532_UID_MAX_LEN];
    uint8_t uid_len = sizeof(uid);

    int tg = pn532_detect_card(dev, uid, &uid_len);
    if (tg < 0) return tg;

    int ret = pn532_mifare_auth(dev, (uint8_t)tg, ENROLL_SECTOR_BLOCK,
                                MIFARE_CMD_AUTH_KEY_A, sector_key,
                                uid, uid_len);
    if (ret < 0) {
        pn532_release(dev, (uint8_t)tg);
        return -EACCES;
    }

    uint8_t token[MIFARE_BLOCK_SIZE];
    ret = pn532_mifare_read(dev, (uint8_t)tg, ENROLL_DATA_BLOCK, token);
    if (ret < 0) {
        pn532_release(dev, (uint8_t)tg);
        return ret;
    }

    pn532_release(dev, (uint8_t)tg);

    /* Decrypt */
    aes128_decrypt(secret_key, token);

    /* Check UID matches first 4 bytes of decrypted token */
    uint8_t uid4 = uid_len < 4 ? uid_len : 4;
    if (memcmp(token, uid, uid4) != 0)
        return -EACCES;

    /* Extract user_id */
    uint32_t user_id = ((uint32_t)token[4] << 24) |
                       ((uint32_t)token[5] << 16) |
                       ((uint32_t)token[6] <<  8) |
                       ((uint32_t)token[7]);

    /* Check blacklist */
    if (users_is_blacklisted(user_id))
        return -EACCES;

    return (int)user_id;
}
