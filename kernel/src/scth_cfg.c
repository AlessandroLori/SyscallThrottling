#include <linux/slab.h>
#include <linux/string.h>
#include <linux/jhash.h>
#include <linux/bitmap.h>
#include <linux/hash.h>
#include "scth_internal.h"

/* Database intenro, mantiene e manipola strutture rappresentanti program names, userid, syscall number selezionate */

static inline u32 hash_comm(const char comm[SCTH_COMM_LEN])
// Calcola hash del program name, serve per aggiungerlo poi nell'hashtable dei nomi
{
    /* hash su 16 byte fissi (comm include NUL/padding) */
    return jhash(comm, SCTH_COMM_LEN, 0);
}

static void scth_cfg_init_store(struct scth_cfg_store *c)
// Inizializza lo store con hash table per program names e userid, bitmap per syscall e contatori
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

struct scth_cfg_store *scth_cfg_alloc_empty(gfp_t gfp)
{
    struct scth_cfg_store *c;

    c = kzalloc(sizeof(*c), gfp);
    if (!c)
        return NULL;

    scth_cfg_init_store(c);
    return c;
}

void scth_cfg_destroy(struct scth_cfg_store *c)
// Usata in cleanup del modulo, pulisce le strutture precedenetemente nominate
{
    int b;
    struct scth_prog_ent *pe;
    struct hlist_node *tmp;

    if (!c)
        return;

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

    kfree(c);
}

/* -------- Programs -------- */

bool scth_cfg_has_prog(struct scth_cfg_store *c, const char comm[SCTH_COMM_LEN])
// Controlla la presenza di un program names nelle strutture
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
// Aggiunge un nuovo program names nelle hashtable
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
// Rimuove un program names presente nelle hashtable
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
// Restituisce numero di program names registrati
{
    return c->prog_count;
}

__u32 scth_cfg_fill_prog_list(struct scth_cfg_store *c, struct scth_prog_arg *out, __u32 cap)
// Esporta la configurazione a user-space
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
// Controlla se userid è presente e registrata
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
// Aggiunge un nuovo userid alle hashtable
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
// Rimuove un userid dalle hashtable
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
// Resituisce quanti userid sono registrati
{
    return c->uid_count;
}

__u32 scth_cfg_fill_uid_list(struct scth_cfg_store *c, __u32 *out, __u32 cap)
// Esporta gli userid registrati verso user-space
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
// Contorlla presenza di syscall registrata nella bitmap
{
    if (nr >= NR_syscalls)
        return false;
    return test_bit(nr, c->sys_bitmap);
}

int scth_cfg_add_sys(struct scth_cfg_store *c, __u32 nr)
// Aggiunge nuova syscall nella bitmap
{
    if (nr >= NR_syscalls)
        return -EINVAL;
    if (test_and_set_bit(nr, c->sys_bitmap))
        return -EEXIST;
    c->sys_count++;
    return 0;
}

int scth_cfg_del_sys(struct scth_cfg_store *c, __u32 nr)
// Rimuove syscall dalla bitmap
{
    if (nr >= NR_syscalls)
        return -EINVAL;
    if (!test_and_clear_bit(nr, c->sys_bitmap))
        return -ENOENT;
    c->sys_count--;
    return 0;
}

__u32 scth_cfg_sys_count(struct scth_cfg_store *c)
// Ritorna numero di syscall nella bitmap
{
    return c->sys_count;
}

__u32 scth_cfg_fill_sys_list(struct scth_cfg_store *c, __u32 *out, __u32 cap)
// Eposrta le syscall configurate verso user-space
{
    __u32 n = 0;
    __u32 nr;

    for (nr = 0; nr < NR_syscalls && n < cap; nr++) {
        if (test_bit(nr, c->sys_bitmap))
            out[n++] = nr;
    }
    return n;
}

struct scth_cfg_store *scth_cfg_clone(const struct scth_cfg_store *src, gfp_t gfp)
{
    struct scth_cfg_store *dst;
    int b;

    dst = scth_cfg_alloc_empty(gfp);
    if (!dst)
        return NULL;

    if (!src)
        return dst;

    bitmap_copy(dst->sys_bitmap, src->sys_bitmap, NR_syscalls);
    dst->prog_count = src->prog_count;
    dst->uid_count  = src->uid_count;
    dst->sys_count  = src->sys_count;

    for (b = 0; b < (1 << SCTH_PROG_HT_BITS); b++) {
        struct scth_prog_ent *pe;

        hlist_for_each_entry(pe, &src->prog_ht[b], node) {
            struct scth_prog_ent *ne = kzalloc(sizeof(*ne), gfp);
            if (!ne) {
                scth_cfg_destroy(dst);
                return NULL;
            }

            memcpy(ne->comm, pe->comm, SCTH_COMM_LEN);
            hlist_add_head(&ne->node, &dst->prog_ht[b]);
        }
    }

    for (b = 0; b < (1 << SCTH_UID_HT_BITS); b++) {
        struct scth_uid_ent *ue;

        hlist_for_each_entry(ue, &src->uid_ht[b], node) {
            struct scth_uid_ent *ne = kzalloc(sizeof(*ne), gfp);
            if (!ne) {
                scth_cfg_destroy(dst);
                return NULL;
            }

            ne->euid = ue->euid;
            hlist_add_head(&ne->node, &dst->uid_ht[b]);
        }
    }

    return dst;
}

static void scth_cfg_rcu_free(struct rcu_head *rh)
{
    struct scth_cfg_store *c = container_of(rh, struct scth_cfg_store, rcu);
    scth_cfg_destroy(c);
}

void scth_cfg_retire(struct scth_cfg_store *c)
{
    if (c)
        call_rcu(&c->rcu, scth_cfg_rcu_free);
}