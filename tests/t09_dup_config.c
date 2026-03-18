#include "common.h"
#include <unistd.h>

int main(void)
{
    if (scth_require_root()) return 2;

    T_CHECK_RC(scth_cleanup_common());

    T_INFO("t09_dup_config: duplicate add/del su prog/uid/sys");

    T_CHECK_RC(scth_add_prog("uname"));
    T_ASSERT(scth_add_prog("uname") == -EEXIST, "second add_prog atteso -EEXIST");
    T_CHECK_RC(scth_del_prog("uname"));
    T_ASSERT(scth_del_prog("uname") == -ENOENT, "second del_prog atteso -ENOENT");

    T_CHECK_RC(scth_add_uid((__u32)geteuid()));
    T_ASSERT(scth_add_uid((__u32)geteuid()) == -EEXIST, "second add_uid atteso -EEXIST");
    T_CHECK_RC(scth_del_uid((__u32)geteuid()));
    T_ASSERT(scth_del_uid((__u32)geteuid()) == -ENOENT, "second del_uid atteso -ENOENT");

    T_CHECK_RC(scth_add_sys(63));
    T_ASSERT(scth_add_sys(63) == -EEXIST, "second add_sys atteso -EEXIST");
    T_CHECK_RC(scth_del_sys(63));
    T_ASSERT(scth_del_sys(63) == -ENOENT, "second del_sys atteso -ENOENT");

    T_PASS();
}
