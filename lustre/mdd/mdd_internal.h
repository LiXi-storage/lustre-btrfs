/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *  mdd/mdd_internel.c
 *
 *  Copyright (C) 2006 Cluster File Systems, Inc.
 *   Author: Wang Di <wangdi@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */

#ifndef _MDD_INTERNAL_H
#define _MDD_INTERNAL_H

#include <asm/semaphore.h>

#include <linux/lustre_acl.h>
#include <lustre_eacl.h>
#include <obd.h>
#include <md_object.h>
#include <dt_object.h>
#include <linux/sched.h>
#include <linux/capability.h>
#include <linux/dynlocks.h>

enum mdd_txn_op {
        MDD_TXN_OBJECT_DESTROY_OP = 0,
        MDD_TXN_OBJECT_CREATE_OP,
        MDD_TXN_ATTR_SET_OP,
        MDD_TXN_XATTR_SET_OP,
        MDD_TXN_INDEX_INSERT_OP,
        MDD_TXN_INDEX_DELETE_OP,
        MDD_TXN_LINK_OP,
        MDD_TXN_UNLINK_OP,
        MDD_TXN_RENAME_OP,
        MDD_TXN_RENAME_TGT_OP,
        MDD_TXN_CREATE_DATA_OP,
        MDD_TXN_MKDIR_OP,
        MDD_TXN_LAST_OP
};

struct mdd_txn_op_descr {
        enum mdd_txn_op mod_op;
        unsigned int    mod_credits;
};

struct mdd_device {
        struct md_device                 mdd_md_dev;
        struct dt_device                *mdd_child;
        struct obd_device               *mdd_obd_dev;
        struct lu_fid                    mdd_root_fid;
        struct dt_device_param           mdd_dt_conf;
        struct dt_object                *mdd_orphans;
        struct dt_txn_callback           mdd_txn_cb;
        cfs_proc_dir_entry_t            *mdd_proc_entry;
        struct lprocfs_stats            *mdd_stats;
        struct mdd_txn_op_descr          mdd_tod[MDD_TXN_LAST_OP];
        unsigned long                    mdd_atime_diff;
};

enum mod_flags {
        /* The dir object has been unlinked */
        DEAD_OBJ   = 1 << 0,
        APPEND_OBJ = 1 << 1,
        IMMUTE_OBJ = 1 << 2,
        ORPHAN_OBJ = 1 << 3,
        MNLINK_OBJ = 1 << 4
};

#define LUSTRE_APPEND_FL LDISKFS_APPEND_FL
#define LUSTRE_IMMUTABLE_FL LDISKFS_IMMUTABLE_FL
#define LUSTRE_DIRSYNC_FL LDISKFS_DIRSYNC_FL

struct mdd_object {
        struct md_object  mod_obj;
        /* open count */
        __u32             mod_count;
        __u32             mod_valid;
        unsigned long     mod_flags;
        struct dynlock    mod_pdlock;
};

struct orph_key {
        /* fid of the object*/
        struct lu_fid ok_fid;
        /* type of operation: unlink, truncate */
        __u32         ok_op;
} __attribute__((packed));

struct mdd_thread_info {
        struct txn_param          mti_param;
        struct lu_fid             mti_fid;
        struct lu_attr            mti_la;
        struct md_attr            mti_ma;
        struct lu_attr            mti_la_for_fix;
        struct obd_info           mti_oi;
        struct orph_key           mti_orph_key;
        struct obd_trans_info     mti_oti;
        struct lu_buf             mti_buf;
        struct obdo               mti_oa;
        char                      mti_xattr_buf[LUSTRE_POSIX_ACL_MAX_SIZE];
        struct lu_fid             mti_fid2; /* used for be & cpu converting */
        struct lu_fid_pack        mti_pack;
        struct dt_allocation_hint mti_hint;
        struct lov_mds_md        *mti_max_lmm;
        int                       mti_max_lmm_size;
        struct llog_cookie       *mti_max_cookie;
        int                       mti_max_cookie_size;
};

struct lov_mds_md *mdd_max_lmm_get(const struct lu_env *env,
                                   struct mdd_device *mdd);

struct llog_cookie *mdd_max_cookie_get(const struct lu_env *env,
                                       struct mdd_device *mdd);

int mdd_init_obd(const struct lu_env *env, struct mdd_device *mdd,
                 struct lustre_cfg *cfg);
int mdd_fini_obd(const struct lu_env *env, struct mdd_device *mdd,
                 struct lustre_cfg *lcfg);
int mdd_xattr_set_txn(const struct lu_env *env, struct mdd_object *obj,
                      const struct lu_buf *buf, const char *name, int fl,
                      struct thandle *txn);
int mdd_lsm_sanity_check(const struct lu_env *env, struct mdd_object *obj);
int mdd_lov_set_md(const struct lu_env *env, struct mdd_object *pobj,
                   struct mdd_object *child, struct lov_mds_md *lmm,
                   int lmm_size, struct thandle *handle, int set_stripe);
int mdd_lov_create(const struct lu_env *env, struct mdd_device *mdd,
                   struct mdd_object *parent, struct mdd_object *child,
                   struct lov_mds_md **lmm, int *lmm_size,
                   const struct md_op_spec *spec, struct lu_attr *la);
void mdd_lov_objid_update(struct mdd_device *mdd, struct lov_mds_md *lmm);
void mdd_lov_create_finish(const struct lu_env *env, struct mdd_device *mdd,
                           struct lov_mds_md *lmm, int lmm_size,
                           const struct md_op_spec *spec);
int mdd_get_md(const struct lu_env *env, struct mdd_object *obj,
               void *md, int *md_size, const char *name);
int mdd_get_md_locked(const struct lu_env *env, struct mdd_object *obj,
                      void *md, int *md_size, const char *name);
int mdd_la_get(const struct lu_env *env, struct mdd_object *obj,
               struct lu_attr *la, struct lustre_capa *capa);
int mdd_attr_set_internal(const struct lu_env *env,
                          struct mdd_object *obj,
                          struct lu_attr *attr,
                          struct thandle *handle,
                          int needacl);
int mdd_attr_check_set_internal(const struct lu_env *env,
                                struct mdd_object *obj,
                                struct lu_attr *attr,
                                struct thandle *handle,
                                int needacl);
int mdd_object_kill(const struct lu_env *env, struct mdd_object *obj,
                    struct md_attr *ma);
int mdd_iattr_get(const struct lu_env *env, struct mdd_object *mdd_obj,
                  struct md_attr *ma);
int mdd_attr_get_internal_locked(const struct lu_env *env,
                                 struct mdd_object *mdd_obj,
                                 struct md_attr *ma);
int mdd_object_create_internal(const struct lu_env *env, struct mdd_object *p,
                               struct mdd_object *c, struct md_attr *ma,
                               struct thandle *handle);
int mdd_attr_check_set_internal_locked(const struct lu_env *env,
                                       struct mdd_object *obj,
                                       struct lu_attr *attr,
                                       struct thandle *handle,
                                       int needacl);
int mdd_lmm_get_locked(const struct lu_env *env, struct mdd_object *mdd_obj,
                       struct md_attr *ma);
/* mdd_lock.c */
void mdd_write_lock(const struct lu_env *env, struct mdd_object *obj);
void mdd_read_lock(const struct lu_env *env, struct mdd_object *obj);
void mdd_write_unlock(const struct lu_env *env, struct mdd_object *obj);
void mdd_read_unlock(const struct lu_env *env, struct mdd_object *obj);

void mdd_pdlock_init(struct mdd_object *obj);
unsigned long mdd_name2hash(const char *name);
struct dynlock_handle *mdd_pdo_write_lock(const struct lu_env *env,
                                          struct mdd_object *obj,
                                          const char *name);
struct dynlock_handle *mdd_pdo_read_lock(const struct lu_env *env,
                                         struct mdd_object *obj,
                                         const char *name);
void mdd_pdo_write_unlock(const struct lu_env *env, struct mdd_object *obj,
                          struct dynlock_handle *dlh);
void mdd_pdo_read_unlock(const struct lu_env *env, struct mdd_object *obj,
                         struct dynlock_handle *dlh);
/* mdd_dir.c */
void __mdd_ref_add(const struct lu_env *env, struct mdd_object *obj,
                   struct thandle *handle);
void __mdd_ref_del(const struct lu_env *env, struct mdd_object *obj,
                   struct thandle *handle, int is_dot);
int mdd_may_create(const struct lu_env *env, struct mdd_object *pobj,
                   struct mdd_object *cobj, int check_perm, int check_nlink);
int mdd_may_unlink(const struct lu_env *env, struct mdd_object *pobj,
                   const struct md_attr *ma);
int mdd_may_delete(const struct lu_env *env, struct mdd_object *pobj,
                   struct mdd_object *cobj, struct md_attr *ma,
                   int check_perm, int check_empty);
int mdd_unlink_sanity_check(const struct lu_env *env, struct mdd_object *pobj,
                            struct mdd_object *cobj, struct md_attr *ma);
int mdd_finish_unlink(const struct lu_env *env, struct mdd_object *obj,
                      struct md_attr *ma, struct thandle *th);
int mdd_object_initialize(const struct lu_env *env, const struct lu_fid *pfid,
                          struct mdd_object *child, struct md_attr *ma,
                          struct thandle *handle);
int mdd_link_sanity_check(const struct lu_env *env, struct mdd_object *tgt_obj,
                          const struct lu_name *lname, struct mdd_object *src_obj);
/* mdd_lov.c */
int mdd_unlink_log(const struct lu_env *env, struct mdd_device *mdd,
                   struct mdd_object *mdd_cobj, struct md_attr *ma);

int mdd_setattr_log(const struct lu_env *env, struct mdd_device *mdd,
                    const struct md_attr *ma,
                    struct lov_mds_md *lmm, int lmm_size,
                    struct llog_cookie *logcookies, int cookies_size);

int mdd_get_cookie_size(const struct lu_env *env, struct mdd_device *mdd,
                        struct lov_mds_md *lmm);

int mdd_lov_setattr_async(const struct lu_env *env, struct mdd_object *obj,
                          struct lov_mds_md *lmm, int lmm_size,
                          struct llog_cookie *logcookies);

struct mdd_thread_info *mdd_env_info(const struct lu_env *env);

struct lu_buf *mdd_buf_get(const struct lu_env *env, void *area, ssize_t len);
const struct lu_buf *mdd_buf_get_const(const struct lu_env *env,
                                       const void *area, ssize_t len);

int __mdd_orphan_cleanup(const struct lu_env *env, struct mdd_device *d);
int __mdd_orphan_add(const struct lu_env *, struct mdd_object *,
                     struct thandle *);
int __mdd_orphan_del(const struct lu_env *, struct mdd_object *,
                     struct thandle *);
int orph_index_init(const struct lu_env *env, struct mdd_device *mdd);
void orph_index_fini(const struct lu_env *env, struct mdd_device *mdd);
int mdd_txn_init_credits(const struct lu_env *env, struct mdd_device *mdd);

/* mdd_lproc.c */
void lprocfs_mdd_init_vars(struct lprocfs_static_vars *lvars);
int mdd_procfs_init(struct mdd_device *mdd, const char *name);
int mdd_procfs_fini(struct mdd_device *mdd);
void mdd_lprocfs_time_start(const struct lu_env *env);
void mdd_lprocfs_time_end(const struct lu_env *env,
                          struct mdd_device *mdd, int op);

int mdd_get_flags(const struct lu_env *env, struct mdd_object *obj);

extern struct md_dir_operations    mdd_dir_ops;
extern struct md_object_operations mdd_obj_ops;

/* mdd_trans.c */
void mdd_txn_param_build(const struct lu_env *env, struct mdd_device *mdd,
                         enum mdd_txn_op);
int mdd_log_txn_param_build(const struct lu_env *env, struct md_object *obj,
                            struct md_attr *ma, enum mdd_txn_op);

static inline void mdd_object_put(const struct lu_env *env,
                                  struct mdd_object *o)
{
        lu_object_put(env, &o->mod_obj.mo_lu);
}

struct thandle* mdd_trans_start(const struct lu_env *env,
                                       struct mdd_device *);

void mdd_trans_stop(const struct lu_env *env, struct mdd_device *mdd,
                    int rc, struct thandle *handle);

int mdd_txn_start_cb(const struct lu_env *env, struct txn_param *param,
                     void *cookie);

int mdd_txn_stop_cb(const struct lu_env *env, struct thandle *txn,
                    void *cookie);

int mdd_txn_commit_cb(const struct lu_env *env, struct thandle *txn,
                      void *cookie);
/* mdd_device.c */
struct lu_object *mdd_object_alloc(const struct lu_env *env,
                                   const struct lu_object_header *hdr,
                                   struct lu_device *d);

/* mdd_object.c */
int accmode(const struct lu_env *env, struct lu_attr *la, int flags);
extern struct lu_context_key mdd_thread_key;
extern struct lu_device_operations mdd_lu_ops;

struct mdd_object *mdd_object_find(const struct lu_env *env,
                                   struct mdd_device *d,
                                   const struct lu_fid *f);

/* mdd_permission.c */
#define mdd_cap_t(x) (x)

#define MDD_CAP_TO_MASK(x) (1 << (x))

#define mdd_cap_raised(c, flag) (mdd_cap_t(c) & MDD_CAP_TO_MASK(flag))

/* capable() is copied from linux kernel! */
static inline int mdd_capable(struct md_ucred *uc, int cap)
{
        if (mdd_cap_raised(uc->mu_cap, cap))
                return 1;
        return 0;
}

int mdd_acl_def_get(const struct lu_env *env, struct mdd_object *mdd_obj,
                    struct md_attr *ma);
int mdd_acl_chmod(const struct lu_env *env, struct mdd_object *o, __u32 mode,
                  struct thandle *handle);
int __mdd_acl_init(const struct lu_env *env, struct mdd_object *obj,
                   struct lu_buf *buf, __u32 *mode, struct thandle *handle);
int mdd_acl_init(const struct lu_env *env, struct mdd_object *pobj,
                 struct mdd_object *cobj, __u32 *mode, struct thandle *handle);
int __mdd_permission_internal(const struct lu_env *env, struct mdd_object *obj,
                              struct lu_attr *la, int mask, int needlock);
int mdd_permission(const struct lu_env *env,
                   struct md_object *pobj, struct md_object *cobj,
                   struct md_attr *ma, int mask);
int mdd_capa_get(const struct lu_env *env, struct md_object *obj,
                 struct lustre_capa *capa, int renewal);

static inline int lu_device_is_mdd(struct lu_device *d)
{
	return ergo(d != NULL && d->ld_ops != NULL, d->ld_ops == &mdd_lu_ops);
}

static inline struct mdd_device* lu2mdd_dev(struct lu_device *d)
{
	LASSERT(lu_device_is_mdd(d));
	return container_of0(d, struct mdd_device, mdd_md_dev.md_lu_dev);
}

static inline struct lu_device *mdd2lu_dev(struct mdd_device *d)
{
	return (&d->mdd_md_dev.md_lu_dev);
}

static inline struct mdd_object *lu2mdd_obj(struct lu_object *o)
{
	LASSERT(ergo(o != NULL, lu_device_is_mdd(o->lo_dev)));
	return container_of0(o, struct mdd_object, mod_obj.mo_lu);
}

static inline struct mdd_device* mdo2mdd(struct md_object *mdo)
{
        return lu2mdd_dev(mdo->mo_lu.lo_dev);
}

static inline struct mdd_object* md2mdd_obj(struct md_object *mdo)
{
        return container_of0(mdo, struct mdd_object, mod_obj);
}

static inline struct dt_device_operations *mdd_child_ops(struct mdd_device *d)
{
        return d->mdd_child->dd_ops;
}

static inline struct lu_object *mdd2lu_obj(struct mdd_object *obj)
{
        return &obj->mod_obj.mo_lu;
}

static inline struct dt_object* mdd_object_child(struct mdd_object *o)
{
        return container_of0(lu_object_next(mdd2lu_obj(o)),
                             struct dt_object, do_lu);
}

static inline struct obd_device *mdd2obd_dev(struct mdd_device *mdd)
{
        return mdd->mdd_obd_dev;
}

static inline struct mdd_device *mdd_obj2mdd_dev(struct mdd_object *obj)
{
        return mdo2mdd(&obj->mod_obj);
}

static inline const struct lu_fid *mdo2fid(const struct mdd_object *obj)
{
        return lu_object_fid(&obj->mod_obj.mo_lu);
}

static inline umode_t mdd_object_type(const struct mdd_object *obj)
{
        return lu_object_attr(&obj->mod_obj.mo_lu);
}

static inline int mdd_lov_mdsize(const struct lu_env *env,
                                 struct mdd_device *mdd)
{
        struct obd_device *obd = mdd2obd_dev(mdd);
        return obd->u.mds.mds_max_mdsize;
}

static inline int mdd_lov_cookiesize(const struct lu_env *env,
                                     struct mdd_device *mdd)
{
        struct obd_device *obd = mdd2obd_dev(mdd);
        return obd->u.mds.mds_max_cookiesize;
}

static inline int mdd_is_immutable(struct mdd_object *obj)
{
        return obj->mod_flags & IMMUTE_OBJ;
}

static inline int mdd_is_dead_obj(struct mdd_object *obj)
{
        return obj && obj->mod_flags & DEAD_OBJ;
}

static inline int mdd_is_append(struct mdd_object *obj)
{
        return obj->mod_flags & APPEND_OBJ;
}

static inline int mdd_is_mnlink(struct mdd_object *obj)
{
        return obj->mod_flags & MNLINK_OBJ;
}

static inline int mdd_object_exists(struct mdd_object *obj)
{
        return lu_object_exists(mdd2lu_obj(obj));
}

static inline const struct lu_fid *mdd_object_fid(struct mdd_object *obj)
{
        return lu_object_fid(mdd2lu_obj(obj));
}

static inline struct lustre_capa *mdd_object_capa(const struct lu_env *env,
                                                  const struct mdd_object *obj)
{
        struct md_capainfo *ci = md_capainfo(env);
        const struct lu_fid *fid = mdo2fid(obj);
        int i;

        /* NB: in mdt_init0 */
        if (!ci)
                return BYPASS_CAPA;
        for (i = 0; i < MD_CAPAINFO_MAX; i++)
                if (ci->mc_fid[i] && lu_fid_eq(ci->mc_fid[i], fid))
                        return ci->mc_capa[i];
        return NULL;
}

static inline void mdd_set_capainfo(const struct lu_env *env, int offset,
                                    const struct mdd_object *obj,
                                    struct lustre_capa *capa)
{
        struct md_capainfo *ci = md_capainfo(env);
        const struct lu_fid *fid = mdo2fid(obj);

        LASSERT(offset >= 0 && offset <= MD_CAPAINFO_MAX);
        /* NB: in mdt_init0 */
        if (!ci)
                return;
        ci->mc_fid[offset]  = fid;
        ci->mc_capa[offset] = capa;
}

#define MAX_ATIME_DIFF 60

enum {
        LPROC_MDD_NR
};

static inline int mdd_permission_internal(const struct lu_env *env,
                                          struct mdd_object *obj,
                                          struct lu_attr *la, int mask)
{
        return __mdd_permission_internal(env, obj, la, mask, 0);
}

static inline int mdd_permission_internal_locked(const struct lu_env *env,
                                                 struct mdd_object *obj,
                                                 struct lu_attr *la, int mask)
{
        return __mdd_permission_internal(env, obj, la, mask, 1);
}

/* mdd inline func for calling osd_dt_object ops */
static inline int mdo_attr_get(const struct lu_env *env, struct mdd_object *obj,
                               struct lu_attr *la, struct lustre_capa *capa)
{
        struct dt_object *next = mdd_object_child(obj);
        return next->do_ops->do_attr_get(env, next, la, capa);
}

static inline int mdo_attr_set(const struct lu_env *env, struct mdd_object *obj,
                               const struct lu_attr *la, struct thandle *handle,
                               struct lustre_capa *capa)
{
        struct dt_object *next = mdd_object_child(obj);
        LASSERT(mdd_object_exists(obj));
        return next->do_ops->do_attr_set(env, next, la, handle, capa);
}

static inline int mdo_xattr_get(const struct lu_env *env,struct mdd_object *obj,
                                struct lu_buf *buf, const char *name,
                                struct lustre_capa *capa)
{
        struct dt_object *next = mdd_object_child(obj);
        return next->do_ops->do_xattr_get(env, next, buf, name, capa);
}

static inline int mdo_xattr_set(const struct lu_env *env,struct mdd_object *obj,
                                const struct lu_buf *buf, const char *name,
                                int fl, struct thandle *handle, 
                                struct lustre_capa *capa)
{
        struct dt_object *next = mdd_object_child(obj);
        LASSERT(mdd_object_exists(obj));
        return next->do_ops->do_xattr_set(env, next, buf, name, fl, handle,
                                          capa);
}

static inline int mdo_xattr_del(const struct lu_env *env,struct mdd_object *obj,
                                const char *name, struct thandle *handle,
                                struct lustre_capa *capa)
{
        struct dt_object *next = mdd_object_child(obj);
        LASSERT(mdd_object_exists(obj));
        return next->do_ops->do_xattr_del(env, next, name, handle, capa);
}

static inline 
int mdo_xattr_list(const struct lu_env *env, struct mdd_object *obj,
                   struct lu_buf *buf, struct lustre_capa *capa)
{
        struct dt_object *next = mdd_object_child(obj);
        LASSERT(mdd_object_exists(obj));
        return next->do_ops->do_xattr_list(env, next, buf, capa);
}

static inline 
int mdo_index_try(const struct lu_env *env, struct mdd_object *obj,
                                 const struct dt_index_features *feat)
{
        struct dt_object *next = mdd_object_child(obj);
        return next->do_ops->do_index_try(env, next, feat);
}

static inline void mdo_ref_add(const struct lu_env *env, struct mdd_object *obj,
                               struct thandle *handle)
{
        struct dt_object *next = mdd_object_child(obj);
        LASSERT(mdd_object_exists(obj));
        return next->do_ops->do_ref_add(env, next, handle);
}

static inline void mdo_ref_del(const struct lu_env *env, struct mdd_object *obj,
                               struct thandle *handle)
{
        struct dt_object *next = mdd_object_child(obj);
        LASSERT(mdd_object_exists(obj));
        return next->do_ops->do_ref_del(env, next, handle);
}

static inline 
int mdo_create_obj(const struct lu_env *env, struct mdd_object *o, 
                   struct lu_attr *attr,
                   struct dt_allocation_hint *hint,
                   struct thandle *handle)
{
        struct dt_object *next = mdd_object_child(o);
        return next->do_ops->do_create(env, next, attr, hint, handle);
}

static inline struct obd_capa *mdo_capa_get(const struct lu_env *env,
                                            struct mdd_object *obj, 
                                            struct lustre_capa *old,
                                            __u64 opc)
{
        struct dt_object *next = mdd_object_child(obj);
        LASSERT(mdd_object_exists(obj));
        return next->do_ops->do_capa_get(env, next, old, opc);
}

#endif
