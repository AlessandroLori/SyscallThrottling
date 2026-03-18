#include "common.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static int run_as_unpriv(uid_t uid, gid_t gid)
{
    if (setgid(gid) != 0) return -errno;
    if (setuid(uid) != 0) return -errno;
    return 0;
}

int main(void)
{
    if (scth_require_root()) return 2;

    const char *suid = getenv("SUDO_UID");
    const char *sgid = getenv("SUDO_GID");
    if (!suid || !sgid) {
        fprintf(stdout, "[SKIP] t08_permissions: SUDO_UID/SUDO_GID non presenti\n");
        return 0;
    }

    uid_t uid = (uid_t)strtoul(suid, NULL, 10);
    gid_t gid = (gid_t)strtoul(sgid, NULL, 10);

    pid_t pid = fork();
    T_ASSERT(pid >= 0, "fork fallita");

    if (pid == 0) {
        int rc = run_as_unpriv(uid, gid);
        if (rc < 0) _exit(100);

        struct scth_cfg cfg;
        rc = scth_get_cfg(&cfg);
        if (rc < 0) _exit(101);

        rc = scth_setmax(7);
        if (rc != -EPERM) _exit(102);

        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    T_ASSERT(WIFEXITED(status), "child non terminato normalmente");
    T_ASSERT(WEXITSTATUS(status) == 0, "permission test failed with code=%d", WEXITSTATUS(status));
    T_PASS();
}
