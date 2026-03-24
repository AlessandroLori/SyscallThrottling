#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "scth_ioctl.h"

/*
gcc -O2 -Wall -Wextra -pthread -I../../include -o sysgen sysgen.c

./sysgen --list-safe

sudo ./sysgen --interactive

sudo ./sysgen --nr 63 --threads 20 --calls 1 --max 3 --policy fifo --mode safe --match prog
sudo ./sysgen --nr 62 --threads 30 --calls 5 --max 4 --policy wake --mode safe --match prog
sudo ./sysgen --nr 64 --threads 12 --calls 2 --max 2 --policy fifo --mode safe --match uid


*/

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

union semun_local {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
    struct seminfo *__buf;
};

enum run_mode {
    MODE_SAFE = 0,
    MODE_RAW  = 1,
};

enum match_mode {
    MATCH_PROG = 0,
    MATCH_UID  = 1,
};

struct options {
    long nr;
    unsigned threads;
    unsigned calls_per_thread;
    unsigned max_active;
    __u32 policy;
    enum run_mode mode;
    enum match_mode match;
    bool auto_config;
    bool cleanup_after;
    bool reset_stats;
    unsigned delay_us_between_calls;
};

struct worker_arg {
    const struct options *opt;
    pthread_barrier_t *bar;
    unsigned index;
    uint64_t errors;
};

static const char *policy_name(__u32 p)
{
    switch (p) {
    case SCTH_POLICY_FIFO_STRICT: return "FIFO_STRICT";
    case SCTH_POLICY_WAKE_RACE:   return "WAKE_RACE";
    default: return "UNKNOWN";
    }
}

static const char *mode_name(enum run_mode m)
{
    return m == MODE_SAFE ? "safe" : "raw";
}

static const char *match_name(enum match_mode m)
{
    return m == MATCH_PROG ? "program-name" : "effective-uid";
}

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

static int scth_on(void)                { return do_ioctl0(SCTH_IOC_ON); }
static int scth_off(void)               { return do_ioctl0(SCTH_IOC_OFF); }
static int scth_resetstats(void)        { return do_ioctl0(SCTH_IOC_RESET_STATS); }
static int scth_setmax(__u32 v)         { return do_ioctl_ptr(SCTH_IOC_SET_MAX, &v); }
static int scth_setpolicy(__u32 v)      { return do_ioctl_ptr(SCTH_IOC_SET_POLICY, &v); }
static int scth_get_cfg(struct scth_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    return do_ioctl_ptr(SCTH_IOC_GET_CFG, cfg);
}
static int scth_get_stats(struct scth_stats *st)
{
    memset(st, 0, sizeof(*st));
    return do_ioctl_ptr(SCTH_IOC_GET_STATS, st);
}
static int scth_add_prog(const char *comm)
{
    struct scth_prog_arg a;
    memset(&a, 0, sizeof(a));
    snprintf(a.comm, sizeof(a.comm), "%s", comm);
    return do_ioctl_ptr(SCTH_IOC_ADD_PROG, &a);
}
static int scth_del_prog(const char *comm)
{
    struct scth_prog_arg a;
    memset(&a, 0, sizeof(a));
    snprintf(a.comm, sizeof(a.comm), "%s", comm);
    return do_ioctl_ptr(SCTH_IOC_DEL_PROG, &a);
}
static int scth_add_uid(__u32 euid)
{
    struct scth_uid_arg a = { .euid = euid };
    return do_ioctl_ptr(SCTH_IOC_ADD_UID, &a);
}
static int scth_del_uid(__u32 euid)
{
    struct scth_uid_arg a = { .euid = euid };
    return do_ioctl_ptr(SCTH_IOC_DEL_UID, &a);
}
static int scth_add_sys(__u32 nr)
{
    struct scth_sys_arg a = { .nr = nr };
    return do_ioctl_ptr(SCTH_IOC_ADD_SYS, &a);
}
static int scth_del_sys(__u32 nr)
{
    struct scth_sys_arg a = { .nr = nr };
    return do_ioctl_ptr(SCTH_IOC_DEL_SYS, &a);
}

static int del_prog_ignore(const char *comm)
{
    int rc = scth_del_prog(comm);
    return (rc == -ENOENT) ? 0 : rc;
}
static int del_uid_ignore(__u32 euid)
{
    int rc = scth_del_uid(euid);
    return (rc == -ENOENT) ? 0 : rc;
}
static int del_sys_ignore(__u32 nr)
{
    int rc = scth_del_sys(nr);
    return (rc == -ENOENT) ? 0 : rc;
}

struct safe_sys_desc {
    long nr;
    const char *name;
    const char *desc;
};

static const struct safe_sys_desc g_safe_syscalls[] = {
    { __NR_read,    "read",    "read(-1, buf, 16) -> safe EBADF path" },
    { __NR_write,   "write",   "write(-1, \"\", 0) -> safe EBADF path" },
    { __NR_close,   "close",   "close(-1) -> safe EBADF path" },
    { __NR_getpid,  "getpid",  "getpid()" },
    { __NR_kill,    "kill",    "kill(getpid(), 0)" },
    { __NR_uname,   "uname",   "uname(&uts)" },
    { __NR_semget,  "semget",  "semget(IPC_PRIVATE,1,0600|IPC_CREAT) + IPC_RMID" },
    { __NR_getuid,  "getuid",  "getuid()" },
    { __NR_getgid,  "getgid",  "getgid()" },
    { __NR_geteuid, "geteuid", "geteuid()" },
    { __NR_getegid, "getegid", "getegid()" },
    { __NR_getppid, "getppid", "getppid()" },
};

static const struct safe_sys_desc *find_safe_desc(long nr)
{
    size_t i;
    for (i = 0; i < ARRAY_SIZE(g_safe_syscalls); i++) {
        if (g_safe_syscalls[i].nr == nr)
            return &g_safe_syscalls[i];
    }
    return NULL;
}

static void list_safe_syscalls(void)
{
    size_t i;
    printf("\nSafe syscall wrappers disponibili:\n");
    printf("  NR   NAME      DESCRIZIONE\n");
    printf("  ---  --------  -----------------------------------------------\n");
    for (i = 0; i < ARRAY_SIZE(g_safe_syscalls); i++) {
        printf("  %-3ld  %-8s  %s\n",
               g_safe_syscalls[i].nr,
               g_safe_syscalls[i].name,
               g_safe_syscalls[i].desc);
    }
    printf("\nNote:\n");
    printf("- La modalità safe invoca solo syscall note con argomenti innocui.\n");
    printf("- Se vuoi scegliere una syscall non presente sopra, puoi usare raw mode.\n");
    printf("- Raw mode serve solo a vedere il passaggio nel wrapper, non garantisce semantica utile.\n\n");
}

static void show_help(const char *prog)
{
    printf("Uso: %s [opzioni]\n\n", prog);
    printf("Questo tool puo':\n");
    printf("1) configurare il modulo scthrottle in automatico;\n");
    printf("2) registrare la syscall scelta e il matcher (program-name o uid);\n");
    printf("3) generare carico concorrente per mostrare il throttling dal vivo.\n\n");
    printf("Opzioni principali:\n");
    printf("  --interactive              modalita' guidata da terminale (default se nessun argomento)\n");
    printf("  --nr N                     numero syscall da generare\n");
    printf("  --threads N                numero thread concorrenti (default 12)\n");
    printf("  --calls N                  chiamate per thread (default 1)\n");
    printf("  --max N                    max syscall per epoca (default 3)\n");
    printf("  --policy fifo|wake         policy throttling (default fifo)\n");
    printf("  --mode safe|raw            safe wrappers o raw syscall(nr,0,0,0,0,0,0) (default safe)\n");
    printf("  --match prog|uid           match su program-name o effective uid (default prog)\n");
    printf("  --auto yes|no              configura automaticamente il modulo (default yes)\n");
    printf("  --cleanup yes|no           pulizia finale (default no)\n");
    printf("  --delay-us N               pausa tra chiamate nello stesso thread (default 0)\n");
    printf("  --list-safe                mostra tutte le syscall safe supportate\n");
    printf("  --help                     mostra questo aiuto\n\n");

    printf("Esempi:\n");
    printf("  sudo ./%s --interactive\n", prog);
    printf("  sudo ./%s --nr 63 --threads 20 --calls 1 --max 3 --policy fifo --mode safe --match prog\n", prog);
    printf("  sudo ./%s --nr 62 --threads 30 --calls 5 --max 4 --policy wake --mode safe --match prog\n", prog);
    printf("  sudo ./%s --nr 64 --threads 12 --calls 2 --max 2 --policy fifo --mode safe --match uid\n", prog);
    printf("  sudo ./%s --nr 999 --threads 8 --calls 1 --max 2 --policy fifo --mode raw --match prog\n\n", prog);

    printf("Comandi equivalenti manuali con scthctl (se non vuoi usare auto-config):\n");
    printf("  sudo ./scthctl off\n");
    printf("  sudo ./scthctl resetstats\n");
    printf("  sudo ./scthctl addsys <nr>\n");
    printf("  sudo ./scthctl addprog sysgen      # oppure adduid <uid>\n");
    printf("  sudo ./scthctl setmax <max>\n");
    printf("  sudo ./scthctl setpolicy <0|1>     # 0=FIFO_STRICT 1=WAKE_RACE\n");
    printf("  sudo ./scthctl on\n\n");
}

static int safe_syscall_once(long nr)
{
    char buf[16] = {0};
    struct utsname u;
    union semun_local arg;
    long rc;

    switch (nr) {
    case __NR_read:
        rc = syscall(__NR_read, -1, buf, sizeof(buf));
        return (rc < 0) ? -errno : 0;
    case __NR_write:
        rc = syscall(__NR_write, -1, "", 0);
        return (rc < 0) ? -errno : 0;
    case __NR_close:
        rc = syscall(__NR_close, -1);
        return (rc < 0) ? -errno : 0;
    case __NR_getpid:
        rc = syscall(__NR_getpid);
        return (rc < 0) ? -errno : 0;
    case __NR_kill:
        rc = syscall(__NR_kill, getpid(), 0);
        return (rc < 0) ? -errno : 0;
    case __NR_uname:
        memset(&u, 0, sizeof(u));
        rc = syscall(__NR_uname, &u);
        return (rc < 0) ? -errno : 0;
    case __NR_semget: {
        int semid = (int)syscall(__NR_semget, IPC_PRIVATE, 1, IPC_CREAT | 0600);
        if (semid < 0)
            return -errno;
        arg.val = 0;
        if (semctl(semid, 0, IPC_RMID, arg) < 0)
            return -errno;
        return 0;
    }
    case __NR_getuid:
        rc = syscall(__NR_getuid);
        return (rc < 0) ? -errno : 0;
    case __NR_getgid:
        rc = syscall(__NR_getgid);
        return (rc < 0) ? -errno : 0;
    case __NR_geteuid:
        rc = syscall(__NR_geteuid);
        return (rc < 0) ? -errno : 0;
    case __NR_getegid:
        rc = syscall(__NR_getegid);
        return (rc < 0) ? -errno : 0;
    case __NR_getppid:
        rc = syscall(__NR_getppid);
        return (rc < 0) ? -errno : 0;
    default:
        return -ENOTSUP;
    }
}

static int raw_syscall_once(long nr)
{
    long rc = syscall(nr, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL);
    return (rc < 0) ? -errno : 0;
}

static void *worker_fn(void *arg_)
{
    struct worker_arg *arg = (struct worker_arg *)arg_;
    unsigned i;

    pthread_barrier_wait(arg->bar);

    for (i = 0; i < arg->opt->calls_per_thread; i++) {
        int rc;

        if (arg->opt->mode == MODE_SAFE)
            rc = safe_syscall_once(arg->opt->nr);
        else
            rc = raw_syscall_once(arg->opt->nr);

        if (rc < 0)
            arg->errors++;

        if (arg->opt->delay_us_between_calls)
            usleep(arg->opt->delay_us_between_calls);
    }

    return NULL;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int parse_yes_no(const char *s, bool *out)
{
    if (!strcasecmp(s, "yes") || !strcasecmp(s, "y") || !strcmp(s, "1") || !strcasecmp(s, "true")) {
        *out = true;
        return 0;
    }
    if (!strcasecmp(s, "no") || !strcasecmp(s, "n") || !strcmp(s, "0") || !strcasecmp(s, "false")) {
        *out = false;
        return 0;
    }
    return -EINVAL;
}

static int parse_policy(const char *s, __u32 *out)
{
    if (!strcasecmp(s, "fifo") || !strcasecmp(s, "fifo_strict") || !strcmp(s, "0")) {
        *out = SCTH_POLICY_FIFO_STRICT;
        return 0;
    }
    if (!strcasecmp(s, "wake") || !strcasecmp(s, "wake_race") || !strcmp(s, "1")) {
        *out = SCTH_POLICY_WAKE_RACE;
        return 0;
    }
    return -EINVAL;
}

static int parse_mode(const char *s, enum run_mode *out)
{
    if (!strcasecmp(s, "safe")) {
        *out = MODE_SAFE;
        return 0;
    }
    if (!strcasecmp(s, "raw")) {
        *out = MODE_RAW;
        return 0;
    }
    return -EINVAL;
}

static int parse_match(const char *s, enum match_mode *out)
{
    if (!strcasecmp(s, "prog") || !strcasecmp(s, "program") || !strcasecmp(s, "program-name")) {
        *out = MATCH_PROG;
        return 0;
    }
    if (!strcasecmp(s, "uid") || !strcasecmp(s, "euid")) {
        *out = MATCH_UID;
        return 0;
    }
    return -EINVAL;
}

static int prompt_line(const char *label, char *buf, size_t bufsz)
{
    printf("%s", label);
    fflush(stdout);
    if (!fgets(buf, (int)bufsz, stdin))
        return -EIO;
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

static int prompt_uint(const char *label, unsigned *out, unsigned defval)
{
    char line[64];
    char *end;
    unsigned long v;
    int rc;

    rc = prompt_line(label, line, sizeof(line));
    if (rc < 0)
        return rc;
    if (line[0] == '\0') {
        *out = defval;
        return 0;
    }
    errno = 0;
    v = strtoul(line, &end, 10);
    if (errno || *end != '\0')
        return -EINVAL;
    *out = (unsigned)v;
    return 0;
}

static int prompt_long(const char *label, long *out, long defval)
{
    char line[64];
    char *end;
    long v;
    int rc;

    rc = prompt_line(label, line, sizeof(line));
    if (rc < 0)
        return rc;
    if (line[0] == '\0') {
        *out = defval;
        return 0;
    }
    errno = 0;
    v = strtol(line, &end, 10);
    if (errno || *end != '\0')
        return -EINVAL;
    *out = v;
    return 0;
}

static int prompt_interactive(struct options *opt, const char *prog_name)
{
    char line[64];
    int rc;

    printf("\n=== sysgen interactive mode ===\n");
    printf("Program name attuale (task->comm): %s\n", prog_name);
    printf("User euid attuale: %u\n", (unsigned)geteuid());
    list_safe_syscalls();

    rc = prompt_long("Numero syscall da generare [es. 63]: ", &opt->nr, __NR_uname);
    if (rc < 0)
        return rc;

    rc = prompt_uint("Thread concorrenti [default 12]: ", &opt->threads, 12);
    if (rc < 0)
        return rc;

    rc = prompt_uint("Chiamate per thread [default 1]: ", &opt->calls_per_thread, 1);
    if (rc < 0)
        return rc;

    rc = prompt_uint("Max syscall per epoca [default 3]: ", &opt->max_active, 3);
    if (rc < 0)
        return rc;

    rc = prompt_uint("Delay tra chiamate dello stesso thread in microsecondi [default 0]: ",
                     &opt->delay_us_between_calls, 0);
    if (rc < 0)
        return rc;

    rc = prompt_line("Policy fifo/wake [default fifo]: ", line, sizeof(line));
    if (rc < 0)
        return rc;
    if (line[0] == '\0')
        opt->policy = SCTH_POLICY_FIFO_STRICT;
    else if (parse_policy(line, &opt->policy) < 0)
        return -EINVAL;

    rc = prompt_line("Mode safe/raw [default safe]: ", line, sizeof(line));
    if (rc < 0)
        return rc;
    if (line[0] == '\0')
        opt->mode = MODE_SAFE;
    else if (parse_mode(line, &opt->mode) < 0)
        return -EINVAL;

    rc = prompt_line("Matcher prog/uid [default prog]: ", line, sizeof(line));
    if (rc < 0)
        return rc;
    if (line[0] == '\0')
        opt->match = MATCH_PROG;
    else if (parse_match(line, &opt->match) < 0)
        return -EINVAL;

    rc = prompt_line("Auto-config modulo yes/no [default yes]: ", line, sizeof(line));
    if (rc < 0)
        return rc;
    if (line[0] == '\0')
        opt->auto_config = true;
    else if (parse_yes_no(line, &opt->auto_config) < 0)
        return -EINVAL;

    rc = prompt_line("Cleanup finale yes/no [default no]: ", line, sizeof(line));
    if (rc < 0)
        return rc;
    if (line[0] == '\0')
        opt->cleanup_after = false;
    else if (parse_yes_no(line, &opt->cleanup_after) < 0)
        return -EINVAL;

    opt->reset_stats = true;
    return 0;
}

static int auto_configure(const struct options *opt, const char *prog_name)
{
    int rc;

    if (geteuid() != 0) {
        fprintf(stderr, "[ERR] auto-config richiede root/sudo.\n");
        return -EPERM;
    }

    printf("\n[INFO] Auto-configurazione modulo in corso...\n");
    printf("[INFO] Equivalente manuale:\n");
    printf("       sudo ./scthctl off\n");
    printf("       sudo ./scthctl resetstats\n");
    printf("       sudo ./scthctl addsys %ld\n", opt->nr);
    if (opt->match == MATCH_PROG)
        printf("       sudo ./scthctl addprog %s\n", prog_name);
    else
        printf("       sudo ./scthctl adduid %u\n", (unsigned)geteuid());
    printf("       sudo ./scthctl setmax %u\n", opt->max_active);
    printf("       sudo ./scthctl setpolicy %u\n", opt->policy);
    printf("       sudo ./scthctl on\n\n");

    rc = scth_off();
    if (rc < 0) return rc;
    if (opt->reset_stats) {
        rc = scth_resetstats();
        if (rc < 0) return rc;
    }

    rc = del_sys_ignore((__u32)opt->nr);
    if (rc < 0) return rc;

    if (opt->match == MATCH_PROG) {
        rc = del_prog_ignore(prog_name);
        if (rc < 0) return rc;
    } else {
        rc = del_uid_ignore((__u32)geteuid());
        if (rc < 0) return rc;
    }

    rc = scth_add_sys((__u32)opt->nr);
    if (rc < 0) return rc;

    if (opt->match == MATCH_PROG)
        rc = scth_add_prog(prog_name);
    else
        rc = scth_add_uid((__u32)geteuid());
    if (rc < 0) return rc;

    rc = scth_setmax(opt->max_active);
    if (rc < 0) return rc;

    rc = scth_setpolicy(opt->policy);
    if (rc < 0) return rc;

    rc = scth_on();
    if (rc < 0) return rc;

    return 0;
}

static void print_cfg(const struct scth_cfg *cfg)
{
    printf("[CFG] abi=%u monitor_on=%u epoch_id=%" PRIu64 "\n",
           cfg->abi_version,
           cfg->monitor_on,
           (uint64_t)cfg->epoch_id);
    printf("[CFG] max_active=%u max_pending=%u\n",
           cfg->max_active,
           cfg->max_pending);
    printf("[CFG] policy_active=%u(%s) policy_pending=%u(%s)\n",
           cfg->policy_active, policy_name(cfg->policy_active),
           cfg->policy_pending, policy_name(cfg->policy_pending));
}

static void print_stats(const struct scth_stats *s)
{
    double avg_blocked = 0.0;
    double avg_delay = 0.0;

    if (s->blocked_samples)
        avg_blocked = (double)s->blocked_sum / (double)s->blocked_samples;
    if (s->delay_num)
        avg_delay = (double)s->delay_sum_ns / (double)s->delay_num;

    printf("[STATS] abi=%u\n", s->abi_version);
    printf("[STATS] peak_delay_ns=%" PRIu64 " peak_prog=%s peak_uid=%u\n",
           (uint64_t)s->peak_delay_ns, s->peak_comm, (unsigned)s->peak_euid);
    printf("[STATS] peak_blocked_threads=%u avg_blocked_threads=%.3f (samples=%" PRIu64 ")\n",
           s->peak_blocked_threads, avg_blocked, (uint64_t)s->blocked_samples);
    printf("[STATS] delay_sum_ns=%" PRIu64 " delay_num=%" PRIu64 " avg_delay_ns=%.0f\n",
           (uint64_t)s->delay_sum_ns, (uint64_t)s->delay_num, avg_delay);
    printf("[STATS] total_tracked=%" PRIu64 " total_immediate=%" PRIu64
           " total_delayed=%" PRIu64 " total_aborted=%" PRIu64 "\n",
           (uint64_t)s->total_tracked,
           (uint64_t)s->total_immediate,
           (uint64_t)s->total_delayed,
           (uint64_t)s->total_aborted);
    printf("[STATS] peak_fifo_qlen=%u current_fifo_qlen=%u\n",
           s->peak_fifo_qlen, s->current_fifo_qlen);
    printf("[STATS] epoch_id=%" PRIu64 " last_epoch_used=%u max_active=%u\n",
           (uint64_t)s->epoch_id, s->last_epoch_used, s->max_active);
    printf("[STATS] policy_active=%u(%s) policy_pending=%u(%s)\n",
           s->policy_active, policy_name(s->policy_active),
           s->policy_pending, policy_name(s->policy_pending));
}

static int run_load(const struct options *opt)
{
    pthread_t *th = NULL;
    struct worker_arg *args = NULL;
    pthread_barrier_t bar;
    double t0, t1;
    uint64_t total_errors = 0;
    unsigned i;
    int rc = 0;

    th = calloc(opt->threads, sizeof(*th));
    args = calloc(opt->threads, sizeof(*args));
    if (!th || !args) {
        rc = -ENOMEM;
        goto out;
    }

    if (pthread_barrier_init(&bar, NULL, opt->threads + 1) != 0) {
        rc = -errno;
        goto out;
    }

    for (i = 0; i < opt->threads; i++) {
        args[i].opt = opt;
        args[i].bar = &bar;
        args[i].index = i;
        args[i].errors = 0;
        rc = pthread_create(&th[i], NULL, worker_fn, &args[i]);
        if (rc != 0) {
            rc = -rc;
            goto out_destroy_barrier;
        }
    }

    t0 = now_sec();
    pthread_barrier_wait(&bar);

    for (i = 0; i < opt->threads; i++) {
        pthread_join(th[i], NULL);
        total_errors += args[i].errors;
    }
    t1 = now_sec();

    printf("\n[RUN] done: threads=%u calls_per_thread=%u total_calls=%llu elapsed=%.3f sec errors=%llu\n",
           opt->threads,
           opt->calls_per_thread,
           (unsigned long long)opt->threads * (unsigned long long)opt->calls_per_thread,
           t1 - t0,
           (unsigned long long)total_errors);

out_destroy_barrier:
    pthread_barrier_destroy(&bar);
out:
    free(th);
    free(args);
    return rc;
}

static void default_options(struct options *opt)
{
    memset(opt, 0, sizeof(*opt));
    opt->nr = __NR_uname;
    opt->threads = 12;
    opt->calls_per_thread = 1;
    opt->max_active = 3;
    opt->policy = SCTH_POLICY_FIFO_STRICT;
    opt->mode = MODE_SAFE;
    opt->match = MATCH_PROG;
    opt->auto_config = true;
    opt->cleanup_after = false;
    opt->reset_stats = true;
    opt->delay_us_between_calls = 0;
}

static int parse_cli(int argc, char **argv, struct options *opt, bool *interactive)
{
    int i;
    default_options(opt);
    *interactive = (argc == 1);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--interactive")) {
            *interactive = true;
        } else if (!strcmp(argv[i], "--help")) {
            show_help(argv[0]);
            exit(0);
        } else if (!strcmp(argv[i], "--list-safe")) {
            list_safe_syscalls();
            exit(0);
        } else if (!strcmp(argv[i], "--nr") && i + 1 < argc) {
            opt->nr = strtol(argv[++i], NULL, 10);
            *interactive = false;
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            opt->threads = (unsigned)strtoul(argv[++i], NULL, 10);
            *interactive = false;
        } else if (!strcmp(argv[i], "--calls") && i + 1 < argc) {
            opt->calls_per_thread = (unsigned)strtoul(argv[++i], NULL, 10);
            *interactive = false;
        } else if (!strcmp(argv[i], "--max") && i + 1 < argc) {
            opt->max_active = (unsigned)strtoul(argv[++i], NULL, 10);
            *interactive = false;
        } else if (!strcmp(argv[i], "--delay-us") && i + 1 < argc) {
            opt->delay_us_between_calls = (unsigned)strtoul(argv[++i], NULL, 10);
            *interactive = false;
        } else if (!strcmp(argv[i], "--policy") && i + 1 < argc) {
            if (parse_policy(argv[++i], &opt->policy) < 0)
                return -EINVAL;
            *interactive = false;
        } else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            if (parse_mode(argv[++i], &opt->mode) < 0)
                return -EINVAL;
            *interactive = false;
        } else if (!strcmp(argv[i], "--match") && i + 1 < argc) {
            if (parse_match(argv[++i], &opt->match) < 0)
                return -EINVAL;
            *interactive = false;
        } else if (!strcmp(argv[i], "--auto") && i + 1 < argc) {
            if (parse_yes_no(argv[++i], &opt->auto_config) < 0)
                return -EINVAL;
            *interactive = false;
        } else if (!strcmp(argv[i], "--cleanup") && i + 1 < argc) {
            if (parse_yes_no(argv[++i], &opt->cleanup_after) < 0)
                return -EINVAL;
            *interactive = false;
        } else {
            fprintf(stderr, "Argomento non riconosciuto: %s\n", argv[i]);
            return -EINVAL;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct options opt;
    struct scth_cfg cfg;
    struct scth_stats st;
    const struct safe_sys_desc *desc;
    const char *prog_name;
    bool interactive;
    int rc;

    prog_name = strrchr(argv[0], '/');
    prog_name = prog_name ? prog_name + 1 : argv[0];

    rc = parse_cli(argc, argv, &opt, &interactive);
    if (rc < 0) {
        fprintf(stderr, "Errore parsing argomenti: %s\n\n", strerror(-rc));
        show_help(argv[0]);
        return 1;
    }

    if (interactive) {
        rc = prompt_interactive(&opt, prog_name);
        if (rc < 0) {
            fprintf(stderr, "Errore input interattivo: %s\n", strerror(-rc));
            return 1;
        }
    }

    desc = find_safe_desc(opt.nr);
    if (opt.mode == MODE_SAFE && !desc) {
        fprintf(stderr,
                "[ERR] syscall %ld non supportata in safe mode.\n"
                "      Usa --list-safe per vedere la whitelist oppure --mode raw.\n",
                opt.nr);
        return 1;
    }

    printf("\n=== sysgen configuration ===\n");
    printf("Program name (task->comm): %s\n", prog_name);
    printf("Current euid: %u\n", (unsigned)geteuid());
    printf("Syscall nr: %ld\n", opt.nr);
    if (desc)
        printf("Safe wrapper: %s - %s\n", desc->name, desc->desc);
    printf("Mode: %s\n", mode_name(opt.mode));
    printf("Threads: %u\n", opt.threads);
    printf("Calls per thread: %u\n", opt.calls_per_thread);
    printf("Policy: %s\n", policy_name(opt.policy));
    printf("Max per epoch: %u\n", opt.max_active);
    printf("Match mode: %s\n", match_name(opt.match));
    printf("Auto-config: %s\n", opt.auto_config ? "yes" : "no");
    printf("Cleanup after: %s\n", opt.cleanup_after ? "yes" : "no");
    printf("Delay between calls (us): %u\n", opt.delay_us_between_calls);

    if (opt.auto_config) {
        rc = auto_configure(&opt, prog_name);
        if (rc < 0) {
            fprintf(stderr, "[ERR] auto-config fallita: %s (%d)\n", strerror(-rc), -rc);
            return 1;
        }
    }

    rc = scth_get_cfg(&cfg);
    if (rc < 0) {
        fprintf(stderr, "[ERR] get_cfg fallita: %s (%d)\n", strerror(-rc), -rc);
        return 1;
    }
    print_cfg(&cfg);

    rc = run_load(&opt);
    if (rc < 0) {
        fprintf(stderr, "[ERR] run_load fallita: %s (%d)\n", strerror(-rc), -rc);
        return 1;
    }

    rc = scth_get_stats(&st);
    if (rc < 0) {
        fprintf(stderr, "[ERR] get_stats fallita: %s (%d)\n", strerror(-rc), -rc);
        return 1;
    }
    print_stats(&st);

    if (opt.cleanup_after) {
        printf("\n[INFO] Cleanup finale...\n");
        (void)scth_off();
        (void)del_sys_ignore((__u32)opt.nr);
        if (opt.match == MATCH_PROG)
            (void)del_prog_ignore(prog_name);
        else
            (void)del_uid_ignore((__u32)geteuid());
    }

    printf("\n[OK] sysgen finished.\n");
    return 0;
}
