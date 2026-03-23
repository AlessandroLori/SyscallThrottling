#include "common.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Verifica unload del modulo mentre ci sono thread ancora dentro il wrapper
 * e in attesa nel meccanismo di throttling.
 *
 * Atteso:
 * - rmmod scthrottle termina con successo
 * - i worker terminano tutti
 * - il modulo sparisce da /sys/module/scthrottle
 * - il device /dev/scthrottle sparisce
 */

static int wait_pid_timeout(pid_t pid, int *status, int timeout_ms)
{
    int elapsed = 0;

    for (;;) {
        pid_t rc = waitpid(pid, status, WNOHANG);
        if (rc == pid)
            return 0;
        if (rc < 0)
            return -errno;

        if (elapsed >= timeout_ms)
            return -ETIMEDOUT;

        usleep(10000); /* 10 ms */
        elapsed += 10;
    }
}

static int wait_all_timeout(pid_t *pids, size_t n, int timeout_ms_each)
{
    for (size_t i = 0; i < n; i++) {
        int status = 0;
        int rc = wait_pid_timeout(pids[i], &status, timeout_ms_each);
        if (rc < 0)
            return rc;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return -ECHILD;
    }
    return 0;
}

int main(void)
{
    if (scth_require_root()) return 2;

    T_CHECK_RC(scth_setup_base(1, SCTH_POLICY_FIFO_STRICT));
    T_CHECK_RC(scth_cleanup_common());
    T_CHECK_RC(scth_add_sys(63));
    T_CHECK_RC(scth_add_prog("uname"));

    pid_t pids[12];

    T_INFO("t14_unload_during_wait: 12 uname, max=1, poi rmmod mentre alcuni sono in wait");
    T_CHECK_RC(scth_spawn_many_uname(pids, 12));

    /* Lascio tempo al burst di entrare nel wrapper e accodarsi */
    usleep(200000);

    pid_t rmpid = fork();
    T_ASSERT(rmpid >= 0, "fork rmmod fallita");

    if (rmpid == 0) {
        execlp("rmmod", "rmmod", "scthrottle", (char *)NULL);
        _exit(127);
    }

    {
        int status = 0;
        int rc = wait_pid_timeout(rmpid, &status, 8000);
        T_ASSERT(rc == 0, "rmmod timeout/hang: rc=%d", rc);
        T_ASSERT(WIFEXITED(status), "rmmod non terminato normalmente");
        T_ASSERT(WEXITSTATUS(status) == 0, "rmmod exit code=%d", WEXITSTATUS(status));
    }

    T_CHECK_RC(wait_all_timeout(pids, 12, 8000));

    {
        struct stat st;

        errno = 0;
        T_ASSERT(stat("/sys/module/scthrottle", &st) != 0 && errno == ENOENT,
                 "modulo ancora presente in /sys/module/scthrottle");

        errno = 0;
        T_ASSERT(stat("/dev/scthrottle", &st) != 0 && errno == ENOENT,
                 "device /dev/scthrottle ancora presente dopo unload");
    }

    T_PASS();
}