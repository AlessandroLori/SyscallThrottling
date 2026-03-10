#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "scth_ioctl.h"

static int open_dev(void)
{
    int fd = open("/dev/" SCTH_DEV_NAME, O_RDWR);
    if (fd < 0) perror("open /dev/" SCTH_DEV_NAME);
    return fd;
}

static int do_ioctl0(unsigned long req)
{
    int fd = open_dev();
    if (fd < 0) return -1;
    int rc = ioctl(fd, req);
    if (rc < 0) perror("ioctl");
    close(fd);
    return rc;
}

static int do_ioctlw(unsigned long req, const void *arg)
{
    int fd = open_dev();
    if (fd < 0) return -1;
    int rc = ioctl(fd, req, arg);
    if (rc < 0) perror("ioctl");
    close(fd);
    return rc;
}

static int ioctl_get_u32(unsigned long req, uint32_t *out)
{
    int fd = open_dev();
    if (fd < 0) return -1;
    int rc = ioctl(fd, req, out);
    if (rc < 0) perror("ioctl");
    close(fd);
    return rc;
}

static int cmd_on(void)         { return do_ioctl0(SCTH_IOC_ON) ? 1 : 0; }
static int cmd_off(void)        { return do_ioctl0(SCTH_IOC_OFF) ? 1 : 0; }
static int cmd_resetstats(void) { return do_ioctl0(SCTH_IOC_RESET_STATS) ? 1 : 0; }

static int cmd_setmax(int argc, char **argv)
{
    if (argc < 3) return -2;
    uint32_t v = (uint32_t)strtoul(argv[2], NULL, 10);
    return do_ioctlw(SCTH_IOC_SET_MAX, &v) ? 1 : 0;
}

static int cmd_setpolicy(int argc, char **argv)
{
    if (argc < 3) return -2;
    __u32 v = (__u32)strtoul(argv[2], NULL, 10);
    return do_ioctlw(SCTH_IOC_SET_POLICY, &v) ? 1 : 0;
}

static int cmd_status(void)
{
    struct scth_cfg c;
    memset(&c, 0, sizeof(c));

    int fd = open_dev();
    if (fd < 0) return 1;
    int rc = ioctl(fd, SCTH_IOC_GET_CFG, &c);
    if (rc < 0) { perror("ioctl"); close(fd); return 1; }
    close(fd);

    printf("abi=%u monitor_on=%u epoch_id=%" PRIu64 "\n", c.abi_version, c.monitor_on, (uint64_t)c.epoch_id);
    printf("max_active=%u max_pending=%u\n", c.max_active, c.max_pending);
    printf("policy_active=%u policy_pending=%u\n", c.policy_active, c.policy_pending);
    return 0;
}

static int cmd_stats(void)
{
    struct scth_stats s;
    memset(&s, 0, sizeof(s));

    int fd = open_dev();
    if (fd < 0) return 1;
    int rc = ioctl(fd, SCTH_IOC_GET_STATS, &s);
    if (rc < 0) { perror("ioctl"); close(fd); return 1; }
    close(fd);

    double avg_blocked = 0.0;
    if (s.blocked_samples) avg_blocked = (double)s.blocked_sum / (double)s.blocked_samples;

    printf("abi=%u\n", s.abi_version);
    printf("peak_delay_ns=%" PRIu64 " peak_prog=%s peak_uid=%u\n",
           (uint64_t)s.peak_delay_ns, s.peak_prog, (unsigned)s.peak_uid);
    printf("peak_blocked_threads=%u avg_blocked_threads=%.3f (samples=%" PRIu64 ")\n",
           (unsigned)s.peak_blocked_threads, avg_blocked, (uint64_t)s.blocked_samples);

    if (s.delay_num) {
        double avg_delay = (double)s.delay_sum_ns / (double)s.delay_num;
        printf("avg_delay_ns=%.0f (n=%" PRIu64 ")\n", avg_delay, (uint64_t)s.delay_num);
    }
    return 0;
}

/* ---- prog ---- */
static int cmd_addprog(int argc, char **argv)
{
    if (argc < 3) return -2;
    struct scth_prog_arg a;
    memset(&a, 0, sizeof(a));
    snprintf(a.comm, sizeof(a.comm), "%s", argv[2]);
    return do_ioctlw(SCTH_IOC_ADD_PROG, &a) ? 1 : 0;
}

static int cmd_delprog(int argc, char **argv)
{
    if (argc < 3) return -2;
    struct scth_prog_arg a;
    memset(&a, 0, sizeof(a));
    snprintf(a.comm, sizeof(a.comm), "%s", argv[2]);
    return do_ioctlw(SCTH_IOC_DEL_PROG, &a) ? 1 : 0;
}

static int cmd_listprog(void)
{
    uint32_t cnt = 0;
    if (ioctl_get_u32(SCTH_IOC_GET_PROG_COUNT, &cnt) != 0) return 1;

    printf("Programs (%u):\n", cnt);
    if (cnt == 0) return 0;

    struct scth_prog_arg *buf = calloc(cnt, sizeof(*buf));
    if (!buf) { perror("calloc"); return 1; }

    struct scth_list_req req = {
        .capacity = cnt,
        .count = 0,
        .user_ptr = (uint64_t)(uintptr_t)buf,
    };

    int fd = open_dev();
    if (fd < 0) { free(buf); return 1; }
    int rc = ioctl(fd, SCTH_IOC_GET_PROG_LIST, &req);
    if (rc < 0) perror("ioctl");
    close(fd);
    if (rc < 0) { free(buf); return 1; }

    for (uint32_t i = 0; i < req.count; i++)
        printf("  %s\n", buf[i].comm);

    free(buf);
    return 0;
}

/* ---- uid ---- */
static int cmd_adduid(int argc, char **argv)
{
    if (argc < 3) return -2;
    struct scth_uid_arg a = { .euid = (uint32_t)strtoul(argv[2], NULL, 10) };
    return do_ioctlw(SCTH_IOC_ADD_UID, &a) ? 1 : 0;
}

static int cmd_deluid(int argc, char **argv)
{
    if (argc < 3) return -2;
    struct scth_uid_arg a = { .euid = (uint32_t)strtoul(argv[2], NULL, 10) };
    return do_ioctlw(SCTH_IOC_DEL_UID, &a) ? 1 : 0;
}

static int cmd_listuid(void)
{
    uint32_t cnt = 0;
    if (ioctl_get_u32(SCTH_IOC_GET_UID_COUNT, &cnt) != 0) return 1;

    printf("UIDs (%u):\n", cnt);
    if (cnt == 0) return 0;

    uint32_t *buf = calloc(cnt, sizeof(*buf));
    if (!buf) { perror("calloc"); return 1; }

    struct scth_list_req req = {
        .capacity = cnt,
        .count = 0,
        .user_ptr = (uint64_t)(uintptr_t)buf,
    };

    int fd = open_dev();
    if (fd < 0) { free(buf); return 1; }
    int rc = ioctl(fd, SCTH_IOC_GET_UID_LIST, &req);
    if (rc < 0) perror("ioctl");
    close(fd);
    if (rc < 0) { free(buf); return 1; }

    for (uint32_t i = 0; i < req.count; i++)
        printf("  %u\n", buf[i]);

    free(buf);
    return 0;
}

/* ---- sys ---- */
static int cmd_addsys(int argc, char **argv)
{
    if (argc < 3) return -2;
    struct scth_sys_arg a = { .nr = (uint32_t)strtoul(argv[2], NULL, 10) };
    return do_ioctlw(SCTH_IOC_ADD_SYS, &a) ? 1 : 0;
}

static int cmd_delsys(int argc, char **argv)
{
    if (argc < 3) return -2;
    struct scth_sys_arg a = { .nr = (uint32_t)strtoul(argv[2], NULL, 10) };
    return do_ioctlw(SCTH_IOC_DEL_SYS, &a) ? 1 : 0;
}

static int cmd_listsys(void)
{
    uint32_t cnt = 0;
    if (ioctl_get_u32(SCTH_IOC_GET_SYS_COUNT, &cnt) != 0) return 1;

    printf("Syscalls (%u):\n", cnt);
    if (cnt == 0) return 0;

    uint32_t *buf = calloc(cnt, sizeof(*buf));
    if (!buf) { perror("calloc"); return 1; }

    struct scth_list_req req = {
        .capacity = cnt,
        .count = 0,
        .user_ptr = (uint64_t)(uintptr_t)buf,
    };

    int fd = open_dev();
    if (fd < 0) { free(buf); return 1; }
    int rc = ioctl(fd, SCTH_IOC_GET_SYS_LIST, &req);
    if (rc < 0) perror("ioctl");
    close(fd);
    if (rc < 0) { free(buf); return 1; }

    for (uint32_t i = 0; i < req.count; i++)
        printf("  %u\n", buf[i]);

    free(buf);
    return 0;
}

/* ---- dispatcher ---- */
int cmd_run(int argc, char **argv)
{
    const char *cmd = argv[1];

    if (!strcmp(cmd, "on")) return cmd_on();
    if (!strcmp(cmd, "off")) return cmd_off();
    if (!strcmp(cmd, "setmax")) return cmd_setmax(argc, argv);
    if (!strcmp(cmd, "setpolicy")) return cmd_setpolicy(argc, argv);
    if (!strcmp(cmd, "resetstats")) return cmd_resetstats();
    if (!strcmp(cmd, "status")) return cmd_status();
    if (!strcmp(cmd, "stats")) return cmd_stats();

    if (!strcmp(cmd, "addprog")) return cmd_addprog(argc, argv);
    if (!strcmp(cmd, "delprog")) return cmd_delprog(argc, argv);
    if (!strcmp(cmd, "listprog")) return cmd_listprog();

    if (!strcmp(cmd, "adduid")) return cmd_adduid(argc, argv);
    if (!strcmp(cmd, "deluid")) return cmd_deluid(argc, argv);
    if (!strcmp(cmd, "listuid")) return cmd_listuid();

    if (!strcmp(cmd, "addsys")) return cmd_addsys(argc, argv);
    if (!strcmp(cmd, "delsys")) return cmd_delsys(argc, argv);
    if (!strcmp(cmd, "listsys")) return cmd_listsys();

    return -2;
}