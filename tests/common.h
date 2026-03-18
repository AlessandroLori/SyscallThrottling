#pragma once

#include <errno.h>
#include <inttypes.h>
#include <linux/types.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "../include/scth_ioctl.h"

#ifndef SCTH_DEV_NAME
#define SCTH_DEV_NAME "scthrottle"
#endif

#define T_ASSERT(cond, fmt, ...)                                              \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "[FAIL] " fmt "\n", ##__VA_ARGS__);               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

#define T_CHECK_RC(expr)                                                      \
    do {                                                                      \
        int _rc = (expr);                                                     \
        if (_rc < 0) {                                                        \
            fprintf(stderr, "[FAIL] %s -> %s (%d)\n",                         \
                    #expr, strerror(-_rc), -_rc);                             \
            return 1;                                                         \
        }                                                                     \
    } while (0)

#define T_INFO(fmt, ...)  fprintf(stdout, "[INFO] " fmt "\n", ##__VA_ARGS__)
#define T_PASS()          do { fprintf(stdout, "[PASS] %s\n", __FILE__); return 0; } while (0)

int  scth_require_root(void);
const char *scth_policy_name(__u32 p);

/* basic ioctls */
int scth_on(void);
int scth_off(void);
int scth_resetstats(void);
int scth_setmax(__u32 v);
int scth_setpolicy(__u32 v);
int scth_get_cfg(struct scth_cfg *cfg);
int scth_get_stats(struct scth_stats *st);

/* config ioctls */
int scth_add_prog(const char *comm);
int scth_del_prog(const char *comm);
int scth_add_uid(__u32 euid);
int scth_del_uid(__u32 euid);
int scth_add_sys(__u32 nr);
int scth_del_sys(__u32 nr);

/* best effort cleanup helpers */
int scth_del_prog_ignore(const char *comm);
int scth_del_uid_ignore(__u32 euid);
int scth_del_sys_ignore(__u32 nr);
int scth_setup_base(__u32 max_active, __u32 policy);
int scth_cleanup_common(void);

/* spawn helpers */
pid_t scth_spawn_quiet(const char *prog, char *const argv[]);
int   scth_spawn_many_uname(pid_t *pids, size_t n);
int   scth_spawn_many_python_getpid(pid_t *pids, size_t n);
int   scth_wait_all(pid_t *pids, size_t n);

/* debug print */
void scth_print_cfg(const struct scth_cfg *cfg);
void scth_print_stats(const struct scth_stats *st);
