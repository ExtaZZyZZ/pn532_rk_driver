#ifndef USERS_H
#define USERS_H

#include <stdint.h>

#define USERS_DB_PATH       "/etc/pn532/users.csv"
#define USERS_BLACKLIST_PATH "/etc/pn532/blacklist"
#define USERS_NAME_MAX  64
#define USERS_ROLE_MAX  32

typedef struct {
    uint32_t id;
    char     name[USERS_NAME_MAX];
    char     role[USERS_ROLE_MAX];
} user_t;

/* Find user by id. Returns 0 on success, -1 if not found. */
int  users_find(uint32_t id, user_t *out);

/* Add or update user. Returns 0 on success, -1 on error. */
int  users_add(const user_t *u);

/* Delete user by id and add to blacklist. Returns 0 on success, -1 if not found. */
int  users_del(uint32_t id);

/* Print all users to stdout. */
void users_list(void);

/* Returns 1 if user_id is blacklisted, 0 if not. */
int  users_is_blacklisted(uint32_t id);

/* Add user_id to blacklist explicitly. Returns 0 on success. */
int  users_blacklist_add(uint32_t id);

/* Remove user_id from blacklist. Returns 0 on success, -1 if not found. */
int  users_blacklist_del(uint32_t id);

/* Print blacklist. */
void users_blacklist_list(void);

#endif
