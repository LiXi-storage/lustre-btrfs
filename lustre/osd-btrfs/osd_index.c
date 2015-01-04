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

#include "osd_internal.h"
#include <lustre_fid.h>
#include <btrfs/btreefs_inode.h>

static inline
struct dentry *osd_child_dentry_by_inode(const struct lu_env *env,
                                         struct inode *inode,
                                         const char *name, const int namelen)
{
        struct osd_thread_info *info = osd_oti_get(env);
        struct dentry *child_dentry = &info->oti_child_dentry;
        struct dentry *obj_dentry = &info->oti_obj_dentry;

        obj_dentry->d_inode = inode;
        obj_dentry->d_sb = inode->i_sb;
        obj_dentry->d_name.hash = 0;

        child_dentry->d_name.hash = 0;
        child_dentry->d_parent = obj_dentry;
        child_dentry->d_name.name = name;
        child_dentry->d_name.len = namelen;
        return child_dentry;
}

struct dentry *osd_child_dentry_get(const struct lu_env *env,
                                    struct osd_object *obj,
                                    const char *name, const int namelen)
{
        return osd_child_dentry_by_inode(env, obj->oo_inode, name, namelen);
}

/**
 * Calls ->lookup() to find dentry. From dentry get inode and
 * read inode's ea to get fid.
 *
 * \retval   0, on success
 * \retval -ve, on error
 */
static int osd_dir_lookup(const struct lu_env *env, struct dt_object *dt,
			  struct dt_rec *rec, const struct dt_key *key,
			  struct lustre_capa *capa)
{
	struct osd_object	*obj = osd_dt_obj(dt);
	char			*name = (char *)key;
	int			 rc;
	struct inode		*obj_inode = obj->oo_inode;
	struct dentry		*child_dentry;
	struct inode		*child_inode;
	struct btreefs_lu_fid	 fid;
	ENTRY;

	LASSERT(S_ISDIR(obj->oo_inode->i_mode));

	if (name[0] == '.') {
		if (name[1] == 0) {
			const struct lu_fid *f = lu_object_fid(&dt->do_lu);
			memcpy(rec, f, sizeof(*f));
			RETURN(1);
		} else if (name[1] == '.' && name[2] == 0) {
#ifdef LIXI
			rc = osd_find_parent_fid(env, dt, (struct lu_fid *)rec);
			RETURN(rc == 0 ? 1 : rc);
#else /* LIXI */
			LBUG();
#endif /* LIXI */
		}
	}


	child_dentry = osd_child_dentry_get(env, obj,
					    name, strlen(name));
	child_inode = btreefs_lookup_dentry(obj_inode, child_dentry);
	if (IS_ERR(child_inode))
		RETURN(PTR_ERR(child_inode));
	else if (child_inode == NULL)
		RETURN(-ENOENT);

	/* LIXI TODO: remove OI from ino to FID */
	rc = btreefs_oi_lookup_with_ino(obj_inode->i_sb,
					btreefs_ino(child_inode),
					BTREEFS_I(child_inode)->generation,
					&fid);
	iput(child_inode);
	if (rc == 0)
		memcpy(rec, &fid, sizeof(struct lu_fid));

	CERROR("osd_dir_lookup XXX %s, parent %p, "
	       "parent inode %p, rc = %d\n",
	       name, dt, obj_inode, rc);
	RETURN(rc == 0 ? 1 : rc);
}

static int osd_declare_dir_insert(const struct lu_env *env,
				  struct dt_object *dt,
				  const struct dt_rec *rec,
				  const struct dt_key *key,
				  struct thandle *th)
{
	struct osd_thandle *oh;
	ENTRY;

	LASSERT(th != NULL);
	oh = container_of0(th, struct osd_thandle, ot_super);

	osd_trans_declare_op(oh, osd_item_number[OTO_DENTRY_ADD]);

	RETURN(0);
}

static int osd_remote_fid(const struct lu_env *env, struct osd_device *osd,
			  const struct lu_fid *fid)
{
	struct seq_server_site	*ss = osd_seq_site(osd);
	ENTRY;

	/* FID seqs not in FLDB, must be local seq */
	if (unlikely(!fid_seq_in_fldb(fid_seq(fid))))
		RETURN(0);

	/* If FLD is not being initialized yet, it only happens during the
	 * initialization, likely during mgs initialization, and we assume
	 * this is local FID. */
	if (ss == NULL || ss->ss_server_fld == NULL)
		RETURN(0);

#ifdef LIXI
	/* Only check the local FLDB here */
	if (osd_seq_exists(env, osd, fid_seq(fid)))
		RETURN(0);
#endif /* LIXI */

	RETURN(1);
}

/**
 * Find the osd object for given fid.
 *
 * \param fid need to find the osd object having this fid
 *
 * \retval osd_object on success
 * \retval        -ve on error
 */
struct osd_object *osd_object_find(const struct lu_env *env,
                                   struct dt_object *dt,
                                   const struct lu_fid *fid)
{
        struct lu_device  *ludev = dt->do_lu.lo_dev;
        struct osd_object *child = NULL;
        struct lu_object  *luch;
        struct lu_object  *lo;

	/*
	 * at this point topdev might not exist yet
	 * (i.e. MGS is preparing profiles). so we can
	 * not rely on topdev and instead lookup with
	 * our device passed as topdev. this can't work
	 * if the object isn't cached yet (as osd doesn't
	 * allocate lu_header). IOW, the object must be
	 * in the cache, otherwise lu_object_alloc() crashes
	 * -bzzz
	 */
	luch = lu_object_find_at(env, ludev->ld_site->ls_top_dev == NULL ?
				 ludev : ludev->ld_site->ls_top_dev,
				 fid, NULL);
	if (!IS_ERR(luch)) {
		if (lu_object_exists(luch)) {
			lo = lu_object_locate(luch->lo_header, ludev->ld_type);
			if (lo != NULL)
				child = osd_obj(lo);
			else
				LU_OBJECT_DEBUG(D_ERROR, env, luch,
						"lu_object can't be located"
						DFID"\n", PFID(fid));

                        if (child == NULL) {
                                lu_object_put(env, luch);
                                CERROR("Unable to get osd_object\n");
                                child = ERR_PTR(-ENOENT);
                        }
                } else {
                        LU_OBJECT_DEBUG(D_ERROR, env, luch,
                                        "lu_object does not exists "DFID"\n",
                                        PFID(fid));
			lu_object_put(env, luch);
                        child = ERR_PTR(-ENOENT);
                }
	} else {
		child = ERR_CAST(luch);
	}

	return child;
}

/**
 * Put the osd object once done with it.
 *
 * \param obj osd object that needs to be put
 */
static inline void osd_object_put(const struct lu_env *env,
                                  struct osd_object *obj)
{
        lu_object_put(env, &obj->oo_dt.do_lu);
}

/**
 *      Inserts (key, value) pair in \a directory object.
 *
 *      \param  dt      osd index object
 *      \param  key     key for index
 *      \param  rec     record reference
 *      \param  th      transaction handler
 *      \param  capa    capability descriptor
 *      \param  ignore_quota update should not affect quota
 *
 *      \retval  0  success
 *      \retval -ve failure
 */
static int osd_dir_insert(const struct lu_env *env, struct dt_object *dt,
			  const struct dt_rec *rec, const struct dt_key *key,
			  struct thandle *th, struct lustre_capa *capa,
			  int ignore_quota)
{
	struct osd_object	*obj = osd_dt_obj(dt);
	struct osd_device	*osd = osd_dev(dt->do_lu.lo_dev);
	struct dt_insert_rec	*rec1	= (struct dt_insert_rec *)rec;
	const struct lu_fid	*fid	= rec1->rec_fid;
	const char		*name = (const char *)key;
	struct osd_thread_info	*oti   = osd_oti_get(env);
	struct osd_inode_id	*id    = &oti->oti_id;
	struct inode		*child_inode = NULL;
	struct osd_object	*child = NULL;
	int			 rc;
	struct osd_thandle	*oh;
	struct osd_thread_info	*info = osd_oti_get(env);
	struct qstr		*qstr = &info->oti_qstr;
	ENTRY;

	LASSERT(osd_invariant(obj));
	LASSERT(dt_object_exists(dt));
	LASSERT(!dt_object_remote(dt));
	LASSERT(th != NULL);

#ifdef LIXI
	osd_trans_exec_op(env, th, OSD_OT_INSERT);

	if (osd_object_auth(env, dt, capa, CAPA_OPC_INDEX_INSERT))
		RETURN(-EACCES);
#endif /* LIXI */

	oh = container_of0(th, struct osd_thandle, ot_super);

	LASSERTF(fid_is_sane(fid), "fid"DFID" is insane!\n", PFID(fid));

	rc = osd_remote_fid(env, osd, fid);
	if (rc < 0) {
		CERROR("%s: Can not find object "DFID" rc %d\n",
		       osd_name(osd), PFID(fid), rc);

		RETURN(rc);
	}

	if (rc == 1) {
#ifdef LIXI
		/* Insert remote entry */
		if (strcmp(name, dotdot) == 0 && strlen(name) == 2) {
			struct osd_mdobj_map	*omm = osd->od_mdt_map;
			struct osd_thandle	*oh;

			/* If parent on remote MDT, we need put this object
			 * under AGENT */
			oh = container_of(th, typeof(*oh), ot_super);
			rc = osd_add_to_remote_parent(env, osd, obj, oh);
			if (rc != 0) {
				CERROR("%s: add "DFID" error: rc = %d\n",
				       osd_name(osd),
				       PFID(lu_object_fid(&dt->do_lu)), rc);
				RETURN(rc);
			}

			child_inode = igrab(omm->omm_remote_parent->d_inode);
		} else {
			child_inode = osd_create_local_agent_inode(env, osd,
					obj, fid, rec1->rec_type & S_IFMT, th);
			if (IS_ERR(child_inode))
				RETURN(PTR_ERR(child_inode));
		}
#else /* LIXI */
		LBUG();
#endif /* LIXI */
	} else {
		/* Insert local entry */
		child = osd_object_find(env, dt, fid);
		if (IS_ERR(child)) {
			CERROR("%s: Can not find object "DFID"%llu:%llu: rc = %d\n",
			       osd_name(osd), PFID(fid),
			       id->oii_ino, id->oii_gen,
			       (int)PTR_ERR(child));
			RETURN(PTR_ERR(child));
		}
		child_inode = igrab(child->oo_inode);
	}

        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' &&
                                                   name[2] =='\0'))) {
		LBUG();
	} else {
		qstr->name = name;
		qstr->len = strlen(name);

		mutex_lock(&obj->oo_inode->i_mutex);
		rc = btreefs_add_entry(oh->ot_handle, obj->oo_inode,
				       child_inode, qstr, &obj->oo_index);
		mutex_unlock(&obj->oo_inode->i_mutex);
		if (rc)
			return rc;
	}

	CDEBUG(D_INODE, "parent %lu insert %s:%lu rc = %d\n",
	       obj->oo_inode->i_ino, name, child_inode->i_ino, rc);

	iput(child_inode);
	if (child != NULL)
		osd_object_put(env, child);
	LASSERT(osd_invariant(obj));
	RETURN(rc);
}

static int osd_declare_dir_delete(const struct lu_env *env,
				  struct dt_object *dt,
				  const struct dt_key *key,
				  struct thandle *th)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct osd_thandle *oh;
	ENTRY;

	LASSERT(dt_object_exists(dt));
	LASSERT(osd_invariant(obj));

	LASSERT(th != NULL);
	oh = container_of0(th, struct osd_thandle, ot_super);

	osd_trans_declare_op(oh, osd_item_number[OTO_DENTRY_DELTE]);

	RETURN(0);
}

static int osd_dir_delete(const struct lu_env *env, struct dt_object *dt,
			  const struct dt_key *key, struct thandle *th,
			  struct lustre_capa *capa)
{
	struct osd_object	*obj = osd_dt_obj(dt);
	struct osd_thandle	*oh;
	char			*name = (char *)key;
	int			 rc;
	struct inode		*obj_inode = obj->oo_inode;
	struct inode		*child_inode;
	struct dentry		*child_dentry;
	ENTRY;

	LASSERT(osd_invariant(obj));
	LASSERT(dt_object_exists(dt));
	LASSERT(!dt_object_remote(dt));
	LASSERT(th != NULL);

	LASSERT(th != NULL);
	oh = container_of0(th, struct osd_thandle, ot_super);

	/*
	 * In Orion . and .. were stored in the directory (not generated upon
	 * request as now). we preserve them for backward compatibility
	 */
	if (name[0] == '.') {
		if (name[1] == 0) {
			RETURN(0);
		} else if (name[1] == '.' && name[2] == 0) {
			RETURN(0);
		}
	}

	child_dentry = osd_child_dentry_get(env, obj,
					    name, strlen(name));
	child_inode = btreefs_lookup_dentry(obj_inode, child_dentry);
	if (IS_ERR(child_inode))
		RETURN(PTR_ERR(child_inode));
	else if (child_inode == NULL)
		RETURN(-ENOENT);

	rc = btreefs_unlink_inode(oh->ot_handle, BTREEFS_I(obj_inode)->root,
				  obj_inode, child_inode, name, strlen(name));
	iput(child_inode);

	RETURN(rc);
}

struct dt_index_operations osd_dir_ops = {
#ifdef LIXI
	.dio_lookup         = osd_dir_lookup,
	.dio_declare_insert = osd_declare_dir_insert,
	.dio_insert         = osd_dir_insert,
	.dio_declare_delete = osd_declare_dir_delete,
	.dio_delete         = osd_dir_delete,
	.dio_it     = {
		.init     = osd_dir_it_init,
		.fini     = osd_index_it_fini,
		.get      = osd_dir_it_get,
		.put      = osd_dir_it_put,
		.next     = osd_dir_it_next,
		.key      = osd_dir_it_key,
		.key_size = osd_dir_it_key_size,
		.rec      = osd_dir_it_rec,
		.rec_size = osd_dir_it_rec_size,
		.store    = osd_dir_it_store,
		.load     = osd_dir_it_load
	}
#else /* LIXI */
	.dio_lookup         = osd_dir_lookup,
	.dio_declare_insert = osd_declare_dir_insert,
	.dio_insert         = osd_dir_insert,
	.dio_declare_delete = osd_declare_dir_delete,
	.dio_delete         = osd_dir_delete,
	.dio_it     = {
		.init     = NULL,
		.fini     = NULL,
		.get      = NULL,
		.put      = NULL,
		.next     = NULL,
		.key      = NULL,
		.key_size = NULL,
		.rec      = NULL,
		.rec_size = NULL,
		.store    = NULL,
		.load     = NULL
	}
#endif /* LIXI */
};

static struct dt_it *osd_index_it_init(const struct lu_env *env,
				       struct dt_object *dt,
				       __u32 unused,
				       struct lustre_capa *capa)
{
	struct osd_thread_info	*info = osd_oti_get(env);
	struct osd_it_index	*it;
	struct osd_object	*obj = osd_dt_obj(dt);
	struct lu_object	*lo = &dt->do_lu;
	ENTRY;

	LASSERT(lu_object_exists(lo));
	LASSERT(obj->oo_inode);
	LASSERT(S_ISDIR(obj->oo_inode->i_mode));
	LASSERT(info);

	if (info->oti_it_inline) {
		OBD_ALLOC_PTR(it);
		if (it == NULL)
			RETURN(ERR_PTR(-ENOMEM));
	} else {
		it = &info->oti_it_index;
		info->oti_it_inline = 1;
	}

	it->oii_obj = obj;
	lu_object_get(lo);
	RETURN((struct dt_it *)it);
}

static void osd_index_it_fini(const struct lu_env *env, struct dt_it *di)
{
	struct osd_thread_info	*info = osd_oti_get(env);
	struct osd_it_index	*it = (struct osd_it_index *)di;
	struct osd_object	*obj = it->oii_obj;
	ENTRY;

	LASSERT(it);
	LASSERT(obj);

	lu_object_put(env, &obj->oo_dt.do_lu);
	if (it != &info->oti_it_index)
		OBD_FREE_PTR(it);
	else
		info->oti_it_inline = 0;
	EXIT;
}

static void osd_index_it_put(const struct lu_env *env, struct dt_it *di)
{
	/* PBS: do nothing : ref are incremented at retrive and decreamented
	 *      next/finish. */
}

static int osd_index_it_get(const struct lu_env *env, struct dt_it *di,
			    const struct dt_key *key)
{
	ENTRY;
	RETURN(-EIO);
}

static int osd_index_it_next(const struct lu_env *env, struct dt_it *di)
{
	ENTRY;
	RETURN(-EIO);
}

static struct dt_key *osd_index_it_key(const struct lu_env *env,
				       const struct dt_it *di)
{
	ENTRY;
	RETURN(ERR_PTR(-EIO));
}

static int osd_index_it_key_size(const struct lu_env *env,
				const struct dt_it *di)
{
	ENTRY;
	RETURN(-EIO);
}

static int osd_index_it_rec(const struct lu_env *env, const struct dt_it *di,
			    struct dt_rec *rec, __u32 attr)
{
	ENTRY;
	RETURN(-EIO);
}

static __u64 osd_index_it_store(const struct lu_env *env,
				const struct dt_it *di)
{
	ENTRY;
	RETURN(-EIO);
}

static int osd_index_it_load(const struct lu_env *env, const struct dt_it *di,
			     __u64 hash)
{
	ENTRY;
	RETURN(0);
}

static struct dt_index_operations osd_index_ops = {
#ifdef LIXI
	.dio_lookup		= osd_index_lookup,
	.dio_declare_insert	= osd_declare_index_insert,
	.dio_insert		= osd_index_insert,
	.dio_declare_delete	= osd_declare_index_delete,
	.dio_delete		= osd_index_delete,
	.dio_it	= {
		.init		= osd_index_it_init,
		.fini		= osd_index_it_fini,
		.get		= osd_index_it_get,
		.put		= osd_index_it_put,
		.next		= osd_index_it_next,
		.key		= osd_index_it_key,
		.key_size	= osd_index_it_key_size,
		.rec		= osd_index_it_rec,
		.store		= osd_index_it_store,
		.load		= osd_index_it_load
	}
#else /* LIXI */
	.dio_lookup		= NULL,
	.dio_declare_insert	= NULL,
	.dio_insert		= NULL,
	.dio_declare_delete	= NULL,
	.dio_delete		= NULL,
	.dio_it	= {
		.init		= osd_index_it_init,
		.fini		= osd_index_it_fini,
		.get		= osd_index_it_get,
		.put		= osd_index_it_put,
		.next		= osd_index_it_next,
		.key		= osd_index_it_key,
		.key_size	= osd_index_it_key_size,
		.rec		= osd_index_it_rec,
		.store		= osd_index_it_store,
		.load		= osd_index_it_load
	}
#endif /* LIXI */
};

int osd_index_try(const struct lu_env *env, struct dt_object *dt,
		  const struct dt_index_features *feat)
{
	struct osd_object *obj = osd_dt_obj(dt);
	int rc = 0;
	ENTRY;

	CERROR("LIXI XXX fid "DFID"\n",
	       PFID(lu_object_fid(&dt->do_lu)));
#ifdef LIXI
	LASSERT(dt_object_exists(dt));
#endif /* LIXI */
	LINVRNT(osd_invariant(obj));

	LASSERT(feat != &dt_acct_features);
	LASSERT(feat != &dt_otable_features);

	if (likely(feat == &dt_directory_features)) {
		if (obj->oo_inode == NULL || !S_ISDIR(obj->oo_inode->i_mode))
			GOTO(out, rc = -ENOTDIR);
		dt->do_index_ops = &osd_dir_ops;
	} else if (unlikely(feat == &dt_acct_features)) {
#ifdef LIXI
		LASSERT(fid_is_acct(lu_object_fid(&dt->do_lu)));
		dt->do_index_ops = &osd_acct_index_ops;
#endif /* LIXI */
	} else if (S_ISDIR(obj->oo_inode->i_mode) &&
		   dt->do_index_ops == NULL) {
		dt->do_index_ops = &osd_index_ops;
	}

out:
	RETURN(rc);
}
