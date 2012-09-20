/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.  A copy is
 * included in the COPYING file that accompanied this code.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * GPL HEADER END
 */
/*
 * Copyright  2009 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2012, Intel, Inc.
 */
/*
 * lustre/lod/lod_object.c
 *
 * Author: Alex Zhuravlev <alexey.zhuravlev@intel.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <obd.h>
#include <obd_class.h>
#include <lustre_ver.h>
#include <obd_support.h>
#include <lprocfs_status.h>

#include <lustre_fid.h>
#include <lustre_param.h>
#include <lustre_fid.h>
#include <obd_lov.h>

#include "lod_internal.h"

extern cfs_mem_cache_t *lod_object_kmem;
static const struct dt_body_operations lod_body_lnk_ops;

static int lod_index_lookup(const struct lu_env *env, struct dt_object *dt,
			    struct dt_rec *rec, const struct dt_key *key,
			    struct lustre_capa *capa)
{
	struct dt_object *next = dt_object_child(dt);
	return next->do_index_ops->dio_lookup(env, next, rec, key, capa);
}

static int lod_declare_index_insert(const struct lu_env *env,
				    struct dt_object *dt,
				    const struct dt_rec *rec,
				    const struct dt_key *key,
				    struct thandle *handle)
{
	return dt_declare_insert(env, dt_object_child(dt), rec, key, handle);
}

static int lod_index_insert(const struct lu_env *env,
			    struct dt_object *dt,
			    const struct dt_rec *rec,
			    const struct dt_key *key,
			    struct thandle *th,
			    struct lustre_capa *capa,
			    int ign)
{
	return dt_insert(env, dt_object_child(dt), rec, key, th, capa, ign);
}

static int lod_declare_index_delete(const struct lu_env *env,
				    struct dt_object *dt,
				    const struct dt_key *key,
				    struct thandle *th)
{
	return dt_declare_delete(env, dt_object_child(dt), key, th);
}

static int lod_index_delete(const struct lu_env *env,
			    struct dt_object *dt,
			    const struct dt_key *key,
			    struct thandle *th,
			    struct lustre_capa *capa)
{
	return dt_delete(env, dt_object_child(dt), key, th, capa);
}

static struct dt_it *lod_it_init(const struct lu_env *env,
				 struct dt_object *dt, __u32 attr,
				 struct lustre_capa *capa)
{
	struct dt_object   *next = dt_object_child(dt);

	return next->do_index_ops->dio_it.init(env, next, attr, capa);
}

static struct dt_index_operations lod_index_ops = {
	.dio_lookup	    = lod_index_lookup,
	.dio_declare_insert = lod_declare_index_insert,
	.dio_insert	    = lod_index_insert,
	.dio_declare_delete = lod_declare_index_delete,
	.dio_delete	    = lod_index_delete,
	.dio_it     = {
		.init	    = lod_it_init,
	}
};

static void lod_object_read_lock(const struct lu_env *env,
				 struct dt_object *dt, unsigned role)
{
	dt_read_lock(env, dt_object_child(dt), role);
}

static void lod_object_write_lock(const struct lu_env *env,
				  struct dt_object *dt, unsigned role)
{
	dt_write_lock(env, dt_object_child(dt), role);
}

static void lod_object_read_unlock(const struct lu_env *env,
				   struct dt_object *dt)
{
	dt_read_unlock(env, dt_object_child(dt));
}

static void lod_object_write_unlock(const struct lu_env *env,
				    struct dt_object *dt)
{
	dt_write_unlock(env, dt_object_child(dt));
}

static int lod_object_write_locked(const struct lu_env *env,
				   struct dt_object *dt)
{
	return dt_write_locked(env, dt_object_child(dt));
}

static int lod_attr_get(const struct lu_env *env,
			struct dt_object *dt,
			struct lu_attr *attr,
			struct lustre_capa *capa)
{
	return dt_attr_get(env, dt_object_child(dt), attr, capa);
}

static int lod_declare_attr_set(const struct lu_env *env,
				struct dt_object *dt,
				const struct lu_attr *attr,
				struct thandle *handle)
{
	struct dt_object  *next = dt_object_child(dt);
	struct lod_object *lo = lod_dt_obj(dt);
	int                rc, i;
	ENTRY;

	/*
	 * declare setattr on the local object
	 */
	rc = dt_declare_attr_set(env, next, attr, handle);
	if (rc)
		RETURN(rc);

	/*
	 * load striping information, notice we don't do this when object
	 * is being initialized as we don't need this information till
	 * few specific cases like destroy, chown
	 */
	rc = lod_load_striping(env, lo);
	if (rc)
		RETURN(rc);

	/*
	 * if object is striped declare changes on the stripes
	 */
	LASSERT(lo->ldo_stripe || lo->ldo_stripenr == 0);
	for (i = 0; i < lo->ldo_stripenr; i++) {
		LASSERT(lo->ldo_stripe[i]);
		rc = dt_declare_attr_set(env, lo->ldo_stripe[i], attr, handle);
		if (rc) {
			CERROR("failed declaration: %d\n", rc);
			break;
		}
	}

	RETURN(rc);
}

static int lod_attr_set(const struct lu_env *env,
			struct dt_object *dt,
			const struct lu_attr *attr,
			struct thandle *handle,
			struct lustre_capa *capa)
{
	struct dt_object  *next = dt_object_child(dt);
	struct lod_object *lo = lod_dt_obj(dt);
	int                rc, i;
	ENTRY;

	/*
	 * apply changes to the local object
	 */
	rc = dt_attr_set(env, next, attr, handle, capa);
	if (rc)
		RETURN(rc);

	/*
	 * if object is striped, apply changes to all the stripes
	 */
	LASSERT(lo->ldo_stripe || lo->ldo_stripenr == 0);
	for (i = 0; i < lo->ldo_stripenr; i++) {
		LASSERT(lo->ldo_stripe[i]);
		rc = dt_attr_set(env, lo->ldo_stripe[i], attr, handle, capa);
		if (rc) {
			CERROR("failed declaration: %d\n", rc);
			break;
		}
	}

	RETURN(rc);
}

static int lod_xattr_get(const struct lu_env *env, struct dt_object *dt,
			 struct lu_buf *buf, const char *name,
			 struct lustre_capa *capa)
{
	return dt_xattr_get(env, dt_object_child(dt), buf, name, capa);
}

/*
 * LOV xattr is a storage for striping, and LOD owns this xattr.
 * but LOD allows others to control striping to some extent
 * - to reset strping
 * - to set new defined striping
 * - to set new semi-defined striping
 *   - number of stripes is defined
 *   - number of stripes + osts are defined
 *   - ??
 */
static int lod_declare_xattr_set(const struct lu_env *env,
				 struct dt_object *dt,
				 const struct lu_buf *buf,
				 const char *name, int fl,
				 struct thandle *th)
{
	struct dt_object *next = dt_object_child(dt);
	int		  rc;
	ENTRY;

	rc = dt_declare_xattr_set(env, next, buf, name, fl, th);

	RETURN(rc);
}

static int lod_xattr_set(const struct lu_env *env,
			 struct dt_object *dt, const struct lu_buf *buf,
			 const char *name, int fl, struct thandle *th,
			 struct lustre_capa *capa)
{
	struct dt_object *next = dt_object_child(dt);
	int		  rc;
	ENTRY;

	/*
	 * behave transparantly for all other EAs
	 */
	rc = dt_xattr_set(env, next, buf, name, fl, th, capa);

	RETURN(rc);
}

static int lod_declare_xattr_del(const struct lu_env *env,
				 struct dt_object *dt, const char *name,
				 struct thandle *th)
{
	return dt_declare_xattr_del(env, dt_object_child(dt), name, th);
}

static int lod_xattr_del(const struct lu_env *env, struct dt_object *dt,
			 const char *name, struct thandle *th,
			 struct lustre_capa *capa)
{
	return dt_xattr_del(env, dt_object_child(dt), name, th, capa);
}

static int lod_xattr_list(const struct lu_env *env,
			  struct dt_object *dt, struct lu_buf *buf,
			  struct lustre_capa *capa)
{
	return dt_xattr_list(env, dt_object_child(dt), buf, capa);
}

int lod_object_set_pool(struct lod_object *o, char *pool)
{
	int len;

	if (o->ldo_pool) {
		len = strlen(o->ldo_pool);
		OBD_FREE(o->ldo_pool, len + 1);
		o->ldo_pool = NULL;
	}
	if (pool) {
		len = strlen(pool);
		OBD_ALLOC(o->ldo_pool, len + 1);
		if (o->ldo_pool == NULL)
			return -ENOMEM;
		strcpy(o->ldo_pool, pool);
	}
	return 0;
}

static inline int lod_object_will_be_striped(int is_reg, const struct lu_fid *fid)
{
	return (is_reg && fid_seq(fid) != FID_SEQ_LOCAL_FILE);
}

static int lod_cache_parent_striping(const struct lu_env *env,
				     struct lod_object *lp)
{
	struct lov_user_md_v1	*v1 = NULL;
	struct lov_user_md_v3	*v3 = NULL;
	int			 rc;
	ENTRY;

	/* dt_ah_init() is called from MDD without parent being write locked
	 * lock it here */
	dt_write_lock(env, dt_object_child(&lp->ldo_obj), 0);
	if (lp->ldo_striping_cached)
		GOTO(unlock, rc = 0);

	rc = lod_get_lov_ea(env, lp);
	if (rc < 0)
		GOTO(unlock, rc);

	if (rc < sizeof(struct lov_user_md)) {
		/* don't lookup for non-existing or invalid striping */
		lp->ldo_def_striping_set = 0;
		lp->ldo_striping_cached = 1;
		lp->ldo_def_stripe_size = 0;
		lp->ldo_def_stripenr = 0;
		lp->ldo_def_stripe_offset = (typeof(v1->lmm_stripe_offset))(-1);
		GOTO(unlock, rc = 0);
	}

	v1 = (struct lov_user_md_v1 *)lod_env_info(env)->lti_ea_store;
	if (v1->lmm_magic == __swab32(LOV_USER_MAGIC_V1))
		lustre_swab_lov_user_md_v1(v1);
	else if (v1->lmm_magic == __swab32(LOV_USER_MAGIC_V3))
		lustre_swab_lov_user_md_v3(v3);

	if (v1->lmm_magic != LOV_MAGIC_V3 && v1->lmm_magic != LOV_MAGIC_V1)
		GOTO(unlock, rc = 0);

	if (v1->lmm_pattern != LOV_PATTERN_RAID0 && v1->lmm_pattern != 0)
		GOTO(unlock, rc = 0);

	lp->ldo_def_stripenr = v1->lmm_stripe_count;
	lp->ldo_def_stripe_size = v1->lmm_stripe_size;
	lp->ldo_def_stripe_offset = v1->lmm_stripe_offset;
	lp->ldo_striping_cached = 1;
	lp->ldo_def_striping_set = 1;

	if (v1->lmm_magic == LOV_USER_MAGIC_V3) {
		/* XXX: sanity check here */
		v3 = (struct lov_user_md_v3 *) v1;
		if (v3->lmm_pool_name[0])
			lod_object_set_pool(lp, v3->lmm_pool_name);
	}

	CDEBUG(D_OTHER, "def. striping: # %d, sz %d, off %d %s%s on "DFID"\n",
	       lp->ldo_def_stripenr, lp->ldo_def_stripe_size,
	       lp->ldo_def_stripe_offset, v3 ? "from " : "",
	       v3 ? lp->ldo_pool : "", PFID(lu_object_fid(&lp->ldo_obj.do_lu)));

	EXIT;
unlock:
	dt_write_unlock(env, dt_object_child(&lp->ldo_obj));
	return rc;
}

/**
 * used to transfer default striping data to the object being created
 */
static void lod_ah_init(const struct lu_env *env,
			struct dt_allocation_hint *ah,
			struct dt_object *parent,
			struct dt_object *child,
			cfs_umode_t child_mode)
{
	struct lod_device *d = lu2lod_dev(child->do_lu.lo_dev);
	struct dt_object  *nextp = NULL;
	struct dt_object  *nextc;
	struct lod_object *lp = NULL;
	struct lod_object *lc;
	struct lov_desc   *desc;
	ENTRY;

	LASSERT(child);

	if (likely(parent)) {
		nextp = dt_object_child(parent);
		lp = lod_dt_obj(parent);
	}

	nextc = dt_object_child(child);
	lc = lod_dt_obj(child);

	LASSERT(lc->ldo_stripenr == 0);
	LASSERT(lc->ldo_stripe == NULL);

	/*
	 * local object may want some hints
	 * in case of late striping creation, ->ah_init()
	 * can be called with local object existing
	 */
	if (!dt_object_exists(nextc))
		nextc->do_ops->do_ah_init(env, ah, nextp, nextc, child_mode);

	if (S_ISDIR(child_mode)) {
		if (lp->ldo_striping_cached == 0) {
			/* we haven't tried to get default striping for
			 * the directory yet, let's cache it in the object */
			lod_cache_parent_striping(env, lp);
		}
		/* transfer defaults to new directory */
		if (lp->ldo_striping_cached) {
			if (lp->ldo_pool)
				lod_object_set_pool(lc, lp->ldo_pool);
			lc->ldo_def_stripenr = lp->ldo_def_stripenr;
			lc->ldo_def_stripe_size = lp->ldo_def_stripe_size;
			lc->ldo_def_stripe_offset = lp->ldo_def_stripe_offset;
			lc->ldo_striping_cached = 1;
			lc->ldo_def_striping_set = 1;
			CDEBUG(D_OTHER, "inherite striping defaults\n");
		}
		return;
	}

	/*
	 * if object is going to be striped over OSTs, transfer default
	 * striping information to the child, so that we can use it
	 * during declaration and creation
	 */
	if (!lod_object_will_be_striped(S_ISREG(child_mode),
					lu_object_fid(&child->do_lu)))
		return;

	/*
	 * try from the parent
	 */
	if (likely(parent)) {
		if (lp->ldo_striping_cached == 0) {
			/* we haven't tried to get default striping for
			 * the directory yet, let's cache it in the object */
			lod_cache_parent_striping(env, lp);
		}

		lc->ldo_def_stripe_offset = (__u16) -1;

		if (lp->ldo_def_striping_set) {
			if (lp->ldo_pool)
				lod_object_set_pool(lc, lp->ldo_pool);
			lc->ldo_stripenr = lp->ldo_def_stripenr;
			lc->ldo_stripe_size = lp->ldo_def_stripe_size;
			lc->ldo_def_stripe_offset = lp->ldo_def_stripe_offset;
			CDEBUG(D_OTHER, "striping from parent: #%d, sz %d %s\n",
			       lc->ldo_stripenr, lc->ldo_stripe_size,
			       lp->ldo_pool ? lp->ldo_pool : "");
		}
	}

	/*
	 * if the parent doesn't provide with specific pattern, grab fs-wide one
	 */
	desc = &d->lod_desc;
	if (lc->ldo_stripenr == 0)
		lc->ldo_stripenr = desc->ld_default_stripe_count;
	if (lc->ldo_stripe_size == 0)
		lc->ldo_stripe_size = desc->ld_default_stripe_size;
	CDEBUG(D_OTHER, "final striping: # %d stripes, sz %d from %s\n",
	       lc->ldo_stripenr, lc->ldo_stripe_size,
	       lc->ldo_pool ? lc->ldo_pool : "");

	EXIT;
}

static int lod_declare_object_create(const struct lu_env *env,
				     struct dt_object *dt,
				     struct lu_attr *attr,
				     struct dt_allocation_hint *hint,
				     struct dt_object_format *dof,
				     struct thandle *th)
{
	struct dt_object   *next = dt_object_child(dt);
	int		    rc;
	ENTRY;

	LASSERT(dof);
	LASSERT(attr);
	LASSERT(th);
	LASSERT(!dt_object_exists(next));

	/*
	 * first of all, we declare creation of local object
	 */
	rc = dt_declare_create(env, next, attr, hint, dof, th);
	if (rc)
		GOTO(out, rc);

	if (dof->dof_type == DFT_SYM)
		dt->do_body_ops = &lod_body_lnk_ops;

out:
	RETURN(rc);
}

static int lod_object_create(const struct lu_env *env, struct dt_object *dt,
			     struct lu_attr *attr,
			     struct dt_allocation_hint *hint,
			     struct dt_object_format *dof, struct thandle *th)
{
	struct dt_object   *next = dt_object_child(dt);
	int		    rc;
	ENTRY;

	/* create local object */
	rc = dt_create(env, next, attr, hint, dof, th);

	RETURN(rc);
}

static int lod_declare_object_destroy(const struct lu_env *env,
				      struct dt_object *dt,
				      struct thandle *th)
{
	struct dt_object   *next = dt_object_child(dt);
	struct lod_object  *lo = lod_dt_obj(dt);
	int		    rc, i;
	ENTRY;

	/*
	 * we declare destroy for the local object
	 */
	rc = dt_declare_destroy(env, next, th);
	if (rc)
		RETURN(rc);

	/*
	 * load striping information, notice we don't do this when object
	 * is being initialized as we don't need this information till
	 * few specific cases like destroy, chown
	 */
	rc = lod_load_striping(env, lo);
	if (rc)
		RETURN(rc);

	/* declare destroy for all underlying objects */
	for (i = 0; i < lo->ldo_stripenr; i++) {
		LASSERT(lo->ldo_stripe[i]);
		rc = dt_declare_destroy(env, lo->ldo_stripe[i], th);

		if (rc)
			break;
	}

	RETURN(rc);
}

static int lod_object_destroy(const struct lu_env *env,
		struct dt_object *dt, struct thandle *th)
{
	struct dt_object  *next = dt_object_child(dt);
	struct lod_object *lo = lod_dt_obj(dt);
	int                rc, i;
	ENTRY;

	/* destroy local object */
	rc = dt_destroy(env, next, th);
	if (rc)
		RETURN(rc);

	/* destroy all underlying objects */
	for (i = 0; i < lo->ldo_stripenr; i++) {
		LASSERT(lo->ldo_stripe[i]);
		rc = dt_destroy(env, lo->ldo_stripe[i], th);
		if (rc)
			break;
	}

	RETURN(rc);
}

static int lod_index_try(const struct lu_env *env, struct dt_object *dt,
			 const struct dt_index_features *feat)
{
	struct dt_object *next = dt_object_child(dt);
	int		  rc;
	ENTRY;

	LASSERT(next->do_ops);
	LASSERT(next->do_ops->do_index_try);

	rc = next->do_ops->do_index_try(env, next, feat);
	if (next->do_index_ops && dt->do_index_ops == NULL) {
		dt->do_index_ops = &lod_index_ops;
		/* XXX: iterators don't accept device, so bypass LOD */
		/* will be fixed with DNE */
		if (lod_index_ops.dio_it.fini == NULL) {
			lod_index_ops.dio_it = next->do_index_ops->dio_it;
			lod_index_ops.dio_it.init = lod_it_init;
		}
	}

	RETURN(rc);
}

static int lod_declare_ref_add(const struct lu_env *env,
			       struct dt_object *dt, struct thandle *th)
{
	return dt_declare_ref_add(env, dt_object_child(dt), th);
}

static int lod_ref_add(const struct lu_env *env,
		       struct dt_object *dt, struct thandle *th)
{
	return dt_ref_add(env, dt_object_child(dt), th);
}

static int lod_declare_ref_del(const struct lu_env *env,
			       struct dt_object *dt, struct thandle *th)
{
	return dt_declare_ref_del(env, dt_object_child(dt), th);
}

static int lod_ref_del(const struct lu_env *env,
		       struct dt_object *dt, struct thandle *th)
{
	return dt_ref_del(env, dt_object_child(dt), th);
}

static struct obd_capa *lod_capa_get(const struct lu_env *env,
				     struct dt_object *dt,
				     struct lustre_capa *old, __u64 opc)
{
	return dt_capa_get(env, dt_object_child(dt), old, opc);
}

static int lod_object_sync(const struct lu_env *env, struct dt_object *dt)
{
	return dt_object_sync(env, dt_object_child(dt));
}

struct dt_object_operations lod_obj_ops = {
	.do_read_lock		= lod_object_read_lock,
	.do_write_lock		= lod_object_write_lock,
	.do_read_unlock		= lod_object_read_unlock,
	.do_write_unlock	= lod_object_write_unlock,
	.do_write_locked	= lod_object_write_locked,
	.do_attr_get		= lod_attr_get,
	.do_declare_attr_set	= lod_declare_attr_set,
	.do_attr_set		= lod_attr_set,
	.do_xattr_get		= lod_xattr_get,
	.do_declare_xattr_set	= lod_declare_xattr_set,
	.do_xattr_set		= lod_xattr_set,
	.do_declare_xattr_del	= lod_declare_xattr_del,
	.do_xattr_del		= lod_xattr_del,
	.do_xattr_list		= lod_xattr_list,
	.do_ah_init		= lod_ah_init,
	.do_declare_create	= lod_declare_object_create,
	.do_create		= lod_object_create,
	.do_declare_destroy	= lod_declare_object_destroy,
	.do_destroy		= lod_object_destroy,
	.do_index_try		= lod_index_try,
	.do_declare_ref_add	= lod_declare_ref_add,
	.do_ref_add		= lod_ref_add,
	.do_declare_ref_del	= lod_declare_ref_del,
	.do_ref_del		= lod_ref_del,
	.do_capa_get		= lod_capa_get,
	.do_object_sync		= lod_object_sync,
};

static ssize_t lod_read(const struct lu_env *env, struct dt_object *dt,
			struct lu_buf *buf, loff_t *pos,
			struct lustre_capa *capa)
{
	struct dt_object *next = dt_object_child(dt);
        return next->do_body_ops->dbo_read(env, next, buf, pos, capa);
}

static ssize_t lod_declare_write(const struct lu_env *env,
				 struct dt_object *dt,
				 const loff_t size, loff_t pos,
				 struct thandle *th)
{
	return dt_declare_record_write(env, dt_object_child(dt),
				       size, pos, th);
}

static ssize_t lod_write(const struct lu_env *env, struct dt_object *dt,
			 const struct lu_buf *buf, loff_t *pos,
			 struct thandle *th, struct lustre_capa *capa, int iq)
{
	struct dt_object *next = dt_object_child(dt);
	LASSERT(next);
	return next->do_body_ops->dbo_write(env, next, buf, pos, th, capa, iq);
}

static const struct dt_body_operations lod_body_lnk_ops = {
	.dbo_read		= lod_read,
	.dbo_declare_write	= lod_declare_write,
	.dbo_write		= lod_write
};

static int lod_object_init(const struct lu_env *env, struct lu_object *o,
			   const struct lu_object_conf *conf)
{
	struct lod_device *d = lu2lod_dev(o->lo_dev);
	struct lu_object  *below;
	struct lu_device  *under;
	ENTRY;

	/*
	 * create local object
	 */
	under = &d->lod_child->dd_lu_dev;
	below = under->ld_ops->ldo_object_alloc(env, o->lo_header, under);
	if (below == NULL)
		RETURN(-ENOMEM);

	lu_object_add(o, below);

	RETURN(0);
}

void lod_object_free_striping(const struct lu_env *env, struct lod_object *lo)
{
	int i;

	if (lo->ldo_stripe) {
		LASSERT(lo->ldo_stripes_allocated > 0);

		for (i = 0; i < lo->ldo_stripenr; i++) {
			if (lo->ldo_stripe[i])
				lu_object_put(env, &lo->ldo_stripe[i]->do_lu);
		}

		i = sizeof(struct dt_object *) * lo->ldo_stripes_allocated;
		OBD_FREE(lo->ldo_stripe, i);
		lo->ldo_stripe = NULL;
		lo->ldo_stripes_allocated = 0;
	}
	lo->ldo_stripenr = 0;
}

/*
 * ->start is called once all slices are initialized, including header's
 * cache for mode (object type). using the type we can initialize ops
 */
static int lod_object_start(const struct lu_env *env, struct lu_object *o)
{
	if (S_ISLNK(o->lo_header->loh_attr & S_IFMT))
		lu2lod_obj(o)->ldo_obj.do_body_ops = &lod_body_lnk_ops;
	return 0;
}

static void lod_object_free(const struct lu_env *env, struct lu_object *o)
{
	struct lod_object *mo = lu2lod_obj(o);

	/*
	 * release all underlying object pinned
	 */

	lod_object_free_striping(env, mo);

	lod_object_set_pool(mo, NULL);

	lu_object_fini(o);
	OBD_SLAB_FREE_PTR(mo, lod_object_kmem);
}

static void lod_object_release(const struct lu_env *env, struct lu_object *o)
{
	/* XXX: shouldn't we release everything here in case if object
	 * creation failed before? */
}

static int lod_object_print(const struct lu_env *env, void *cookie,
			    lu_printer_t p, const struct lu_object *l)
{
	struct lod_object *o = lu2lod_obj((struct lu_object *) l);

	return (*p)(env, cookie, LUSTRE_LOD_NAME"-object@%p", o);
}

struct lu_object_operations lod_lu_obj_ops = {
	.loo_object_init	= lod_object_init,
	.loo_object_start	= lod_object_start,
	.loo_object_free	= lod_object_free,
	.loo_object_release	= lod_object_release,
	.loo_object_print	= lod_object_print,
};