#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

#include "scth_ioctl.h"

#define DEV_PATH "/dev/scthrottle"

static int open_dev(void)
{
    int fd = open(DEV_PATH, O_RDWR);
    if (fd < 0)
        perror("open " DEV_PATH);
    return fd;
}

static int do_ioctl0(unsigned long req)
{
    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, req) != 0) {
        perror("ioctl");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int do_ioctl_w_u32(unsigned long req, __u32 v)
{
    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, req, &v) != 0) {
        perror("ioctl");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int do_ioctl_w_u8(unsigned long req, __u8 v)
{
    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, req, &v) != 0) {
        perror("ioctl");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int ioctl_get_u32(unsigned long req, __u32 *out)
{
    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, req, out) != 0) {
        perror("ioctl");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

/* ---------------- status/stats ---------------- */

static int cmd_status(void)
{
    int fd = open_dev();
    if (fd < 0) return 1;

    struct scth_cfg cfg;
    if (ioctl(fd, SCTH_IOC_GET_CFG, &cfg) != 0) {
        perror("ioctl GET_CFG");
        close(fd);
        return 1;
    }
    close(fd);

    printf("abi=%u monitor_on=%u epoch_id=%llu\n",
           cfg.abi_version, cfg.monitor_on,
           (unsigned long long)cfg.epoch_id);

    printf("max_active=%u max_pending=%u\n",
           cfg.max_active, cfg.max_pending);

    printf("policy_active=%u policy_pending=%u\n",
           cfg.policy_active, cfg.policy_pending);

    return 0;
}

static int cmd_stats(void)
{
    int fd = open_dev();
    if (fd < 0) return 1;

    struct scth_stats st;
    if (ioctl(fd, SCTH_IOC_GET_STATS, &st) != 0) {
        perror("ioctl GET_STATS");
        close(fd);
        return 1;
    }
    close(fd);

    double avg_blocked = 0.0;
    if (st.blocked_num_samples)
        avg_blocked = (double)st.blocked_sum_samples / (double)st.blocked_num_samples;

    printf("abi=%u\n", st.abi_version);
    printf("peak_delay_ns=%llu peak_prog=%s peak_uid=%u\n",
           (unsigned long long)st.peak_delay_ns,
           st.peak_comm,
           st.peak_euid);
    printf("peak_blocked_threads=%u avg_blocked_threads=%.3f (samples=%llu)\n",
           st.peak_blocked_threads,
           avg_blocked,
           (unsigned long long)st.blocked_num_samples);

    return 0;
}

/* ---------------- add/del ---------------- */

static int cmd_addprog(const char *s)
{
    struct scth_prog_arg a;
    memset(&a, 0, sizeof(a));
    strncpy(a.comm, s, SCTH_COMM_LEN - 1);

    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, SCTH_IOC_ADD_PROG, &a) != 0) {
        perror("ioctl ADD_PROG");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int cmd_delprog(const char *s)
{
    struct scth_prog_arg a;
    memset(&a, 0, sizeof(a));
    strncpy(a.comm, s, SCTH_COMM_LEN - 1);

    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, SCTH_IOC_DEL_PROG, &a) != 0) {
        perror("ioctl DEL_PROG");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int cmd_adduid(__u32 euid)
{
    struct scth_uid_arg a = { .euid = euid };

    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, SCTH_IOC_ADD_UID, &a) != 0) {
        perror("ioctl ADD_UID");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int cmd_deluid(__u32 euid)
{
    struct scth_uid_arg a = { .euid = euid };

    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, SCTH_IOC_DEL_UID, &a) != 0) {
        perror("ioctl DEL_UID");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int cmd_addsys(__u32 nr)
{
    struct scth_sys_arg a = { .nr = nr };

    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, SCTH_IOC_ADD_SYS, &a) != 0) {
        perror("ioctl ADD_SYS");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int cmd_delsys(__u32 nr)
{
    struct scth_sys_arg a = { .nr = nr };

    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, SCTH_IOC_DEL_SYS, &a) != 0) {
        perror("ioctl DEL_SYS");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

/* ---------------- list (two-step, retry on ENOSPC) ---------------- */

static int cmd_listprog(void)
{
    __u32 cnt = 0;
    if (ioctl_get_u32(SCTH_IOC_GET_PROG_COUNT, &cnt)) return 1;

    __u32 cap = cnt ? cnt : 1;

    for (int attempt = 0; attempt < 2; attempt++) {
        struct scth_prog_arg *buf = calloc(cap, sizeof(*buf));
        if (!buf) { perror("calloc"); return 1; }

        struct scth_list_req req;
        req.capacity = cap;
        req.count = 0;
        req.user_ptr = (__u64)(uintptr_t)buf;

        int fd = open_dev();
        if (fd < 0) { free(buf); return 1; }

        if (ioctl(fd, SCTH_IOC_GET_PROG_LIST, &req) != 0) {
            int err = errno;
            close(fd);
            free(buf);

            if (err == ENOSPC && req.count > cap) {
                cap = req.count;
                continue;
            }

            errno = err;
            perror("ioctl GET_PROG_LIST");
            return 1;
        }
        close(fd);

        printf("Programs (%u):\n", req.count);
        for (__u32 i = 0; i < req.count; i++)
            printf("  %s\n", buf[i].comm);

        free(buf);
        return 0;
    }

    fprintf(stderr, "listprog: too many retries\n");
    return 1;
}

static int cmd_listuid(void)
{
    __u32 cnt = 0;
    if (ioctl_get_u32(SCTH_IOC_GET_UID_COUNT, &cnt)) return 1;

    __u32 cap = cnt ? cnt : 1;

    for (int attempt = 0; attempt < 2; attempt++) {
        __u32 *buf = calloc(cap, sizeof(*buf));
        if (!buf) { perror("calloc"); return 1; }

        struct scth_list_req req;
        req.capacity = cap;
        req.count = 0;
        req.user_ptr = (__u64)(uintptr_t)buf;

        int fd = open_dev();
        if (fd < 0) { free(buf); return 1; }

        if (ioctl(fd, SCTH_IOC_GET_UID_LIST, &req) != 0) {
            int err = errno;
            close(fd);
            free(buf);

            if (err == ENOSPC && req.count > cap) {
                cap = req.count;
                continue;
            }

            errno = err;
            perror("ioctl GET_UID_LIST");
            return 1;
        }
        close(fd);

        printf("UIDs (%u):\n", req.count);
        for (__u32 i = 0; i < req.count; i++)
            printf("  %u\n", buf[i]);

        free(buf);
        return 0;
    }

    fprintf(stderr, "listuid: too many retries\n");
    return 1;
}

static int cmd_listsys(void)
{
    __u32 cnt = 0;
    if (ioctl_get_u32(SCTH_IOC_GET_SYS_COUNT, &cnt)) return 1;

    __u32 cap = cnt ? cnt : 1;

    for (int attempt = 0; attempt < 2; attempt++) {
        __u32 *buf = calloc(cap, sizeof(*buf));
        if (!buf) { perror("calloc"); return 1; }

        struct scth_list_req req;
        req.capacity = cap;
        req.count = 0;
        req.user_ptr = (__u64)(uintptr_t)buf;

        int fd = open_dev();
        if (fd < 0) { free(buf); return 1; }

        if (ioctl(fd, SCTH_IOC_GET_SYS_LIST, &req) != 0) {
            int err = errno;
            close(fd);
            free(buf);

            if (err == ENOSPC && req.count > cap) {
                cap = req.count;
                continue;
            }

            errno = err;
            perror("ioctl GET_SYS_LIST");
            return 1;
        }
        close(fd);

        printf("Syscalls (%u):\n", req.count);
        for (__u32 i = 0; i < req.count; i++)
            printf("  %u\n", buf[i]);

        free(buf);
        return 0;
    }

    fprintf(stderr, "listsys: too many retries\n");
    return 1;
}

/* ---------------- cmd dispatcher ---------------- */

int cmd_run(int argc, char **argv)
{
    const char *cmd = argv[1];

    if (!strcmp(cmd, "on")) return do_ioctl0(SCTH_IOC_ON);
    if (!strcmp(cmd, "off")) return do_ioctl0(SCTH_IOC_OFF);

    if (!strcmp(cmd, "setmax")) {
        if (argc != 3) return -2;
        __u32 v = (__u32)strtoul(argv[2], NULL, 10);
        return do_ioctl_w_u32(SCTH_IOC_SET_MAX, v);
    }

    if (!strcmp(cmd, "setpolicy")) {
        if (argc != 3) return -2;
        __u8 p = (__u8)strtoul(argv[2], NULL, 10);
        return do_ioctl_w_u8(SCTH_IOC_SET_POLICY, p);
    }

    if (!strcmp(cmd, "resetstats")) return do_ioctl0(SCTH_IOC_RESET_STATS);
    if (!strcmp(cmd, "status")) return cmd_status();
    if (!strcmp(cmd, "stats")) return cmd_stats();

    if (!strcmp(cmd, "addprog")) { if (argc != 3) return -2; return cmd_addprog(argv[2]); }
    if (!strcmp(cmd, "delprog")) { if (argc != 3) return -2; return cmd_delprog(argv[2]); }
    if (!strcmp(cmd, "listprog")) return cmd_listprog();

    if (!strcmp(cmd, "adduid")) { if (argc != 3) return -2; return cmd_adduid((__u32)strtoul(argv[2], NULL, 10)); }
    if (!strcmp(cmd, "deluid")) { if (argc != 3) return -2; return cmd_deluid((__u32)strtoul(argv[2], NULL, 10)); }
    if (!strcmp(cmd, "listuid")) return cmd_listuid();

    if (!strcmp(cmd, "addsys")) { if (argc != 3) return -2; return cmd_addsys((__u32)strtoul(argv[2], NULL, 10)); }
    if (!strcmp(cmd, "delsys")) { if (argc != 3) return -2; return cmd_delsys((__u32)strtoul(argv[2], NULL, 10)); }
    if (!strcmp(cmd, "listsys")) return cmd_listsys();

    return -2;
}