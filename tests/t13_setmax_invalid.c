#include "common.h"

// verifica valore accettabile da setmax, 0 non è accettato per scelta progettuale e torna errore

int main(void)
{
    if (scth_require_root()) return 2;

    struct scth_cfg before, after;

    T_CHECK_RC(scth_off());
    T_CHECK_RC(scth_setmax(5));
    T_CHECK_RC(scth_setpolicy(SCTH_POLICY_FIFO_STRICT));
    T_CHECK_RC(scth_on());

    T_CHECK_RC(scth_get_cfg(&before));
    T_INFO("cfg prima di setmax(0):");
    scth_print_cfg(&before);

    int rc = scth_setmax(0);
    T_ASSERT(rc == -EINVAL, "setmax(0) atteso -EINVAL, got %d", rc);

    T_CHECK_RC(scth_get_cfg(&after));
    T_INFO("cfg dopo setmax(0):");
    scth_print_cfg(&after);

    T_ASSERT(after.max_pending == before.max_pending,
             "max_pending non dovrebbe cambiare dopo setmax(0)");
    T_ASSERT(after.max_active == before.max_active,
             "max_active non dovrebbe cambiare dopo setmax(0)");
    T_PASS();
}