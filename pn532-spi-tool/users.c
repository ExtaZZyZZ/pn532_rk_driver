#include "users.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CSV format: id,name,role */

static FILE *db_open(const char *mode)
{
    return fopen(USERS_DB_PATH, mode);
}

static int parse_line(const char *line, user_t *u)
{
    char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Strip trailing newline */
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
    if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';

    char *tok = strtok(buf, ",");
    if (!tok) return -1;
    u->id = (uint32_t)atoi(tok);

    tok = strtok(NULL, ",");
    if (!tok) return -1;
    strncpy(u->name, tok, USERS_NAME_MAX - 1);
    u->name[USERS_NAME_MAX - 1] = '\0';

    tok = strtok(NULL, ",");
    if (!tok) {
        u->role[0] = '\0';
    } else {
        strncpy(u->role, tok, USERS_ROLE_MAX - 1);
        u->role[USERS_ROLE_MAX - 1] = '\0';
    }

    return 0;
}

int users_find(uint32_t id, user_t *out)
{
    FILE *f = db_open("r");
    if (!f) return -1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        user_t u;
        if (parse_line(line, &u) == 0 && u.id == id) {
            if (out) *out = u;
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

int users_add(const user_t *u)
{
    /* Read all existing entries, replace if id exists */
    FILE *f = db_open("r");
    char lines[1024][256];
    int  count = 0;
    int  found = 0;

    if (f) {
        while (count < 1024 && fgets(lines[count], sizeof(lines[count]), f)) {
            user_t tmp;
            if (parse_line(lines[count], &tmp) == 0 && tmp.id == u->id) {
                snprintf(lines[count], sizeof(lines[count]),
                         "%u,%s,%s\n", u->id, u->name, u->role);
                found = 1;
            }
            count++;
        }
        fclose(f);
    }

    if (!found) {
        if (count >= 1024) return -1;
        snprintf(lines[count], sizeof(lines[count]),
                 "%u,%s,%s\n", u->id, u->name, u->role);
        count++;
    }

    f = db_open("w");
    if (!f) return -1;
    for (int i = 0; i < count; i++)
        fputs(lines[i], f);
    fclose(f);
    return 0;
}

int users_del(uint32_t id)
{
    FILE *f = db_open("r");
    if (!f) return -1;

    char lines[1024][256];
    int  count = 0;
    int  found = 0;

    while (count < 1024 && fgets(lines[count], sizeof(lines[count]), f)) {
        user_t tmp;
        if (parse_line(lines[count], &tmp) == 0 && tmp.id == id) {
            found = 1;
            continue;  /* skip this line */
        }
        count++;
    }
    fclose(f);

    if (!found) return -1;

    f = db_open("w");
    if (!f) return -1;
    for (int i = 0; i < count; i++)
        fputs(lines[i], f);
    fclose(f);

    /* Automatically add to blacklist on delete */
    users_blacklist_add(id);
    return 0;
}

void users_list(void)
{
    FILE *f = db_open("r");
    if (!f) {
        printf("No users database found (%s)\n", USERS_DB_PATH);
        return;
    }

    printf("%-8s %-32s %-16s\n", "ID", "Name", "Role");
    printf("%-8s %-32s %-16s\n", "--------", "--------------------------------", "----------------");

    char line[256];
    int  count = 0;
    while (fgets(line, sizeof(line), f)) {
        user_t u;
        if (parse_line(line, &u) == 0) {
            printf("%-8u %-32s %-16s\n", u.id, u.name, u.role);
            count++;
        }
    }
    fclose(f);

    if (count == 0)
        printf("(empty)\n");
    else
        printf("\nTotal: %d user(s)\n", count);
}

/* ── Blacklist ────────────────────────────────────────────────────────── */

int users_is_blacklisted(uint32_t id)
{
    FILE *f = fopen(USERS_BLACKLIST_PATH, "r");
    if (!f) return 0;

    char line[32];
    while (fgets(line, sizeof(line), f)) {
        uint32_t bid = (uint32_t)atoi(line);
        if (bid == id) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

int users_blacklist_add(uint32_t id)
{
    if (users_is_blacklisted(id))
        return 0;  /* already there */

    FILE *f = fopen(USERS_BLACKLIST_PATH, "a");
    if (!f) return -1;
    fprintf(f, "%u\n", id);
    fclose(f);
    return 0;
}

int users_blacklist_del(uint32_t id)
{
    FILE *f = fopen(USERS_BLACKLIST_PATH, "r");
    if (!f) return -1;

    char lines[1024][32];
    int  count = 0;
    int  found = 0;

    while (count < 1024 && fgets(lines[count], sizeof(lines[count]), f)) {
        uint32_t bid = (uint32_t)atoi(lines[count]);
        if (bid == id) { found = 1; continue; }
        count++;
    }
    fclose(f);
    if (!found) return -1;

    f = fopen(USERS_BLACKLIST_PATH, "w");
    if (!f) return -1;
    for (int i = 0; i < count; i++)
        fputs(lines[i], f);
    fclose(f);
    return 0;
}

void users_blacklist_list(void)
{
    FILE *f = fopen(USERS_BLACKLIST_PATH, "r");
    if (!f) {
        printf("Blacklist is empty\n");
        return;
    }

    printf("Blacklisted user IDs:\n");
    char line[32];
    int  count = 0;
    while (fgets(line, sizeof(line), f)) {
        /* strip newline */
        line[strcspn(line, "\r\n")] = '\0';
        if (strlen(line) > 0) {
            printf("  %s\n", line);
            count++;
        }
    }
    fclose(f);

    if (count == 0)
        printf("  (empty)\n");
}
