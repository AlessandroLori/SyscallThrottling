#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "../include/scth_ioctl.h"

/* --- compat: se qualcuno ha rinominato ON/OFF --- */
#ifndef SCTH_IOC_ON
#ifdef SCTH_IOC_MONITOR_ON
#define SCTH_IOC_ON SCTH_IOC_MONITOR_ON
#endif
#endif

#ifndef SCTH_IOC_OFF
#ifdef SCTH_IOC_MONITOR_OFF
#define SCTH_IOC_OFF SCTH_IOC_MONITOR_OFF
#endif
#endif

#ifndef SCTH_DEV_NAME
#define SCTH_DEV_NAME "scthrottle"
#endif

/* ---- open device ---- */
static int open_dev(void)
{
    char path[128];
    snprintf(path, sizeof(path), "/dev/%s", SCTH_DEV_NAME);

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        perror("open /dev/scthrottle");
    }
    return fd;
}

static int do_ioctl0(unsigned long req)
{
    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, req) != 0) { perror("ioctl"); close(fd); return 1; }
    close(fd);
    return 0;
}

static int do_ioctl_u32(unsigned long req, uint32_t v)
{
    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, req, &v) != 0) { perror("ioctl"); close(fd); return 1; }
    close(fd);
    return 0;
}

/* ============================================================
 * list_req ABI-agnostic:
 * assumiamo layout tipico:
 *   offset 0: u32 cap
 *   offset 4: u32 count
 *   offset 8: u64 user_ptr
 * così NON dipendiamo dai NOMI dei campi (capacity/user_ptr vs cap/ptr)
 * ============================================================ */
typedef char _scth_list_req_size_check[(sizeof(struct scth_list_req) >= 16) ? 1 : -1];

static void listreq_init(struct scth_list_req *r, uint32_t cap, void *ptr)
{
    memset(r, 0, sizeof(*r));
    ((uint32_t *)r)[0] = cap; /* cap/capacity */
    ((uint32_t *)r)[1] = 0;   /* count (out) */
    *(uint64_t *)((uint8_t *)r + 8) = (uint64_t)(uintptr_t)ptr; /* ptr/user_ptr */
}

static uint32_t listreq_count(const struct scth_list_req *r)
{
    return ((const uint32_t *)r)[1];
}

/* ---- commands ---- */
static void usage(void)
{
    printf("Usage:\n");
    printf("  ./scthctl on | off\n");
    printf("  ./scthctl setmax <max_per_sec>\n");
    printf("  ./scthctl setpolicy <0|1>   (0=FIFO, 1=WAKE_RACE)\n");
    printf("  ./scthctl resetstats\n");
    printf("  ./scthctl status\n");
    printf("  ./scthctl stats\n");
    printf("  ./scthctl addprog <comm> | delprog <comm> | listprog\n");
    printf("  ./scthctl adduid <euid>  | deluid <euid>  | listuid\n");
    printf("  ./scthctl addsys <nr>    | delsys <nr>    | listsys\n");
}

static int cmd_on(void)        { return do_ioctl0(SCTH_IOC_ON); }
static int cmd_off(void)       { return do_ioctl0(SCTH_IOC_OFF); }
static int cmd_resetstats(void){ return do_ioctl0(SCTH_IOC_RESET_STATS); }

static int cmd_setmax(const char *s)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || *s == 0 || (end && *end != 0) || v < 0 || v > 1000000) {
        fprintf(stderr, "setmax: valore non valido\n");
        return 1;
    }
    return do_ioctl_u32(SCTH_IOC_SET_MAX, (uint32_t)v);
}

static int cmd_setpolicy(const char *s)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || *s == 0 || (end && *end != 0) || (v != 0 && v != 1)) {
        fprintf(stderr, "setpolicy: usa 0 (FIFO) o 1 (WAKE_RACE)\n");
        return 1;
    }
    return do_ioctl_u32(SCTH_IOC_SET_POLICY, (uint32_t)v);
}

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

    printf("max_active=%u max_pending=%u\n", cfg.max_active, cfg.max_pending);
    printf("policy_active=%u policy_pending=%u\n", cfg.policy_active, cfg.policy_pending);
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
           (unsigned long long)st.peak_delay_ns, st.peak_comm, st.peak_euid);
    printf("peak_blocked_threads=%u avg_blocked_threads=%.3f (samples=%llu)\n",
           st.peak_blocked_threads, avg_blocked,
           (unsigned long long)st.blocked_num_samples);
    return 0;
}

/* ---- add/del prog ---- */
static int cmd_addprog(const char *s)
{
    struct scth_prog_arg a;
    memset(&a, 0, sizeof(a));
    strncpy(a.comm, s, SCTH_COMM_LEN - 1);

    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, SCTH_IOC_ADD_PROG, &a) != 0) { perror("ioctl ADD_PROG"); close(fd); return 1; }
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
    if (ioctl(fd, SCTH_IOC_DEL_PROG, &a) != 0) { perror("ioctl DEL_PROG"); close(fd); return 1; }
    close(fd);
    return 0;
}

static int cmd_listprog(void)
{
    int fd = open_dev();
    if (fd < 0) return 1;

    uint32_t n = 0;
    if (ioctl(fd, SCTH_IOC_GET_PROG_COUNT, &n) != 0) {
        perror("ioctl GET_PROG_COUNT");
        close(fd);
        return 1;
    }

    printf("Programs (%u):\n", n);
    if (n == 0) { close(fd); return 0; }

    uint32_t cap = n;
    struct scth_prog_arg *buf = calloc(cap, sizeof(*buf));
    if (!buf) { perror("calloc"); close(fd); return 1; }

    struct scth_list_req req;
    listreq_init(&req, cap, buf);

    if (ioctl(fd, SCTH_IOC_GET_PROG_LIST, &req) != 0) {
        perror("ioctl GET_PROG_LIST");
        free(buf);
        close(fd);
        return 1;
    }

    uint32_t got = listreq_count(&req);
    if (got > cap) got = cap;

    for (uint32_t i = 0; i < got; i++)
        printf("  %s\n", buf[i].comm);

    free(buf);
    close(fd);
    return 0;
}

/* ---- add/del uid ---- */
static int cmd_adduid(const char *s)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || *s == 0 || (end && *end != 0) || v < 0 || v > 0xffffffff) {
        fprintf(stderr, "adduid: valore non valido\n");
        return 1;
    }
    uint32_t uid = (uint32_t)v;

    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, SCTH_IOC_ADD_UID, &uid) != 0) { perror("ioctl ADD_UID"); close(fd); return 1; }
    close(fd);
    return 0;
}

static int cmd_deluid(const char *s)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || *s == 0 || (end && *end != 0) || v < 0 || v > 0xffffffff) {
        fprintf(stderr, "deluid: valore non valido\n");
        return 1;
    }
    uint32_t uid = (uint32_t)v;

    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, SCTH_IOC_DEL_UID, &uid) != 0) { perror("ioctl DEL_UID"); close(fd); return 1; }
    close(fd);
    return 0;
}

static int cmd_listuid(void)
{
    int fd = open_dev();
    if (fd < 0) return 1;

    uint32_t n = 0;
    if (ioctl(fd, SCTH_IOC_GET_UID_COUNT, &n) != 0) {
        perror("ioctl GET_UID_COUNT");
        close(fd);
        return 1;
    }

    printf("UIDs (%u):\n", n);
    if (n == 0) { close(fd); return 0; }

    uint32_t cap = n;
    uint32_t *buf = calloc(cap, sizeof(*buf));
    if (!buf) { perror("calloc"); close(fd); return 1; }

    struct scth_list_req req;
    listreq_init(&req, cap, buf);

    if (ioctl(fd, SCTH_IOC_GET_UID_LIST, &req) != 0) {
        perror("ioctl GET_UID_LIST");
        free(buf);
        close(fd);
        return 1;
    }

    uint32_t got = listreq_count(&req);
    if (got > cap) got = cap;

    for (uint32_t i = 0; i < got; i++)
        printf("  %u\n", buf[i]);

    free(buf);
    close(fd);
    return 0;
}

/* ---- add/del sys ---- */
static int cmd_addsys(const char *s)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || *s == 0 || (end && *end != 0) || v < 0 || v > 0xffffffff) {
        fprintf(stderr, "addsys: valore non valido\n");
        return 1;
    }
    uint32_t nr = (uint32_t)v;

    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, SCTH_IOC_ADD_SYS, &nr) != 0) { perror("ioctl ADD_SYS"); close(fd); return 1; }
    close(fd);
    return 0;
}

static int cmd_delsys(const char *s)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || *s == 0 || (end && *end != 0) || v < 0 || v > 0xffffffff) {
        fprintf(stderr, "delsys: valore non valido\n");
        return 1;
    }
    uint32_t nr = (uint32_t)v;

    int fd = open_dev();
    if (fd < 0) return 1;
    if (ioctl(fd, SCTH_IOC_DEL_SYS, &nr) != 0) { perror("ioctl DEL_SYS"); close(fd); return 1; }
    close(fd);
    return 0;
}

static int cmd_listsys(void)
{
    int fd = open_dev();
    if (fd < 0) return 1;

    uint32_t n = 0;
    if (ioctl(fd, SCTH_IOC_GET_SYS_COUNT, &n) != 0) {
        perror("ioctl GET_SYS_COUNT");
        close(fd);
        return 1;
    }

    printf("Syscalls (%u):\n", n);
    if (n == 0) { close(fd); return 0; }

    uint32_t cap = n;
    uint32_t *buf = calloc(cap, sizeof(*buf));
    if (!buf) { perror("calloc"); close(fd); return 1; }

    struct scth_list_req req;
    listreq_init(&req, cap, buf);

    if (ioctl(fd, SCTH_IOC_GET_SYS_LIST, &req) != 0) {
        perror("ioctl GET_SYS_LIST");
        free(buf);
        close(fd);
        return 1;
    }

    uint32_t got = listreq_count(&req);
    if (got > cap) got = cap;

    for (uint32_t i = 0; i < got; i++)
        printf("  %u\n", buf[i]);

    free(buf);
    close(fd);
    return 0;
}

/* ---- dispatcher ---- */
int cmd_run(int argc, char **argv)
{
    if (argc <= 0) { usage(); return 1; }

    const char *cmd = argv[0];

    if (!strcmp(cmd, "help")) { usage(); return 0; }

    if (!strcmp(cmd, "on")) return cmd_on();
    if (!strcmp(cmd, "off")) return cmd_off();

    if (!strcmp(cmd, "setmax")) {
        if (argc < 2) { usage(); return 1; }
        return cmd_setmax(argv[1]);
    }

    if (!strcmp(cmd, "setpolicy")) {
        if (argc < 2) { usage(); return 1; }
        return cmd_setpolicy(argv[1]);
    }

    if (!strcmp(cmd, "resetstats")) return cmd_resetstats();
    if (!strcmp(cmd, "status")) return cmd_status();
    if (!strcmp(cmd, "stats")) return cmd_stats();

    if (!strcmp(cmd, "addprog")) { if (argc < 2) { usage(); return 1; } return cmd_addprog(argv[1]); }
    if (!strcmp(cmd, "delprog")) { if (argc < 2) { usage(); return 1; } return cmd_delprog(argv[1]); }
    if (!strcmp(cmd, "listprog")) return cmd_listprog();

    if (!strcmp(cmd, "adduid")) { if (argc < 2) { usage(); return 1; } return cmd_adduid(argv[1]); }
    if (!strcmp(cmd, "deluid")) { if (argc < 2) { usage(); return 1; } return cmd_deluid(argv[1]); }
    if (!strcmp(cmd, "listuid")) return cmd_listuid();

    if (!strcmp(cmd, "addsys")) { if (argc < 2) { usage(); return 1; } return cmd_addsys(argv[1]); }
    if (!strcmp(cmd, "delsys")) { if (argc < 2) { usage(); return 1; } return cmd_delsys(argv[1]); }
    if (!strcmp(cmd, "listsys")) return cmd_listsys();

    fprintf(stderr, "Comando sconosciuto: %s\n", cmd);
    usage();
    return 1;
}