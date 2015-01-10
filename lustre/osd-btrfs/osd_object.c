/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (C) 2014 DataDirect Networks, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/osd/osd_handler.c
 *
 * Top-level entry points into osd module
 *
 * Author: Li Xi <lixi@ddn.com>
 */

#include <linux/fs_struct.h>
#include <lustre_fid.h>
#include "osd_internal.h"
#include <btrfs/btreefs_inode.h>
#include <btrfs/object-index.h>

extern struct kmem_cache *osd_object_kmem;

static void osd_object_init0(struct osd_object *obj)
{
        LASSERT(obj->oo_inode != NULL);
        obj->oo_dt.do_body_ops = &osd_body_ops;
        obj->oo_dt.do_lu.lo_header->loh_attr |=
                (LOHA_EXISTS | (obj->oo_inode->i_mode & S_IFMT));
}

/*
 * Concurrency: no concurrent access is possible that early in object
 * life-cycle.
 */
static int osd_object_init(const struct lu_env *env, struct lu_object *l,
			   const struct lu_object_conf *conf)
{
	struct osd_object	*obj = osd_obj(l);
	struct osd_device	*osd = osd_obj2dev(obj);
	int			 rc;
	ENTRY;

	LASSERT(osd_invariant(obj));

	CERROR("LIXI XXX %s: lookup "DFID"\n",
	       osd->od_svname, PFID(lu_object_fid(l)));

#ifdef LIXI
	if (fid_is_otable_it(&l->lo_header->loh_fid)) {
		obj->oo_dt.do_ops = &osd_obj_otable_it_ops;
		l->lo_header->loh_attr |= LOHA_EXISTS;
		RETURN(0);
	}
#endif /* LIXI */

	rc = osd_fid_lookup(env, obj, lu_object_fid(l));
	obj->oo_dt.do_body_ops = &osd_body_ops_new;
	if (rc == 0 && obj->oo_inode != NULL) {
		CERROR("LIXI XXX %s: lookup "DFID" succeeded\n",
		       osd->od_svname, PFID(lu_object_fid(l)));
		osd_object_init0(obj);

#ifdef LIXI
		/* LIXI TODO: move to osd_fid_lookup? */
		rc = osd_check_lma(env, obj);
		if (rc != 0) {
			iput(obj->oo_inode);
			obj->oo_inode = NULL;
			GOTO(out, rc);
		}
#endif /* LIXI */
	}
	LASSERT(osd_invariant(obj));
#ifdef LIXI
out:
#endif /* LIXI */
	RETURN(rc);
}

/*
 * Concurrency: no concurrent access is possible that late in object
 * life-cycle.
 */
static void osd_object_free(const struct lu_env *env, struct lu_object *l)
{
        struct osd_object *obj = osd_obj(l);

        LINVRNT(osd_invariant(obj));

	/* Should have been freed in osd_object_delete() */
	LASSERT(obj->oo_inode == NULL);
        dt_object_fini(&obj->oo_dt);
        OBD_FREE_PTR(obj);
}

/*
 * Called just before object is freed. Releases all resources except for
 * object itself (that is released by osd_object_free()).
 *
 * Concurrency: no concurrent access is possible that late in object
 * life-cycle.
 */
static void osd_object_delete(const struct lu_env *env, struct lu_object *l)
{
	struct osd_object *obj   = osd_obj(l);
	struct inode      *inode = obj->oo_inode;

	LINVRNT(osd_invariant(obj));

	/*
	 * If object is unlinked remove fid->ino mapping from object index.
	 */

#ifdef LIXI
	osd_index_fini(obj);
#endif /* LIXI */
	if (inode != NULL) {
#ifdef LIXI
		struct qsd_instance	*qsd = osd_obj2dev(obj)->od_quota_slave;
		qid_t			 uid = i_uid_read(inode);
		qid_t			 gid = i_gid_read(inode);
#endif /* LIXI */

		iput(inode);
		obj->oo_inode = NULL;

#ifdef LIXI
		if (qsd != NULL) {
			struct osd_thread_info	*info = osd_oti_get(env);
			struct lquota_id_info	*qi = &info->oti_qi;

			/* Release granted quota to master if necessary */
			qi->lqi_id.qid_uid = uid;
			qsd_op_adjust(env, qsd, &qi->lqi_id, USRQUOTA);

			qi->lqi_id.qid_uid = gid;
			qsd_op_adjust(env, qsd, &qi->lqi_id, GRPQUOTA);
		}
#endif /* LIXI */
	}
}

/*
 * Concurrency: ->loo_object_release() is called under site spin-lock.
 */
static void osd_object_release(const struct lu_env *env,
                               struct lu_object *l)
{
}

/*
 * Concurrency: shouldn't matter.
 */
static int osd_object_print(const struct lu_env *env, void *cookie,
                            lu_printer_t p, const struct lu_object *l)
{
        struct osd_object	*o = osd_obj(l);
        struct inode		*inode = o->oo_inode;

	return (*p)(env, cookie,
		    LUSTRE_OSD_BTRFS_NAME"-object@%p(i:%p:%llu/%llu)",
                    o, inode,
                    inode ? btreefs_ino(inode) : 0,
                    inode ? BTREEFS_I(inode)->generation : 0);
}

static inline int osd_object_invariant(const struct lu_object *l)
{
	return osd_invariant(osd_obj(l));
}

static const struct lu_object_operations osd_lu_obj_ops = {
	.loo_object_init      = osd_object_init,
	.loo_object_delete    = osd_object_delete,
	.loo_object_release   = osd_object_release,
	.loo_object_free      = osd_object_free,
	.loo_object_print     = osd_object_print,
	.loo_object_invariant = osd_object_invariant
};

/*
 * Concurrency: no external locking is necessary.
 */
static int osd_declare_object_create(const struct lu_env *env,
				     struct dt_object *dt,
				     struct lu_attr *attr,
				     struct dt_allocation_hint *hint,
				     struct dt_object_format *dof,
				     struct thandle *handle)
{
	struct osd_thandle	*oh;
	ENTRY;

	LASSERT(handle != NULL);

	oh = container_of0(handle, struct osd_thandle, ot_super);
	LASSERT(oh->ot_handle == NULL);

	osd_trans_declare_op(oh, osd_item_number[OTO_OBJECT_CREATE]);
	osd_trans_declare_op(oh, osd_item_number[OTO_INDEX_INSERT]);
	/* LIXI TODO: osd_declare_inode_qid? */

	RETURN(0);
}

static void osd_object_read_lock(const struct lu_env *env,
				 struct dt_object *dt, unsigned role)
{
	struct osd_object *obj = osd_dt_obj(dt);

	LASSERT(osd_invariant(obj));

	down_read(&obj->oo_sem);
}

static void osd_object_write_lock(const struct lu_env *env,
				  struct dt_object *dt, unsigned role)
{
	struct osd_object *obj = osd_dt_obj(dt);

	LASSERT(osd_invariant(obj));

	down_write(&obj->oo_sem);
}

static void osd_object_read_unlock(const struct lu_env *env,
				   struct dt_object *dt)
{
	struct osd_object *obj = osd_dt_obj(dt);

	LASSERT(osd_invariant(obj));
	up_read(&obj->oo_sem);
}

static void osd_object_write_unlock(const struct lu_env *env,
                                    struct dt_object *dt)
{
        struct osd_object *obj = osd_dt_obj(dt);

        LASSERT(osd_invariant(obj));
	up_write(&obj->oo_sem);
}

static int osd_object_write_locked(const struct lu_env *env,
				   struct dt_object *dt)
{
	struct osd_object *obj = osd_dt_obj(dt);
	int rc = 1;

	LASSERT(osd_invariant(obj));

	if (down_write_trylock(&obj->oo_sem)) {
		rc = 0;
		up_write(&obj->oo_sem);
	}
	return rc;
}

/**
 * Helper function for osd_object_create()
 *
 * \retval 0, on success
 */
static int __osd_oi_insert(const struct lu_env *env, struct osd_object *obj,
			   const struct lu_fid *fid, struct thandle *th)
{
	struct osd_thread_info	*info = osd_oti_get(env);
#ifdef LIXI
	struct osd_inode_id	*id = &info->oti_id;
#endif /* LIXI */
	struct osd_device	*osd = osd_obj2dev(obj);
	struct inode		*inode = obj->oo_inode;
	struct osd_thandle	*oh;
	int			 rc;
	struct dentry		*dprant;
	struct qstr		*qstr = &info->oti_qstr;

	LASSERT(obj->oo_inode != NULL);

	oh = container_of0(th, struct osd_thandle, ot_super);
	LASSERT(oh->ot_handle);

	/**
	 * Add dentry for some kind of fids,
	 * dentries of other fids will be added in dt_insert()
	 */
	if (unlikely(fid_is_last_id(fid)) ||
	    fid_is_on_ost(env, osd, fid) ||
	    fid_is_llog(fid) ||
	    fid_seq(fid) == FID_SEQ_LOCAL_FILE) {
		rc = osd_get_idx_and_name(info, osd, fid, &dprant,
					  (char *)info->oti_str,
					  sizeof(info->oti_str));
		if (rc)
			return rc;
		LASSERT(dprant);
		qstr->name = info->oti_str;
		qstr->len = strlen(info->oti_str);

		mutex_lock(&dprant->d_inode->i_mutex);
		rc = btreefs_add_entry(oh->ot_handle, dprant->d_inode,
				       inode, qstr, &obj->oo_index);
		mutex_unlock(&dprant->d_inode->i_mutex);
		if (rc)
			return rc;
	}

#ifdef LIXI
	osd_id_gen(id, obj->oo_inode->i_ino, obj->oo_inode->i_generation);
	return osd_oi_insert(info, osd, fid, id, oh->ot_handle, OI_CHECK_FLD);
#else /* LIXI */
	rc = btreefs_oi_insert(oh->ot_handle, osd->od_mnt->mnt_sb,
			       (struct btreefs_lu_fid *)fid,
			       btreefs_ino(inode),
			       BTREEFS_I(inode)->generation);
	CERROR("__osd_oi_insert LIXI XXX fid "DFID", rc = %d\n",
	       PFID(fid), rc);
	return rc;
#endif /* LIXI */
}

/**
 * Helper function for osd_object_create()
 *
 * \retval 0, on success
 */
static int __osd_object_create(struct osd_thread_info *info,
                               struct osd_object *obj, struct lu_attr *attr,
                               struct dt_allocation_hint *hint,
                               struct dt_object_format *dof,
                               struct thandle *th)
{
	int			 rc = 0;
	//const struct lu_fid	*fid = lu_object_fid(&obj->oo_dt.do_lu);
	__u32			 umask = current->fs->umask;
	struct osd_thandle	*oh  = container_of(th, struct osd_thandle,
						    ot_super);
#ifdef LIXI
	char			*name = info->oti_str;
#endif
	struct dt_object	*parent = NULL;
	struct osd_device	*osd = osd_obj2dev(obj);
	struct inode		*dir = osd_sb(osd)->s_root->d_inode;
	struct inode		*inode;
	ENTRY;

	/* we drop umask so that permissions we pass are not affected */
	current->fs->umask = 0;

	if (hint && hint->dah_parent)
		parent = hint->dah_parent;
	if (parent != NULL)
		dir = osd_dt_obj(parent)->oo_inode;

	/* Index is not file but a directory */
	if (dof->dof_type)
		attr->la_mode = S_IFDIR | ((~S_IFMT) & attr->la_mode);

	mutex_lock(&dir->i_mutex);
	inode = btreefs_create_inode(oh->ot_handle, dir, attr->la_mode,
				     (dev_t)attr->la_rdev, &obj->oo_index);
	mutex_unlock(&dir->i_mutex);
	if (IS_ERR(inode))
		GOTO(out, rc = PTR_ERR(inode));

	obj->oo_inode = inode;
	/* Do not update file c/mtime in btrfs.
	 * NB: don't need any lock because no contention at this
	 * early stage */
	inode->i_flags |= S_NOCMTIME;
#ifdef LIXI
	osd_attr_init(info, obj, attr, dof);
#endif /* LIXI */
	osd_object_init0(obj);
#ifdef LIXI
	/* bz 24037 */
	if (obj->oo_inode && (obj->oo_inode->i_state & I_NEW))
		unlock_new_inode(obj->oo_inode);
#endif /* LIXI */

out:
	/* restore previous umask value */
	current->fs->umask = umask;
	RETURN(rc);
}

static int osd_object_create(const struct lu_env *env, struct dt_object *dt,
			     struct lu_attr *attr,
			     struct dt_allocation_hint *hint,
			     struct dt_object_format *dof, struct thandle *th)
{
	const struct lu_fid	*fid	= lu_object_fid(&dt->do_lu);
	struct osd_object	*obj	= osd_dt_obj(dt);
	struct osd_thread_info	*info	= osd_oti_get(env);
	int rc;
	ENTRY;

	LINVRNT(osd_invariant(obj));
	LASSERT(!dt_object_exists(dt));
	LASSERT(!dt_object_remote(dt));
	LASSERT(osd_object_write_locked(env, dt));
	LASSERT(th != NULL);

	if (unlikely(fid_is_acct(fid)))
		/* Quota files can't be created from the kernel any more,
		 * 'tune2fs -O quota' will take care of creating them */
		RETURN(-EPERM);

	rc = __osd_object_create(info, obj, attr, hint, dof, th);
	if (rc)
		GOTO(out, rc);

	rc = __osd_oi_insert(env, obj, fid, th);
	if (rc)
		GOTO(out, rc);

out:
	LASSERT(ergo(rc == 0,
		dt_object_exists(dt) && !dt_object_remote(dt)));

	LASSERT(osd_invariant(obj));
	RETURN(rc);
}

static int osd_declare_object_ref_add(const struct lu_env *env,
                                      struct dt_object *dt,
                                      struct thandle *handle)
{
	struct osd_thandle       *oh;

        /* it's possible that object doesn't exist yet */
        LASSERT(handle != NULL);

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);

	osd_trans_declare_op(oh, osd_item_number[OTO_ATTR_SET_BASE]);

	return 0;
}

/*
 * Concurrency: @dt is write locked.
 */
static int osd_object_ref_add(const struct lu_env *env,
			      struct dt_object *dt, struct thandle *th)
{
	struct osd_object  *obj = osd_dt_obj(dt);
	struct inode       *inode = obj->oo_inode;
	struct osd_thandle *oh;
	int		    rc = 0;

	LINVRNT(osd_invariant(obj));
	LASSERT(dt_object_exists(dt));
	LASSERT(!dt_object_remote(dt));
	LASSERT(osd_object_write_locked(env, dt));
	LASSERT(th != NULL);

	oh = container_of0(th, struct osd_thandle, ot_super);
	LASSERT(oh->ot_handle != NULL);

#ifdef LIXI
	osd_trans_exec_op(env, th, OSD_OT_REF_ADD);
#endif /* LIXI */

	CDEBUG(D_INODE, DFID" increase nlink %d\n",
	       PFID(lu_object_fid(&dt->do_lu)), inode->i_nlink);
	/*
	 * The DIR_NLINK feature allows directories to exceed LDISKFS_LINK_MAX
	 * (65000) subdirectories by storing "1" in i_nlink if the link count
	 * would otherwise overflow. Directory tranversal tools understand
	 * that (st_nlink == 1) indicates that the filesystem dose not track
	 * hard links count on the directory, and will not abort subdirectory
	 * scanning early once (st_nlink - 2) subdirs have been found.
	 *
	 * This also has to properly handle the case of inodes with nlink == 0
	 * in case they are being linked into the PENDING directory
	 */
	spin_lock(&obj->oo_guard);
	if (unlikely(inode->i_nlink == 0)) {
		/* inc_nlink from 0 may cause WARN_ON */
		set_nlink(inode, 1);
	} else {
		inc_nlink(inode);
		if (!S_ISDIR(inode->i_mode))
			LASSERT(inode->i_nlink <= BTREEFS_LINK_MAX);
	}
	spin_unlock(&obj->oo_guard);

	/* No s_op->dirty_inode() is defined, so can't use ll_dirty_inode() */
	btreefs_dirty_inode(inode);
	LINVRNT(osd_invariant(obj));

	return rc;
}

static int osd_declare_object_ref_del(const struct lu_env *env,
                                      struct dt_object *dt,
                                      struct thandle *handle)
{
	struct osd_thandle       *oh;

        /* it's possible that object doesn't exist yet */
        LASSERT(handle != NULL);

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);

	osd_trans_declare_op(oh, osd_item_number[OTO_ATTR_SET_BASE]);

	return 0;
}

/*
 * Concurrency: @dt is write locked.
 */
static int osd_object_ref_del(const struct lu_env *env, struct dt_object *dt,
			      struct thandle *th)
{
	struct osd_object	*obj = osd_dt_obj(dt);
	struct inode		*inode = obj->oo_inode;
	struct osd_device	*osd = osd_dev(dt->do_lu.lo_dev);
	struct osd_thandle      *oh;

	LINVRNT(osd_invariant(obj));
	LASSERT(dt_object_exists(dt));
	LASSERT(!dt_object_remote(dt));
	LASSERT(osd_object_write_locked(env, dt));
	LASSERT(th != NULL);

	oh = container_of0(th, struct osd_thandle, ot_super);
	LASSERT(oh->ot_handle != NULL);

#ifdef LIXI
	osd_trans_exec_op(env, th, OSD_OT_REF_DEL);
#endif /* LIXI */

	spin_lock(&obj->oo_guard);
	/* That can be result of upgrade from old Lustre version and
	 * applied only to local files.  Just skip this ref_del call.
	 * ext4_unlink() only treats this as a warning, don't LASSERT here.*/
	if (inode->i_nlink == 0) {
		CDEBUG_LIMIT(fid_is_norm(lu_object_fid(&dt->do_lu)) ?
			     D_ERROR : D_INODE, "%s: nlink == 0 on "DFID
			     ", maybe an upgraded file? (LU-3915)\n",
			     osd_name(osd), PFID(lu_object_fid(&dt->do_lu)));
		spin_unlock(&obj->oo_guard);
		return 0;
	}

	CDEBUG(D_INODE, DFID" decrease nlink %d\n",
	       PFID(lu_object_fid(&dt->do_lu)), inode->i_nlink);

	if (!S_ISDIR(inode->i_mode) || inode->i_nlink > 2)
		drop_nlink(inode);
	spin_unlock(&obj->oo_guard);

	/* No s_op->dirty_inode() is defined, so can't use ll_dirty_inode() */
	btreefs_dirty_inode(inode);
	LINVRNT(osd_invariant(obj));

	return 0;
}

/**
 * Called to destroy on-disk representation of the object
 *
 * Concurrency: must be locked
 */
static int osd_declare_object_destroy(const struct lu_env *env,
				      struct dt_object *dt,
				      struct thandle *th)
{
	struct osd_object  *obj = osd_dt_obj(dt);
	struct inode       *inode = obj->oo_inode;
	struct osd_thandle *oh;
	ENTRY;

	oh = container_of0(th, struct osd_thandle, ot_super);
	LASSERT(oh->ot_handle == NULL);
	LASSERT(inode);

	osd_trans_declare_op(oh, osd_item_number[OTO_INDEX_DELETE]);
	osd_trans_declare_op(oh, osd_item_number[OTO_OBJECT_DELETE]);
	RETURN(0);
}

/*
 * Concurrency: no concurrent access is possible that late in object
 * life-cycle (for all existing callers, that is. New callers have to provide
 * their own locking.)
 */
static int osd_inode_unlinked(const struct inode *inode)
{
        return inode->i_nlink == 0;
}

static int osd_object_destroy(const struct lu_env *env,
                              struct dt_object *dt,
                              struct thandle *th)
{
        const struct lu_fid    *fid = lu_object_fid(&dt->do_lu);
        struct osd_object      *obj = osd_dt_obj(dt);
        struct inode           *inode = obj->oo_inode;
        struct osd_device      *osd = osd_obj2dev(obj);
        struct osd_thandle     *oh;
        int                     result;
        ENTRY;

        oh = container_of0(th, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle);
        LASSERT(inode);
        LASSERT(!lu_object_is_dying(dt->do_lu.lo_header));

	if (unlikely(fid_is_acct(fid)))
		RETURN(-EPERM);

	if (S_ISDIR(inode->i_mode)) {
		LASSERT(osd_inode_unlinked(inode) || inode->i_nlink == 1 ||
			inode->i_nlink == 2);
#ifdef LIXI
		/* it will check/delete the inode from remote parent,
		 * how to optimize it? unlink performance impaction XXX */
		result = osd_delete_from_remote_parent(env, osd, obj, oh);
		if (result != 0 && result != -ENOENT) {
			CERROR("%s: delete inode "DFID": rc = %d\n",
			       osd_name(osd), PFID(fid), result);
		}
#endif /* LIXI */
		spin_lock(&obj->oo_guard);
		clear_nlink(inode);
		spin_unlock(&obj->oo_guard);
		/**
		 * No s_op->dirty_inode() is defined,
		 * so can't use ll_dirty_inode()
		 */
		btreefs_dirty_inode(inode);
	}

#ifdef LIXI
	osd_trans_exec_op(env, th, OSD_OT_DESTROY);
#endif /* LIXI */

	result = btreefs_oi_delete_with_fid(oh->ot_handle,
					    osd->od_mnt->mnt_sb,
					    (struct btreefs_lu_fid *)fid);

#ifdef LIXI
        /* XXX: add to ext3 orphan list */
        /* rc = ext3_orphan_add(handle_t *handle, struct inode *inode) */

        /* not needed in the cache anymore */
        set_bit(LU_OBJECT_HEARD_BANSHEE, &dt->do_lu.lo_header->loh_flags);
#endif /* LIXI */

        RETURN(0);
}

static void osd_inode_getattr(const struct lu_env *env,
			      struct inode *inode, struct lu_attr *attr)
{
	attr->la_valid	|= LA_ATIME | LA_MTIME | LA_CTIME | LA_MODE |
			   LA_SIZE | LA_BLOCKS | LA_UID | LA_GID |
			   LA_FLAGS | LA_NLINK | LA_RDEV | LA_BLKSIZE |
			   LA_TYPE;

	attr->la_atime	 = LTIME_S(inode->i_atime);
	attr->la_mtime	 = LTIME_S(inode->i_mtime);
	attr->la_ctime	 = LTIME_S(inode->i_ctime);
	attr->la_mode	 = inode->i_mode;
	attr->la_size	 = i_size_read(inode);
	attr->la_blocks	 = inode->i_blocks;
	attr->la_uid	 = i_uid_read(inode);
	attr->la_gid	 = i_gid_read(inode);
	/* LIXI TODO: is this correct? */
	attr->la_flags	 = inode->i_flags;
	attr->la_nlink	 = inode->i_nlink;
	attr->la_rdev	 = inode->i_rdev;
	attr->la_blksize = 1 << inode->i_blkbits;
	attr->la_blkbits = inode->i_blkbits;
}

static int osd_attr_get(const struct lu_env *env,
			struct dt_object *dt,
			struct lu_attr *attr,
			struct lustre_capa *capa)
{
	struct osd_object *obj = osd_dt_obj(dt);

	LASSERT(dt_object_exists(dt));
	LASSERT(!dt_object_remote(dt));
	LINVRNT(osd_invariant(obj));

#ifdef LIXI
	if (osd_object_auth(env, dt, capa, CAPA_OPC_META_READ))
		return -EACCES;
#endif /* LIXI */

	spin_lock(&obj->oo_guard);
	osd_inode_getattr(env, obj->oo_inode, attr);
	spin_unlock(&obj->oo_guard);
	return 0;
}

static int osd_declare_attr_set(const struct lu_env *env,
                                struct dt_object *dt,
                                const struct lu_attr *attr,
                                struct thandle *handle)
{
	struct osd_thandle *oh;
	ENTRY;

	LASSERT(handle != NULL);
	oh = container_of0(handle, struct osd_thandle, ot_super);

	osd_trans_declare_op(oh, osd_item_number[OTO_ATTR_SET_BASE]);

	/* LIXI TODO: quota space for UID/GID change? */
	RETURN(0);
}

static struct timespec *osd_inode_time(const struct lu_env *env,
				       struct inode *inode, __u64 seconds)
{
	struct osd_thread_info	*oti = osd_oti_get(env);
	struct timespec		*t   = &oti->oti_time;

	t->tv_sec = seconds;
	t->tv_nsec = 0;
	*t = timespec_trunc(*t, inode->i_sb->s_time_gran);
	return t;
}

static int osd_inode_setattr(const struct lu_env *env,
			     struct inode *inode, const struct lu_attr *attr)
{
	__u64 bits = attr->la_valid;

	/* Only allow set size for regular file */
	if (!S_ISREG(inode->i_mode))
		bits &= ~(LA_SIZE | LA_BLOCKS);

	if (bits == 0)
		return 0;

        if (bits & LA_ATIME)
                inode->i_atime  = *osd_inode_time(env, inode, attr->la_atime);
        if (bits & LA_CTIME)
                inode->i_ctime  = *osd_inode_time(env, inode, attr->la_ctime);
        if (bits & LA_MTIME)
                inode->i_mtime  = *osd_inode_time(env, inode, attr->la_mtime);
        if (bits & LA_SIZE) {
                BTREEFS_I(inode)->disk_i_size = attr->la_size;
                i_size_write(inode, attr->la_size);
        }

#if 0
        /* OSD should not change "i_blocks" which is used by quota.
         * "i_blocks" should be changed by ldiskfs only. */
        if (bits & LA_BLOCKS)
                inode->i_blocks = attr->la_blocks;
#endif
	if (bits & LA_MODE)
		inode->i_mode = (inode->i_mode & S_IFMT) |
				(attr->la_mode & ~S_IFMT);
	if (bits & LA_UID)
		i_uid_write(inode, attr->la_uid);
	if (bits & LA_GID)
		i_gid_write(inode, attr->la_gid);
	if (bits & LA_NLINK)
		set_nlink(inode, attr->la_nlink);
	if (bits & LA_RDEV)
		inode->i_rdev = attr->la_rdev;

        if (bits & LA_FLAGS) {
                /* always keep S_NOCMTIME */
                inode->i_flags = ll_ext_to_inode_flags(attr->la_flags) |
                                 S_NOCMTIME;
        }
        return 0;
}

static int osd_attr_set(const struct lu_env *env,
			struct dt_object *dt,
			const struct lu_attr *attr,
			struct thandle *handle,
			struct lustre_capa *capa)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct inode      *inode;
	int rc;

	LASSERT(handle != NULL);
	LASSERT(dt_object_exists(dt));
	LASSERT(!dt_object_remote(dt));
	LASSERT(osd_invariant(obj));

#ifdef LIXI
	if (osd_object_auth(env, dt, capa, CAPA_OPC_META_WRITE))
		return -EACCES;
#endif /* LIXI */

        inode = obj->oo_inode;

#ifdef LIXI
	rc = osd_quota_transfer(inode, attr);
	if (rc)
		return rc;
#endif /* LIXI */

	spin_lock(&obj->oo_guard);
	rc = osd_inode_setattr(env, inode, attr);
	spin_unlock(&obj->oo_guard);

	/* No s_op->dirty_inode() is defined, so can't use ll_dirty_inode() */
        if (!rc)
		btreefs_dirty_inode(inode);
        return rc;
}

static const struct dt_object_operations osd_obj_ops = {
#ifdef LIXI
	.do_read_lock         = osd_object_read_lock,
	.do_write_lock        = osd_object_write_lock,
	.do_read_unlock       = osd_object_read_unlock,
	.do_write_unlock      = osd_object_write_unlock,
	.do_write_locked      = osd_object_write_locked,
	.do_attr_get          = osd_attr_get,
	.do_declare_attr_set  = osd_declare_attr_set,
	.do_attr_set          = osd_attr_set,
	.do_ah_init           = osd_ah_init,
	.do_declare_create    = osd_declare_object_create,
	.do_create            = osd_object_create,
	.do_declare_destroy   = osd_declare_object_destroy,
	.do_destroy           = osd_object_destroy,
	.do_index_try         = osd_index_try,
	.do_declare_ref_add   = osd_declare_object_ref_add,
	.do_ref_add           = osd_object_ref_add,
	.do_declare_ref_del   = osd_declare_object_ref_del,
	.do_ref_del           = osd_object_ref_del,
	.do_xattr_get         = osd_xattr_get,
	.do_declare_xattr_set = osd_declare_xattr_set,
	.do_xattr_set         = osd_xattr_set,
	.do_declare_xattr_del = osd_declare_xattr_del,
	.do_xattr_del         = osd_xattr_del,
	.do_xattr_list        = osd_xattr_list,
	.do_capa_get          = osd_capa_get,
	.do_object_sync       = osd_object_sync,
	.do_data_get          = osd_data_get,
#else /* LIXI */
	.do_read_lock         = osd_object_read_lock,
	.do_write_lock        = osd_object_write_lock,
	.do_read_unlock       = osd_object_read_unlock,
	.do_write_unlock      = osd_object_write_unlock,
	.do_write_locked      = osd_object_write_locked,
	.do_attr_get          = osd_attr_get,
	.do_declare_attr_set  = osd_declare_attr_set,
	.do_attr_set          = osd_attr_set,
	.do_ah_init           = NULL,
	.do_declare_create    = osd_declare_object_create,
	.do_create            = osd_object_create,
	.do_declare_destroy   = osd_declare_object_destroy,
	.do_destroy           = osd_object_destroy,
	.do_index_try         = osd_index_try,
	.do_declare_ref_add   = osd_declare_object_ref_add,
	.do_ref_add           = osd_object_ref_add,
	.do_declare_ref_del   = osd_declare_object_ref_del,
	.do_ref_del           = osd_object_ref_del,
	.do_xattr_get         = osd_xattr_get,
	.do_declare_xattr_set = osd_declare_xattr_set,
	.do_xattr_set         = osd_xattr_set,
	.do_declare_xattr_del = NULL,
	.do_xattr_del         = NULL,
	.do_xattr_list        = NULL,
	.do_capa_get          = NULL,
	.do_object_sync       = NULL,
	.do_data_get          = NULL,
#endif /* LIXI */
};

/*
 * Concurrency: no concurrent access is possible that early in object
 * life-cycle.
 */
struct lu_object *osd_object_alloc(const struct lu_env *env,
				   const struct lu_object_header *hdr,
				   struct lu_device *d)
{
	struct osd_object *mo;

	OBD_SLAB_ALLOC_PTR_GFP(mo, osd_object_kmem, GFP_NOFS);
	if (mo != NULL) {
		struct lu_object *l;

		l = &mo->oo_dt.do_lu;
		dt_object_init(&mo->oo_dt, NULL, d);
		mo->oo_dt.do_ops = &osd_obj_ops;
		l->lo_ops = &osd_lu_obj_ops;
		init_rwsem(&mo->oo_sem);
		spin_lock_init(&mo->oo_guard);
#ifdef LIXI
		INIT_LIST_HEAD(&mo->oo_sa_linkage);
		rwlock_init(&mo->oo_attr_lock);
#endif /* LIXI */
		return l;
	} else {
		return NULL;
	}
}
