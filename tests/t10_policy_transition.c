#include "common.h"
#include <sys/wait.h>
#include <unistd.h>

static pid_t spawn_load_generator(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        pid_t workers[18];

        for (int i = 0; i < 18; i++) {
            char *const argv[] = { (char *)"uname", NULL };
            workers[i] = scth_spawn_quiet("uname", argv);
            if (workers[i] < 0)
                _exit(100);

            usleep(150000);   /* 150 ms */
        }

        for (int i = 0; i < 18; i++) {
            int st = 0;
            if (waitpid(workers[i], &st, 0) < 0)
                _exit(101);
        }

        _exit(0);
    }

    return pid;
}

int main(void)
{
    if (scth_require_root()) return 2;

    T_CHECK_RC(scth_setup_base(3, SCTH_POLICY_FIFO_STRICT));
    T_CHECK_RC(scth_cleanup_common());
    T_CHECK_RC(scth_add_sys(63));
    T_CHECK_RC(scth_add_prog("uname"));

    T_INFO("t10_policy_transition: FIFO -> WAKE_RACE a monitor acceso sotto carico");

    pid_t gen = spawn_load_generator();
    T_ASSERT(gen > 0, "spawn load generator fallita");

    usleep(900000);

    struct scth_cfg cfg1, cfg2, cfg3;
    struct scth_stats st;

    T_CHECK_RC(scth_get_cfg(&cfg1));
    T_INFO("cfg prima del cambio policy:");
    scth_print_cfg(&cfg1);

    T_CHECK_RC(scth_setpolicy(SCTH_POLICY_WAKE_RACE));

    T_CHECK_RC(scth_get_cfg(&cfg2));
    T_INFO("cfg subito dopo setpolicy(WAKE_RACE):");
    scth_print_cfg(&cfg2);

    T_ASSERT(cfg2.policy_active == SCTH_POLICY_FIFO_STRICT,
             "policy_active dovrebbe restare FIFO prima della nuova epoca");
    T_ASSERT(cfg2.policy_pending == SCTH_POLICY_WAKE_RACE,
             "policy_pending dovrebbe essere WAKE_RACE subito");

    usleep(1200000);

    T_CHECK_RC(scth_get_cfg(&cfg3));
    T_INFO("cfg dopo una nuova epoca:");
    scth_print_cfg(&cfg3);

    T_ASSERT(cfg3.policy_active == SCTH_POLICY_WAKE_RACE,
             "policy_active dovrebbe diventare WAKE_RACE dopo la nuova epoca");
    T_ASSERT(cfg3.policy_pending == SCTH_POLICY_WAKE_RACE,
             "policy_pending dovrebbe restare WAKE_RACE");

    T_INFO("attendo fine generatore...");
    int wst = 0;
    T_ASSERT(waitpid(gen, &wst, 0) > 0, "waitpid generator fallita");
    T_ASSERT(WIFEXITED(wst) && WEXITSTATUS(wst) == 0, "generator terminato male");

    T_CHECK_RC(scth_get_stats(&st));
    scth_print_stats(&st);

    T_ASSERT(st.total_tracked > 0, "tracked atteso > 0");
    T_ASSERT(st.policy_active == SCTH_POLICY_WAKE_RACE,
             "stats.policy_active attesa WAKE_RACE");

    T_PASS();
}