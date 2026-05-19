#include "pn532_spi.h"
#include "users.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#define SOCK_PATH       "/run/pn532/pn532.sock"
#define LOG_PATH        "/var/log/pn532.log"
#define MAX_CLIENTS     8
#define CARD_TIMEOUT_MS 500
#define DEFAULT_DEVICE  "/dev/spidev2.0"
#define DEFAULT_KEY     "\xFF\xFF\xFF\xFF\xFF\xFF"
#define DEFAULT_SECRET  "/etc/pn532/secret.key"

static pn532_t g_dev;
static uint8_t g_secret[16];
static uint8_t g_sector_key[6];
static int     g_server_fd = -1;
static int     g_clients[MAX_CLIENTS];
static int     g_nclients = 0;
static int socket_init(const char *path)
{
    struct sockaddr_un addr;
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    chmod(path, 0666);
    if(listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}
static void socket_accept(void)
{
    int fd = accept(g_server_fd, NULL, NULL);
    if (fd < 0) return;
    fprintf(stderr, "new client connected, fd=%d\n", fd);
    if (g_nclients >= MAX_CLIENTS) {
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    g_clients[g_nclients++] = fd;
}
static void broadcast(const char *msg)
{
    fprintf(stderr, "broadcast: %d clients, msg: %s", g_nclients, msg);
    int i = 0;
    while (i < g_nclients) {
        ssize_t n = write(g_clients[i], msg, strlen(msg));
        if (n < 0) {
            fprintf(stderr, "broadcast: client %d error: %s\n", i, strerror(errno));
            close(g_clients[i]);
            g_clients[i] = g_clients[--g_nclients];
        } else {
            i++;
        }
    }
}
static void build_json(char* buf, size_t len,
                       int granted, uint32_t user_id, 
                       const char *name, char *role)
{
    time_t now = time(NULL);
    struct tm *t = gmtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", t);
      snprintf(buf, len,
        "{\"granted\":%s,\"user_id\":%u,\"name\":\"%s\","
        "\"role\":\"%s\",\"timestamp\":\"%s\"}\n",
        granted ? "true" : "false",
        user_id,
        name ? name : "",
        role ? role : "",
        ts);
}
static void log_event(const char *msg)
{
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    fputs(msg, f);
    fclose(f);
}
static int parse_hex(const char *s, uint8_t *out, size_t len)
{
    if (strlen(s) != len * 2) return -1;
    for (size_t i = 0; i < len; i++) {
        char buf[3] = { s[i*2], s[i*2+1], 0 };
        char *end;
        long v = strtol(buf, &end, 16);
        if (end != buf + 2) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}
static int load_secret(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(g_secret, 1, 16, f);
    fclose(f);
    return (n == 16) ? 0 : -1;
}
int main(int argc, char *argv[])
{
    const char *device      = DEFAULT_DEVICE;
    const char *secret_file = DEFAULT_SECRET;
    const char *sock_path   = SOCK_PATH;

    memcpy(g_sector_key, DEFAULT_KEY, 6);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "--secret") == 0 && i + 1 < argc) {
            secret_file = argv[++i];
        } else if (strcmp(argv[i], "--sector-key") == 0 && i + 1 < argc) {
            if (parse_hex(argv[++i], g_sector_key, 6) < 0) {
                fprintf(stderr, "Error: --sector-key must be 12 hex chars\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        }
    }

    if (load_secret(secret_file) < 0) {
        fprintf(stderr, "Error: cannot load secret key from %s\n", secret_file);
        return 1;
    }

    mkdir("/run/pn532", 0755);

    g_server_fd = socket_init(sock_path);
    if (g_server_fd < 0) {
        fprintf(stderr, "Error: cannot create socket %s: %s\n",
                sock_path, strerror(errno));
        return 1;
    }
    fprintf(stderr, "Listening on %s\n", sock_path);

    if (pn532_open(&g_dev, device) < 0) {
        fprintf(stderr, "Error: cannot open %s\n", device);
        return 1;
    }
    fprintf(stderr, "PN532 opened on %s\n", device);

    for (;;) {
        socket_accept();

        int ret = pn532_verify_timeout(&g_dev, g_secret, g_sector_key,
                                       CARD_TIMEOUT_MS);
        char json[256];

        if (ret > 0) {
            user_t u;
            uint32_t uid = (uint32_t)ret;
            if (users_find(uid, &u) == 0)
                build_json(json, sizeof(json), 1, uid, u.name, u.role);
            else
                build_json(json, sizeof(json), 1, uid, "", "");
            broadcast(json);
            log_event(json);
            usleep(1500000);

        } else if (ret == -EACCES) {
            build_json(json, sizeof(json), 0, 0, "", "");
            broadcast(json);
            log_event(json);
            usleep(1500000);
        }
    }

    pn532_close(&g_dev);
    close(g_server_fd);
    unlink(sock_path);
    return 0;
}
