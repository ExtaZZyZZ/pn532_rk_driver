#include "pn532.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>

/* ─── HSU frame builder / parser ─────────────────────────────────────── */

/*
 * HSU frame layout:
 * 00 00 FF [LEN] [LCS] [TFI] [CMD] [DATA...] [DCS] 00
 *
 * LEN  = 1(TFI) + 1(CMD) + len(DATA)
 * LCS  = (~LEN + 1) & 0xFF
 * DCS  = (~(TFI + CMD + sum(DATA)) + 1) & 0xFF
 */
static int build_frame(uint8_t *buf, size_t bufsz,
                       uint8_t cmd, const uint8_t *data, size_t dlen)
{
    size_t frame_len = 6 + 1 + 1 + dlen + 1 + 1; /* preamble(3)+LEN+LCS+TFI+CMD+data+DCS+postamble */
    if (frame_len > bufsz)
        return -1;

    uint8_t len = (uint8_t)(1 + 1 + dlen);  /* TFI + CMD + data */
    uint8_t lcs = (uint8_t)((~len + 1) & 0xFF);

    uint8_t dcs = PN532_TFI_HOST_TO_PN532 + cmd;
    for (size_t i = 0; i < dlen; i++)
        dcs += data[i];
    dcs = (uint8_t)((~dcs + 1) & 0xFF);

    size_t i = 0;
    buf[i++] = 0x00;  /* preamble */
    buf[i++] = 0x00;  /* start code 1 */
    buf[i++] = 0xFF;  /* start code 2 */
    buf[i++] = len;
    buf[i++] = lcs;
    buf[i++] = PN532_TFI_HOST_TO_PN532;
    buf[i++] = cmd;
    for (size_t j = 0; j < dlen; j++)
        buf[i++] = data[j];
    buf[i++] = dcs;
    buf[i++] = 0x00;  /* postamble */

    return (int)i;
}

/*
 * Parse PN532 response frame in buf[0..len-1].
 * Fills out[] with payload bytes (after TFI+response_cmd),
 * sets *out_len. Returns 0 on success, -1 on error.
 */
static int parse_frame(const uint8_t *buf, size_t len,
                       uint8_t expected_cmd,
                       uint8_t *out, size_t *out_len)
{
    size_t pos = 0;
    int saw_ack = 0;

    while (pos + 5 < len) {
        /* PN532 HSU frame start is always 00 00 FF */
        if (!(buf[pos] == 0x00 && buf[pos + 1] == 0x00 && buf[pos + 2] == 0xFF)) {
            pos++;
            continue;
        }

        size_t i = pos + 3;
        uint8_t frame_len = buf[i++];
        uint8_t lcs       = buf[i++];

        if (((frame_len + lcs) & 0xFF) != 0x00) {
            pos++;
            continue;
        }

        /* ACK frame: 00 00 FF 00 FF 00 */
        if (frame_len == 0x00 && lcs == 0xFF) {
            saw_ack = 1;
            if (i >= len)
                return -EAGAIN;
            pos = i + 1;
            continue;
        }

        if (frame_len < 2)
            return -EIO;

        if (i + frame_len + 2 > len)
            return -EAGAIN;

        uint8_t tfi = buf[i++];
        if (tfi != PN532_TFI_PN532_TO_HOST)
            return -EIO;

        uint8_t resp_cmd = buf[i++];
        if (resp_cmd != (expected_cmd + 1))
            return -EIO;

        size_t payload = frame_len - 2;

        uint8_t dcs_calc = tfi + resp_cmd;
        for (size_t j = 0; j < payload; j++)
            dcs_calc += buf[i + j];
        dcs_calc = (uint8_t)((~dcs_calc + 1) & 0xFF);
        if (dcs_calc != buf[i + payload])
            return -EIO;

        if (out && payload > 0) {
            size_t copy = payload;
            if (out_len && copy > *out_len)
                copy = *out_len;
            memcpy(out, buf + i, copy);
            if (out_len)
                *out_len = copy;
        } else if (out_len) {
            *out_len = payload;
        }

        return 0;
    }

    return saw_ack ? -EAGAIN : -EIO;
}

/* Читаем с таймаутом через select, накапливаем байты пока данные идут */
static ssize_t uart_read_timeout(int fd, uint8_t *buf, size_t maxlen,
                                 int timeout_ms)
{
    size_t total = 0;
    struct timeval tv;
    fd_set fds;

    while (total < maxlen) {
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int r = select(fd + 1, &fds, NULL, NULL, &tv);
        if (r <= 0)
            break;  /* таймаут или ошибка */

        ssize_t n = read(fd, buf + total, maxlen - total);
        if (n <= 0)
            break;
        total += n;

        /* После первых байт уменьшаем таймаут до 50ms — ждём хвост фрейма */
        timeout_ms = 50;
    }

    return (ssize_t)total;
}

/*
 * Send cmd+params, read response.
 * Works directly with /dev/ttyS7 — no kernel module needed.
 */
static int send_recv(pn532_t *dev,
                     uint8_t cmd,
                     const uint8_t *params, size_t plen,
                     uint8_t *resp, size_t *rlen)
{
    uint8_t txbuf[280];
    uint8_t rxbuf[280];

    int flen = build_frame(txbuf, sizeof(txbuf), cmd, params, plen);
    if (flen < 0)
        return -EINVAL;

    tcflush(dev->fd, TCIFLUSH);

    size_t written = 0;
    while (written < (size_t)flen) {
        ssize_t wr = write(dev->fd, txbuf + written, (size_t)flen - written);
        if (wr < 0)
            return -errno;
        written += (size_t)wr;
    }

    size_t total = 0;
    int timeout_ms = 1000;

    while (total < sizeof(rxbuf)) {
        ssize_t rd = uart_read_timeout(dev->fd, rxbuf + total,
                                       sizeof(rxbuf) - total, timeout_ms);
        if (rd <= 0)
            return total ? -EIO : -ETIMEDOUT;

        total += (size_t)rd;

        int ret = parse_frame(rxbuf, total, cmd, resp, rlen);
        if (ret == 0)
            return 0;
        if (ret != -EAGAIN)
            return ret;

        /* After ACK wait briefly for the data frame tail. */
        timeout_ms = 200;
    }

    return -EOVERFLOW;
}

/* ─── UART setup ──────────────────────────────────────────────────────── */

static int uart_configure(int fd)
{
    struct termios t;

    if (tcgetattr(fd, &t) < 0)
        return -errno;

    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);

    t.c_cflag |=  (CLOCAL | CREAD | CS8);
    t.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    t.c_iflag  =  0;
    t.c_oflag  =  0;
    t.c_lflag  =  0;
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 10;  /* 1 секунда таймаут на чтение */

    if (tcsetattr(fd, TCSANOW, &t) < 0)
        return -errno;

    tcflush(fd, TCIOFLUSH);
    return 0;
}

/* ─── Public API ──────────────────────────────────────────────────────── */

int pn532_open(pn532_t *dev, const char *path)
{
    /* O_NOCTTY — не делаем ttyS7 controlling terminal процесса */
    dev->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (dev->fd < 0)
        return -errno;

    /* Снимаем O_NONBLOCK — он нужен был только для open() */
    int flags = fcntl(dev->fd, F_GETFL);
    fcntl(dev->fd, F_SETFL, flags & ~O_NONBLOCK);

    if (uart_configure(dev->fd) < 0) {
        int e = errno;
        close(dev->fd);
        return -e;
    }

    /* Wake-up: 16 байт 0x55 + пауза 50ms */
    static const uint8_t wakeup[16] = {
        0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,
        0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,
    };
    (void)write(dev->fd, wakeup, sizeof(wakeup));
    usleep(50000);
    tcflush(dev->fd, TCIFLUSH);

    return 0;
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

    int ret = send_recv(dev, PN532_CMD_GET_FIRMWARE_VERSION,
                        NULL, 0, resp, &rlen);
    if (ret < 0)
        return ret;

    if (rlen < 3)
        return -EIO;

    if (ic)  *ic  = resp[0];
    if (ver) *ver = resp[1];
    if (rev) *rev = resp[2];

    return 0;
}

int pn532_detect_card(pn532_t *dev, uint8_t *uid, uint8_t *uid_len)
{
    /*
     * InListPassiveTarget: MaxTg=1, BrTy=0x00 (106 kbps ISO14443A / MIFARE)
     * Response: NbTg, Tg, SENS_RES(2), SEL_RES(1), NFCIDLEN, NFCID...
     */
    static const uint8_t params[] = { 0x01, 0x00 };
    uint8_t resp[32];
    size_t rlen = sizeof(resp);

    int ret = send_recv(dev, PN532_CMD_IN_LIST_PASSIVE_TARGET,
                        params, sizeof(params), resp, &rlen);
    if (ret < 0)
        return ret;

    if (rlen < 6)
        return -EIO;

    uint8_t nb_tg = resp[0];
    if (nb_tg == 0)
        return -ENODEV;

    uint8_t tg       = resp[1];
    /* resp[2], resp[3] = SENS_RES */
    /* resp[4]           = SEL_RES  */
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
    /*
     * InDataExchange: Tg, [Auth cmd, BlockNum, Key(6), UID(4)]
     * key_type = 0x60 (KeyA) or 0x61 (KeyB)
     */
    uint8_t params[3 + 6 + 4] = {0};
    size_t  plen = 0;

    params[plen++] = tg;
    params[plen++] = key_type;
    params[plen++] = block;
    memcpy(&params[plen], key, 6);
    plen += 6;
    /* Используем первые 4 байта UID */
    memcpy(&params[plen], uid, (uid_len < 4) ? uid_len : 4);
    plen += 4;

    uint8_t resp[4];
    size_t rlen = sizeof(resp);

    int ret = send_recv(dev, PN532_CMD_IN_DATA_EXCHANGE,
                        params, plen, resp, &rlen);
    if (ret < 0)
        return ret;

    /* resp[0] = status byte, 0x00 = success */
    if (rlen < 1 || resp[0] != 0x00)
        return -EACCES;

    return 0;
}

int pn532_mifare_read(pn532_t *dev, uint8_t tg, uint8_t block,
                      uint8_t out[MIFARE_BLOCK_SIZE])
{
    uint8_t params[3] = { tg, MIFARE_CMD_READ, block };
    uint8_t resp[1 + MIFARE_BLOCK_SIZE];
    size_t rlen = sizeof(resp);

    int ret = send_recv(dev, PN532_CMD_IN_DATA_EXCHANGE,
                        params, sizeof(params), resp, &rlen);
    if (ret < 0)
        return ret;

    /* resp[0] = status, resp[1..16] = block data */
    if (rlen < 1 + MIFARE_BLOCK_SIZE || resp[0] != 0x00)
        return -EIO;

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

    int ret = send_recv(dev, PN532_CMD_IN_DATA_EXCHANGE,
                        params, sizeof(params), resp, &rlen);
    if (ret < 0)
        return ret;

    if (rlen < 1 || resp[0] != 0x00)
        return -EIO;

    return 0;
}

int pn532_release(pn532_t *dev, uint8_t tg)
{
    uint8_t params[1] = { tg };
    uint8_t resp[2];
    size_t rlen = sizeof(resp);

    return send_recv(dev, PN532_CMD_IN_RELEASE,
                     params, sizeof(params), resp, &rlen);
}
