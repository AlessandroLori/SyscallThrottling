#include <linux/slab.h>
#include <linux/string.h>
#include <linux/jhash.h>
#include <linux/bitmap.h>
#include <linux/hash.h>

#include "scth_internal.h"

static inline u32 hash_comm(const char comm[SCTH_COMM_LEN])
{
    /* hash su 16 byte fissi (comm include NUL/padding) */
    return jhash(comm, SCTH_COMM_LEN, 0);
}

void scth_cfg_init(struct scth_cfg_store *c)
{
    int i;
    for (i = 0; i < (1 << SCTH_PROG_HT_BITS); i++)
        INIT_HLIST_HEAD(&c->prog_ht[i]);
    for (i = 0; i < (1 << SCTH_UID_HT_BITS); i++)
        INIT_HLIST_HEAD(&c->uid_ht[i]);

    bitmap_zero(c->sys_bitmap, NR_syscalls);

    c->prog_count = 0;
    c->uid_count  = 0;
    c->sys_count  = 0;
}

void scth_cfg_destroy(struct scth_cfg_store *c)
{
    int b;
    struct scth_prog_ent *pe;
    struct hlist_node *tmp;

    for (b = 0; b < (1 << SCTH_PROG_HT_BITS); b++) {
        hlist_for_each_entry_safe(pe, tmp, &c->prog_ht[b], node) {
            hlist_del(&pe->node);
            kfree(pe);
        }
    }

    {
        struct scth_uid_ent *ue;
        for (b = 0; b < (1 << SCTH_UID_HT_BITS); b++) {
            hlist_for_each_entry_safe(ue, tmp, &c->uid_ht[b], node) {
                hlist_del(&ue->node);
                kfree(ue);
            }
        }
    }

    bitmap_zero(c->sys_bitmap, NR_syscalls);
    c->prog_count = c->uid_count = c->sys_count = 0;
}

/* -------- Programs -------- */

bool scth_cfg_has_prog(struct scth_cfg_store *c, const char comm[SCTH_COMM_LEN])
{
    u32 key = hash_comm(comm);
    u32 idx = key & ((1 << SCTH_PROG_HT_BITS) - 1);
    struct scth_prog_ent *e;

    hlist_for_each_entry(e, &c->prog_ht[idx], node) {
        if (memcmp(e->comm, comm, SCTH_COMM_LEN) == 0)
            return true;
    }
    return false;
}

int scth_cfg_add_prog(struct scth_cfg_store *c, const char comm[SCTH_COMM_LEN])
{
    u32 key = hash_comm(comm);
    u32 idx = key & ((1 << SCTH_PROG_HT_BITS) - 1);
    struct scth_prog_ent *e;

    /* già presente? */
    hlist_for_each_entry(e, &c->prog_ht[idx], node) {
        if (memcmp(e->comm, comm, SCTH_COMM_LEN) == 0)
            return -EEXIST;
    }

    e = kzalloc(sizeof(*e), GFP_KERNEL);
    if (!e)
        return -ENOMEM;

    memcpy(e->comm, comm, SCTH_COMM_LEN);
    hlist_add_head(&e->node, &c->prog_ht[idx]);
    c->prog_count++;
    return 0;
}

int scth_cfg_del_prog(struct scth_cfg_store *c, const char comm[SCTH_COMM_LEN])
{
    u32 key = hash_comm(comm);
    u32 idx = key & ((1 << SCTH_PROG_HT_BITS) - 1);
    struct scth_prog_ent *e;

    hlist_for_each_entry(e, &c->prog_ht[idx], node) {
        if (memcmp(e->comm, comm, SCTH_COMM_LEN) == 0) {
            hlist_del(&e->node);
            kfree(e);
            c->prog_count--;
            return 0;
        }
    }
    return -ENOENT;
}

__u32 scth_cfg_prog_count(struct scth_cfg_store *c)
{
    return c->prog_count;
}

__u32 scth_cfg_fill_prog_list(struct scth_cfg_store *c, struct scth_prog_arg *out, __u32 cap)
{
    __u32 n = 0;
    int b;
    struct scth_prog_ent *e;

    for (b = 0; b < (1 << SCTH_PROG_HT_BITS) && n < cap; b++) {
        hlist_for_each_entry(e, &c->prog_ht[b], node) {
            if (n >= cap) break;
            memcpy(out[n].comm, e->comm, SCTH_COMM_LEN);
            n++;
        }
    }
    return n;
}

/* -------- UIDs -------- */

bool scth_cfg_has_uid(struct scth_cfg_store *c, __u32 euid)
{
    u32 idx = hash_32(euid, SCTH_UID_HT_BITS);
    struct scth_uid_ent *e;

    hlist_for_each_entry(e, &c->uid_ht[idx], node) {
        if (e->euid == euid)
            return true;
    }
    return false;
}

int scth_cfg_add_uid(struct scth_cfg_store *c, __u32 euid)
{
    u32 idx = hash_32(euid, SCTH_UID_HT_BITS);
    struct scth_uid_ent *e;

    hlist_for_each_entry(e, &c->uid_ht[idx], node) {
        if (e->euid == euid)
            return -EEXIST;
    }

    e = kzalloc(sizeof(*e), GFP_KERNEL);
    if (!e)
        return -ENOMEM;

    e->euid = euid;
    hlist_add_head(&e->node, &c->uid_ht[idx]);
    c->uid_count++;
    return 0;
}

int scth_cfg_del_uid(struct scth_cfg_store *c, __u32 euid)
{
    u32 idx = hash_32(euid, SCTH_UID_HT_BITS);
    struct scth_uid_ent *e;

    hlist_for_each_entry(e, &c->uid_ht[idx], node) {
        if (e->euid == euid) {
            hlist_del(&e->node);
            kfree(e);
            c->uid_count--;
            return 0;
        }
    }
    return -ENOENT;
}

__u32 scth_cfg_uid_count(struct scth_cfg_store *c)
{
    return c->uid_count;
}

__u32 scth_cfg_fill_uid_list(struct scth_cfg_store *c, __u32 *out, __u32 cap)
{
    __u32 n = 0;
    int b;
    struct scth_uid_ent *e;

    for (b = 0; b < (1 << SCTH_UID_HT_BITS) && n < cap; b++) {
        hlist_for_each_entry(e, &c->uid_ht[b], node) {
            if (n >= cap) break;
            out[n++] = e->euid;
        }
    }
    return n;
}

/* -------- Syscalls -------- */

bool scth_cfg_has_sys(struct scth_cfg_store *c, __u32 nr)
{
    if (nr >= NR_syscalls)
        return false;
    return test_bit(nr, c->sys_bitmap);
}

int scth_cfg_add_sys(struct scth_cfg_store *c, __u32 nr)
{
    if (nr >= NR_syscalls)
        return -EINVAL;
    if (test_and_set_bit(nr, c->sys_bitmap))
        return -EEXIST;
    c->sys_count++;
    return 0;
}

int scth_cfg_del_sys(struct scth_cfg_store *c, __u32 nr)
{
    if (nr >= NR_syscalls)
        return -EINVAL;
    if (!test_and_clear_bit(nr, c->sys_bitmap))
        return -ENOENT;
    c->sys_count--;
    return 0;
}

__u32 scth_cfg_sys_count(struct scth_cfg_store *c)
{
    return c->sys_count;
}

__u32 scth_cfg_fill_sys_list(struct scth_cfg_store *c, __u32 *out, __u32 cap)
{
    __u32 n = 0;
    __u32 nr;

    for (nr = 0; nr < NR_syscalls && n < cap; nr++) {
        if (test_bit(nr, c->sys_bitmap))
            out[n++] = nr;
    }
    return n;
}