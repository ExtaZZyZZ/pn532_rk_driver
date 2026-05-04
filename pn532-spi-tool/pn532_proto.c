#include "pn532_proto.h"

#include <string.h>
#include <errno.h>

/*
 * HSU frame layout:
 * 00 00 FF [LEN] [LCS] [TFI] [CMD] [DATA...] [DCS] 00
 */
int pn532_build_frame(uint8_t *buf, size_t bufsz,
                      uint8_t cmd, const uint8_t *data, size_t dlen)
{
    size_t frame_len = 6 + 1 + 1 + dlen + 1 + 1;
    if (frame_len > bufsz)
        return -1;

    uint8_t len = (uint8_t)(1 + 1 + dlen);
    uint8_t lcs = (uint8_t)((~len + 1) & 0xFF);

    uint8_t dcs = PN532_TFI_HOST_TO_PN532 + cmd;
    for (size_t i = 0; i < dlen; i++)
        dcs += data[i];
    dcs = (uint8_t)((~dcs + 1) & 0xFF);

    size_t i = 0;
    buf[i++] = 0x00;
    buf[i++] = 0x00;
    buf[i++] = 0xFF;
    buf[i++] = len;
    buf[i++] = lcs;
    buf[i++] = PN532_TFI_HOST_TO_PN532;
    buf[i++] = cmd;
    for (size_t j = 0; j < dlen; j++)
        buf[i++] = data[j];
    buf[i++] = dcs;
    buf[i++] = 0x00;

    return (int)i;
}

/*
 * Returns 0 on success (data frame), 1 if ACK only, -EAGAIN if incomplete,
 * -EIO on protocol error.
 *
 * In SPI mode ACK and data frames are read in separate calls, so returning 1
 * for ACK lets the caller distinguish it from an error.
 */
int pn532_parse_frame(const uint8_t *buf, size_t len,
                      uint8_t expected_cmd,
                      uint8_t *out, size_t *out_len)
{
    size_t pos = 0;
    int saw_ack = 0;

    while (pos + 5 < len) {
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
                return 1;   /* ACK received, no data frame yet */
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

    return saw_ack ? 1 : -EIO;
}
