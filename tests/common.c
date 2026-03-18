#include "common.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int open_dev(void)
{
    char path[128];
    snprintf(path, sizeof(path), "/dev/%s", SCTH_DEV_NAME);
    return open(path, O_RDWR);
}

static int do_ioctl0(unsigned long req)
{
    int fd = open_dev();
    if (fd < 0)
        return -errno;

    if (ioctl(fd, req) != 0) {
        int e = errno;
        close(fd);
        return -e;
    }

    close(fd);
    return 0;
}

static int do_ioctl_ptr(unsigned long req, void *arg)
{
    int fd = open_dev();
    if (fd < 0)
        return -errno;

    if (ioctl(fd, req, arg) != 0) {
        int e = errno;
        close(fd);
        return -e;
    }

    close(fd);
    return 0;
}

int scth_require_root(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "[FAIL] run this test with sudo/root\n");
        return 1;
    }
    return 0;
}

const char *scth_policy_name(__u32 p)
{
    switch (p) {
    case SCTH_POLICY_FIFO_STRICT: return "FIFO_STRICT";
    case SCTH_POLICY_WAKE_RACE:   return "WAKE_RACE";
    default: return "UNKNOWN";
    }
}

int scth_on(void)                { return do_ioctl0(SCTH_IOC_ON); }
int scth_off(void)               { return do_ioctl0(SCTH_IOC_OFF); }
int scth_resetstats(void)        { return do_ioctl0(SCTH_IOC_RESET_STATS); }
int scth_setmax(__u32 v)         { return do_ioctl_ptr(SCTH_IOC_SET_MAX, &v); }
int scth_setpolicy(__u32 v)      { return do_ioctl_ptr(SCTH_IOC_SET_POLICY, &v); }
int scth_get_cfg(struct scth_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    return do_ioctl_ptr(SCTH_IOC_GET_CFG, cfg);
}
int scth_get_stats(struct scth_stats *st)
{
    memset(st, 0, sizeof(*st));
    return do_ioctl_ptr(SCTH_IOC_GET_STATS, st);
}

int scth_add_prog(const char *comm)
{
    struct scth_prog_arg a;
    memset(&a, 0, sizeof(a));
    snprintf(a.comm, sizeof(a.comm), "%s", comm);
    return do_ioctl_ptr(SCTH_IOC_ADD_PROG, &a);
}

int scth_del_prog(const char *comm)
{
    struct scth_prog_arg a;
    memset(&a, 0, sizeof(a));
    snprintf(a.comm, sizeof(a.comm), "%s", comm);
    return do_ioctl_ptr(SCTH_IOC_DEL_PROG, &a);
}

int scth_add_uid(__u32 euid)
{
    struct scth_uid_arg a = { .euid = euid };
    return do_ioctl_ptr(SCTH_IOC_ADD_UID, &a);
}

int scth_del_uid(__u32 euid)
{
    struct scth_uid_arg a = { .euid = euid };
    return do_ioctl_ptr(SCTH_IOC_DEL_UID, &a);
}

int scth_add_sys(__u32 nr)
{
    struct scth_sys_arg a = { .nr = nr };
    return do_ioctl_ptr(SCTH_IOC_ADD_SYS, &a);
}

int scth_del_sys(__u32 nr)
{
    struct scth_sys_arg a = { .nr = nr };
    return do_ioctl_ptr(SCTH_IOC_DEL_SYS, &a);
}

int scth_del_prog_ignore(const char *comm)
{
    int rc = scth_del_prog(comm);
    if (rc == -ENOENT)
        return 0;
    return rc;
}

int scth_del_uid_ignore(__u32 euid)
{
    int rc = scth_del_uid(euid);
    if (rc == -ENOENT)
        return 0;
    return rc;
}

int scth_del_sys_ignore(__u32 nr)
{
    int rc = scth_del_sys(nr);
    if (rc == -ENOENT)
        return 0;
    return rc;
}

int scth_setup_base(__u32 max_active, __u32 policy)
{
    int rc;
    rc = scth_off(); if (rc < 0) return rc;
    rc = scth_resetstats(); if (rc < 0) return rc;
    rc = scth_setmax(max_active); if (rc < 0) return rc;
    rc = scth_setpolicy(policy); if (rc < 0) return rc;
    rc = scth_on(); if (rc < 0) return rc;
    return 0;
}

int scth_cleanup_common(void)
{
    int rc;
    rc = scth_del_sys_ignore(63); if (rc < 0) return rc;
    rc = scth_del_sys_ignore(39); if (rc < 0) return rc;
    rc = scth_del_prog_ignore("uname"); if (rc < 0) return rc;
    rc = scth_del_prog_ignore("python3"); if (rc < 0) return rc;
    rc = scth_del_uid_ignore((__u32)geteuid()); if (rc < 0) return rc;
    return 0;
}

pid_t scth_spawn_quiet(const char *prog, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO)
                close(fd);
        }
        execvp(prog, argv);
        _exit(127);
    }

    return pid;
}

int scth_spawn_many_uname(pid_t *pids, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char *const argv[] = { (char *)"uname", NULL };
        pids[i] = scth_spawn_quiet("uname", argv);
        if (pids[i] < 0)
            return -errno;
    }
    return 0;
}

int scth_spawn_many_python_getpid(pid_t *pids, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char *const argv[] = {
            (char *)"python3",
            (char *)"-c",
            (char *)"import os; os.getpid()",
            NULL
        };
        pids[i] = scth_spawn_quiet("python3", argv);
        if (pids[i] < 0)
            return -errno;
    }
    return 0;
}

int scth_wait_all(pid_t *pids, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int status = 0;
        if (waitpid(pids[i], &status, 0) < 0)
            return -errno;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return -ECHILD;
    }
    return 0;
}

void scth_print_cfg(const struct scth_cfg *cfg)
{
    printf("abi=%u monitor_on=%u epoch_id=%" PRIu64 "\n",
           cfg->abi_version, cfg->monitor_on, (uint64_t)cfg->epoch_id);
    printf("max_active=%u max_pending=%u\n", cfg->max_active, cfg->max_pending);
    printf("policy_active=%u(%s) policy_pending=%u(%s)\n",
           cfg->policy_active, scth_policy_name(cfg->policy_active),
           cfg->policy_pending, scth_policy_name(cfg->policy_pending));
}

void scth_print_stats(const struct scth_stats *s)
{
    double avg_blocked = 0.0;
    double avg_delay = 0.0;

    if (s->blocked_num_samples)
        avg_blocked = (double)s->blocked_sum_samples / (double)s->blocked_num_samples;
    if (s->delay_num)
        avg_delay = (double)s->delay_sum_ns / (double)s->delay_num;

    printf("abi=%u\n", s->abi_version);
    printf("peak_delay_ns=%" PRIu64 " peak_prog=%s peak_uid=%u\n",
           (uint64_t)s->peak_delay_ns, s->peak_comm, (unsigned)s->peak_euid);
    printf("peak_blocked_threads=%u avg_blocked_threads=%.3f (samples=%" PRIu64 ")\n",
           (unsigned)s->peak_blocked_threads, avg_blocked,
           (uint64_t)s->blocked_num_samples);
    printf("blocked_sum_samples=%" PRIu64 " blocked_num_samples=%" PRIu64 "\n",
           (uint64_t)s->blocked_sum_samples, (uint64_t)s->blocked_num_samples);
    printf("delay_sum_ns=%" PRIu64 " delay_num=%" PRIu64 " avg_delay_ns=%.0f\n",
           (uint64_t)s->delay_sum_ns, (uint64_t)s->delay_num, avg_delay);
    printf("total_tracked=%" PRIu64 " total_immediate=%" PRIu64
           " total_delayed=%" PRIu64 " total_aborted=%" PRIu64 "\n",
           (uint64_t)s->total_tracked, (uint64_t)s->total_immediate,
           (uint64_t)s->total_delayed, (uint64_t)s->total_aborted);
    printf("peak_fifo_qlen=%u current_fifo_qlen=%u\n",
           (unsigned)s->peak_fifo_qlen, (unsigned)s->current_fifo_qlen);
    printf("epoch_id=%" PRIu64 " last_epoch_used=%u max_active=%u\n",
           (uint64_t)s->epoch_id, (unsigned)s->last_epoch_used, (unsigned)s->max_active);
    printf("policy_active=%u(%s) policy_pending=%u(%s)\n",
           (unsigned)s->policy_active, scth_policy_name(s->policy_active),
           (unsigned)s->policy_pending, scth_policy_name(s->policy_pending));
}
