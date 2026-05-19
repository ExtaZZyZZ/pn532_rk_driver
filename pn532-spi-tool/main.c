#include "pn532_spi.h"
#include "users.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#define DEFAULT_DEVICE      "/dev/spidev2.0"
#define DEFAULT_KEY         "\xFF\xFF\xFF\xFF\xFF\xFF"
#define DEFAULT_SECRET_FILE "/etc/pn532/secret.key"

static int load_secret_key(const char *path, uint8_t key[16])
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open secret key file %s\n", path);
        return -1;
    }
    size_t n = fread(key, 1, 16, f);
    fclose(f);
    if (n != 16) {
        fprintf(stderr, "Error: secret key file must be exactly 16 bytes\n");
        return -1;
    }
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS] COMMAND\n"
        "\n"
        "Commands:\n"
        "  info                      Get PN532 firmware version\n"
        "  uid                       Detect card and print UID\n"
        "  read  --block N           Authenticate + read block N (16 bytes)\n"
        "  write --block N --data HH...  Authenticate + write 16 bytes to block N\n"
        "  enroll --user-id N        Write encrypted token to card (anti-clone)\n"
        "  verify                    Verify card token, print user info if valid\n"
        "  user-add --user-id N --name NAME --role ROLE  Add user to database\n"
        "  user-del --user-id N      Delete user and blacklist them\n"
        "  user-list                 List all users in database\n"
        "  blacklist-add --user-id N Add user to blacklist (block access)\n"
        "  blacklist-del --user-id N Remove user from blacklist (restore access)\n"
        "  blacklist-list            Show all blacklisted users\n"
        "\n"
        "Options:\n"
        "  --device PATH             SPI device path (default: /dev/spidev2.0)\n"
        "  --key HEXKEY              6-byte Key A in hex (default: FFFFFFFFFFFF)\n"
        "  --key-b                   Use Key B instead of Key A\n"
        "  --secret PATH             AES-128 secret key file (default: /etc/pn532/secret.key)\n"
        "  --sector-key HEXKEY       6-byte sector key for enroll/verify (default: FFFFFFFFFFFF)\n"
        "  --user-id N               User ID for enroll (1-2147483647)\n"
        "\n"
        "Examples:\n"
        "  %s info\n"
        "  %s uid\n"
        "  %s read --block 4\n"
        "  %s write --block 4 --data DEADBEEF00112233445566778899AABB\n"
        "  %s enroll --user-id 42 --sector-key AABBCCDDEEFF\n"
        "  %s verify --sector-key AABBCCDDEEFF\n",
        prog, prog, prog, prog, prog, prog, prog);
    exit(1);
}

static int parse_hex_byte(const char *s, uint8_t *out)
{
    char buf[3] = { s[0], s[1], 0 };
    char *end;
    long v = strtol(buf, &end, 16);
    if (end != buf + 2)
        return -1;
    *out = (uint8_t)v;
    return 0;
}

static int parse_hex(const char *s, uint8_t *out, size_t len)
{
    if (strlen(s) != len * 2)
        return -1;
    for (size_t i = 0; i < len; i++) {
        if (parse_hex_byte(s + i * 2, &out[i]) < 0)
            return -1;
    }
    return 0;
}

static void print_hex(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (i) printf(" ");
        printf("%02X", data[i]);
    }
    printf("\n");
}

static void print_uid(const uint8_t *uid, uint8_t uid_len)
{
    printf("Card UID: ");
    print_hex(uid, uid_len);
}

int main(int argc, char *argv[])
{
    const char *device      = DEFAULT_DEVICE;
    const char *cmd         = NULL;
    const char *secret_file = DEFAULT_SECRET_FILE;
    const char *name        = NULL;
    const char *role        = "user";
    int         block       = -1;
    int         user_id     = -1;
    uint8_t     key[6];
    uint8_t     sector_key[6];
    uint8_t     key_type    = MIFARE_CMD_AUTH_KEY_A;
    uint8_t     wdata[MIFARE_BLOCK_SIZE];
    int         has_data    = 0;

    memcpy(key,        DEFAULT_KEY, 6);
    memcpy(sector_key, DEFAULT_KEY, 6);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "--block") == 0 && i + 1 < argc) {
            block = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--user-id") == 0 && i + 1 < argc) {
            user_id = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            role = argv[++i];
        } else if (strcmp(argv[i], "--secret") == 0 && i + 1 < argc) {
            secret_file = argv[++i];
        } else if (strcmp(argv[i], "--sector-key") == 0 && i + 1 < argc) {
            if (parse_hex(argv[++i], sector_key, 6) < 0) {
                fprintf(stderr, "Error: --sector-key must be 12 hex chars\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            if (parse_hex(argv[++i], key, 6) < 0) {
                fprintf(stderr, "Error: --key must be 12 hex chars (e.g. FFFFFFFFFFFF)\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--key-b") == 0) {
            key_type = MIFARE_CMD_AUTH_KEY_B;
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            if (parse_hex(argv[++i], wdata, MIFARE_BLOCK_SIZE) < 0) {
                fprintf(stderr, "Error: --data must be %d hex bytes (%d chars)\n",
                        MIFARE_BLOCK_SIZE, MIFARE_BLOCK_SIZE * 2);
                return 1;
            }
            has_data = 1;
        } else if (argv[i][0] != '-') {
            cmd = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
        }
    }

    if (!cmd)
        usage(argv[0]);

    pn532_t dev;
    printf("Opening %s...\n", device);
    int ret = pn532_open(&dev, device);
    if (ret < 0) {
        fprintf(stderr, "Error: cannot open %s: %s\n", device, strerror(-ret));
        return 1;
    }

    if (strcmp(cmd, "info") == 0) {
        uint8_t ic, ver, rev;
        ret = pn532_get_firmware_version(&dev, &ic, &ver, &rev);
        if (ret < 0) {
            fprintf(stderr, "Error: GetFirmwareVersion failed: %s\n", strerror(-ret));
            pn532_close(&dev);
            return 1;
        }
        printf("PN532 Firmware: IC=0x%02X  Ver=%u.%u\n", ic, ver, rev);

    } else if (strcmp(cmd, "uid") == 0) {
        printf("Waiting for card...\n");
        uint8_t uid[PN532_UID_MAX_LEN];
        uint8_t uid_len = sizeof(uid);
        ret = pn532_detect_card(&dev, uid, &uid_len);
        if (ret < 0) {
            fprintf(stderr, "Error: InListPassiveTarget failed: %s\n", strerror(-ret));
            pn532_close(&dev);
            return 1;
        }
        print_uid(uid, uid_len);
        pn532_release(&dev, (uint8_t)ret);

    } else if (strcmp(cmd, "read") == 0) {
        if (block < 0) {
            fprintf(stderr, "Error: --block required\n");
            pn532_close(&dev);
            return 1;
        }

        printf("Waiting for card...\n");
        uint8_t uid[PN532_UID_MAX_LEN];
        uint8_t uid_len = sizeof(uid);
        int tg = pn532_detect_card(&dev, uid, &uid_len);
        if (tg < 0) {
            fprintf(stderr, "Error: no card detected: %s\n", strerror(-tg));
            pn532_close(&dev);
            return 1;
        }
        print_uid(uid, uid_len);
        

        uint8_t sector_block = (uint8_t)(block & ~0x03);
        printf("Authenticating sector %d (block %d) with Key %s...\n",
               block / 4, sector_block,
               (key_type == MIFARE_CMD_AUTH_KEY_A) ? "A" : "B");

        ret = pn532_mifare_auth(&dev, (uint8_t)tg, sector_block,
                                key_type, key, uid, uid_len);
        if (ret < 0) {
            fprintf(stderr, "Error: authentication failed: %s\n", strerror(-ret));
            pn532_release(&dev, (uint8_t)tg);
            pn532_close(&dev);
            return 1;
        }
        printf("Authentication OK\n");

        uint8_t rdata[MIFARE_BLOCK_SIZE];
        ret = pn532_mifare_read(&dev, (uint8_t)tg, (uint8_t)block, rdata);
        if (ret < 0) {
            fprintf(stderr, "Error: read block %d failed: %s\n", block, strerror(-ret));
            pn532_release(&dev, (uint8_t)tg);
            pn532_close(&dev);
            return 1;
        }

        printf("Block %d: ", block);
        print_hex(rdata, MIFARE_BLOCK_SIZE);
        pn532_release(&dev, (uint8_t)tg);

    } else if (strcmp(cmd, "write") == 0) {
        if (block < 0) {
            fprintf(stderr, "Error: --block required\n");
            pn532_close(&dev);
            return 1;
        }
        if (!has_data) {
            fprintf(stderr, "Error: --data required\n");
            pn532_close(&dev);
            return 1;
        }
        if (block == 0) {
            fprintf(stderr, "Error: block 0 is read-only (UID/manufacturer data)\n");
            pn532_close(&dev);
            return 1;
        }
        if ((block & 0x03) == 3) {
            fprintf(stderr,
                "Warning: block %d is a sector trailer (keys + access bits).\n"
                "Writing wrong data PERMANENTLY LOCKS the card!\n"
                "Aborting for safety. Use a dedicated key-write tool.\n", block);
            pn532_close(&dev);
            return 1;
        }

        printf("Waiting for card...\n");
        uint8_t uid[PN532_UID_MAX_LEN];
        uint8_t uid_len = sizeof(uid);
        int tg = pn532_detect_card(&dev, uid, &uid_len);
        if (tg < 0) {
            fprintf(stderr, "Error: no card detected: %s\n", strerror(-tg));
            pn532_close(&dev);
            return 1;
        }
        print_uid(uid, uid_len);

        uint8_t sector_block = (uint8_t)(block & ~0x03);
        printf("Authenticating sector %d (block %d) with Key %s...\n",
               block / 4, sector_block,
               (key_type == MIFARE_CMD_AUTH_KEY_A) ? "A" : "B");

        ret = pn532_mifare_auth(&dev, (uint8_t)tg, sector_block,
                                key_type, key, uid, uid_len);
        if (ret < 0) {
            fprintf(stderr, "Error: authentication failed: %s\n", strerror(-ret));
            pn532_release(&dev, (uint8_t)tg);
            pn532_close(&dev);
            return 1;
        }
        printf("Authentication OK\n");

        printf("Writing block %d: ", block);
        print_hex(wdata, MIFARE_BLOCK_SIZE);

        ret = pn532_mifare_write(&dev, (uint8_t)tg, (uint8_t)block, wdata);
        if (ret < 0) {
            fprintf(stderr, "Error: write block %d failed: %s\n", block, strerror(-ret));
            pn532_release(&dev, (uint8_t)tg);
            pn532_close(&dev);
            return 1;
        }
        printf("Write OK\n");
        pn532_release(&dev, (uint8_t)tg);

    } else if (strcmp(cmd, "enroll") == 0) {
        if (user_id < 1) {
            fprintf(stderr, "Error: --user-id required (positive integer)\n");
            pn532_close(&dev);
            return 1;
        }
        uint8_t secret[16];
        if (load_secret_key(secret_file, secret) < 0) {
            pn532_close(&dev);
            return 1;
        }

        printf("Waiting for card...\n");
        ret = pn532_enroll(&dev, (uint32_t)user_id, secret, sector_key);
        if (ret < 0) {
            fprintf(stderr, "Error: enroll failed: %s\n", strerror(-ret));
            pn532_close(&dev);
            return 1;
        }
        printf("Enroll OK: user_id=%d written to card\n", user_id);

    } else if (strcmp(cmd, "verify") == 0) {
        uint8_t secret[16];
        if (load_secret_key(secret_file, secret) < 0) {
            pn532_close(&dev);
            return 1;
        }

        printf("Waiting for card...\n");
        ret = pn532_verify(&dev, secret, sector_key);
        if (ret == -EACCES) {
            fprintf(stderr, "Access DENIED: card token invalid or cloned\n");
            pn532_close(&dev);
            return 1;
        } else if (ret < 0) {
            fprintf(stderr, "Error: verify failed: %s\n", strerror(-ret));
            pn532_close(&dev);
            return 1;
        }
        user_t u;
        if (users_find((uint32_t)ret, &u) == 0)
            printf("Access Ok: user_id=%d name=%s role=%s\n",
                   ret, u.name, u.role);
        else
            printf("Access Okey: user_id=%d (not in database)\n", ret);

    } else if (strcmp(cmd, "user-add") == 0) {
        if (user_id < 1) {
            fprintf(stderr, "Error: --user-id required\n");
            pn532_close(&dev);
            return 1;
        }
        if (!name) {
            fprintf(stderr, "Error: --name required\n");
            pn532_close(&dev);
            return 1;
        }
        user_t u;
        u.id = (uint32_t)user_id;
        strncpy(u.name, name, USERS_NAME_MAX - 1);
        u.name[USERS_NAME_MAX - 1] = '\0';
        strncpy(u.role, role, USERS_ROLE_MAX - 1);
        u.role[USERS_ROLE_MAX - 1] = '\0';

        if (users_add(&u) < 0) {
            fprintf(stderr, "Error: cannot write to database\n");
            pn532_close(&dev);
            return 1;
        }
        printf("User added: id=%u name=%s role=%s\n", u.id, u.name, u.role);

    } else if (strcmp(cmd, "user-del") == 0) {
        if (user_id < 1) {
            fprintf(stderr, "Error: --user-id required\n");
            pn532_close(&dev);
            return 1;
        }
        if (users_del((uint32_t)user_id) < 0) {
            fprintf(stderr, "Error: user %d not found\n", user_id);
            pn532_close(&dev);
            return 1;
        }
        printf("User %d deleted\n", user_id);

    } else if (strcmp(cmd, "user-list") == 0) {
        users_list();

    } else if (strcmp(cmd, "blacklist-add") == 0) {
        if (user_id < 1) {
            fprintf(stderr, "Error: --user-id required\n");
            pn532_close(&dev);
            return 1;
        }
        if (users_blacklist_add((uint32_t)user_id) < 0) {
            fprintf(stderr, "Error: cannot write blacklist\n");
            pn532_close(&dev);
            return 1;
        }
        printf("User %d added to blacklist\n", user_id);

    } else if (strcmp(cmd, "blacklist-del") == 0) {
        if (user_id < 1) {
            fprintf(stderr, "Error: --user-id required\n");
            pn532_close(&dev);
            return 1;
        }
        if (users_blacklist_del((uint32_t)user_id) < 0) {
            fprintf(stderr, "Error: user %d not in blacklist\n", user_id);
            pn532_close(&dev);
            return 1;
        }
        printf("User %d removed from blacklist\n", user_id);

    } else if (strcmp(cmd, "blacklist-list") == 0) {
        users_blacklist_list();

    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        pn532_close(&dev);
        usage(argv[0]);
    }

    pn532_close(&dev);
    return 0;
}
