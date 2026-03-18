#include "common.h"
#include <time.h>
#include <unistd.h>

static double monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void)
{
    if (scth_require_root()) return 2;

    T_CHECK_RC(scth_setup_base(3, SCTH_POLICY_FIFO_STRICT));
    T_CHECK_RC(scth_cleanup_common());
    T_CHECK_RC(scth_add_sys(63));
    T_CHECK_RC(scth_add_prog("uname"));

    pid_t pids[30];
    size_t spawned = 0;
    double t0 = monotonic_sec();
    int changed_5 = 0;
    int changed_4 = 0;

    T_INFO("t06_dynamic_setmax: 30 uname con gap 250ms, setmax 3 -> 5 -> 4");

    while (spawned < 30) {
        char *const argv[] = { (char *)"uname", NULL };
        pids[spawned] = scth_spawn_quiet("uname", argv);
        T_ASSERT(pids[spawned] > 0, "fork/exec uname fallita");
        spawned++;

        double dt = monotonic_sec() - t0;
        if (!changed_5 && dt >= 2.2) {
            struct scth_cfg cfg;
            T_CHECK_RC(scth_setmax(5));
            T_CHECK_RC(scth_get_cfg(&cfg));
            T_INFO("dopo setmax(5):");
            scth_print_cfg(&cfg);
            changed_5 = 1;
        }
        if (!changed_4 && dt >= 4.4) {
            struct scth_cfg cfg;
            T_CHECK_RC(scth_setmax(4));
            T_CHECK_RC(scth_get_cfg(&cfg));
            T_INFO("dopo setmax(4):");
            scth_print_cfg(&cfg);
            changed_4 = 1;
        }

        usleep(250000);
    }

    T_CHECK_RC(scth_wait_all(pids, 30));

    usleep(2200000); /* lascio applicare l'ultimo pending -> active */

    struct scth_cfg cfg;
    struct scth_stats st;
    T_CHECK_RC(scth_get_cfg(&cfg));
    T_CHECK_RC(scth_get_stats(&st));

    T_INFO("cfg finale:");
    scth_print_cfg(&cfg);
    scth_print_stats(&st);

    T_ASSERT(cfg.max_active == 4, "cfg.max_active atteso 4, got %u", cfg.max_active);
    T_ASSERT(cfg.max_pending == 4, "cfg.max_pending atteso 4, got %u", cfg.max_pending);
    T_ASSERT(cfg.policy_active == SCTH_POLICY_FIFO_STRICT, "cfg.policy_active attesa FIFO");
    T_ASSERT(st.total_tracked == 30, "tracked atteso 30");
    T_ASSERT(st.total_immediate + st.total_delayed == 30, "immediate+delayed atteso 30");
    T_ASSERT(st.total_aborted == 0, "aborted atteso 0");
    T_ASSERT(st.max_active == 4, "stats.max_active atteso 4, got %u", st.max_active);
    T_ASSERT(st.policy_active == SCTH_POLICY_FIFO_STRICT, "stats.policy_active attesa FIFO");
    T_PASS();
}
