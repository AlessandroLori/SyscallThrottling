#include "common.h"
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

// verifica funzionamento throttlando su userid non root

static int drop_to_sudo_user(uid_t *uid_out, gid_t *gid_out)
{
    const char *suid = getenv("SUDO_UID");
    const char *sgid = getenv("SUDO_GID");

    if (!suid || !sgid)
        return -ENOENT;

    uid_t uid = (uid_t)strtoul(suid, NULL, 10);
    gid_t gid = (gid_t)strtoul(sgid, NULL, 10);

    if (setgid(gid) != 0)
        return -errno;
    if (setuid(uid) != 0)
        return -errno;

    if (uid_out) *uid_out = uid;
    if (gid_out) *gid_out = gid;
    return 0;
}

int main(void)
{
    if (scth_require_root()) return 2;

    const char *suid = getenv("SUDO_UID");
    const char *sgid = getenv("SUDO_GID");
    if (!suid || !sgid) {
        fprintf(stdout, "[SKIP] t11_nonroot_uid_match: SUDO_UID/SUDO_GID non presenti\n");
        return 0;
    }

    uid_t target_uid = (uid_t)strtoul(suid, NULL, 10);

    T_CHECK_RC(scth_setup_base(3, SCTH_POLICY_FIFO_STRICT));
    T_CHECK_RC(scth_cleanup_common());
    T_CHECK_RC(scth_del_uid_ignore((__u32)target_uid));

    T_CHECK_RC(scth_add_sys(63));
    T_CHECK_RC(scth_add_uid((__u32)target_uid));

    T_INFO("t11_nonroot_uid_match: worker non-root matchano su uid=%u", (unsigned)target_uid);

    pid_t pid = fork();
    T_ASSERT(pid >= 0, "fork fallita");

    if (pid == 0) {
        uid_t uid = 0;
        gid_t gid = 0;
        int rc = drop_to_sudo_user(&uid, &gid);
        if (rc < 0)
            _exit(100);

        pid_t pids[12];
        rc = scth_spawn_many_uname(pids, 12);
        if (rc < 0)
            _exit(101);

        rc = scth_wait_all(pids, 12);
        if (rc < 0)
            _exit(102);

        _exit(0);
    }

    int st_child = 0;
    T_ASSERT(waitpid(pid, &st_child, 0) > 0, "waitpid child fallita");
    T_ASSERT(WIFEXITED(st_child) && WEXITSTATUS(st_child) == 0,
             "child terminato male: %d", WEXITSTATUS(st_child));

    struct scth_stats st;
    T_CHECK_RC(scth_get_stats(&st));
    scth_print_stats(&st);

    T_ASSERT(st.total_tracked == 12, "tracked atteso 12, got %" PRIu64, (uint64_t)st.total_tracked);
    T_ASSERT(st.total_immediate + st.total_delayed == 12,
             "immediate+delayed atteso 12");
    T_ASSERT(st.total_aborted == 0, "aborted atteso 0");
    T_ASSERT(st.peak_euid == (__u32)target_uid,
             "peak_euid atteso %u, got %u", (unsigned)target_uid, (unsigned)st.peak_euid);
    T_PASS();
}