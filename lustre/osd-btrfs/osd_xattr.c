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

#include <linux/quotaops.h>
#include <libcfs/libcfs.h>
#include "osd_internal.h"

int osd_declare_xattr_set(const struct lu_env *env, struct dt_object *dt,
			  const struct lu_buf *buf, const char *name,
			  int fl, struct thandle *handle)
{
	struct osd_thandle *oh;
	ENTRY;

	LASSERT(handle != NULL);
	oh = container_of0(handle, struct osd_thandle, ot_super);

	osd_trans_declare_op(oh, osd_item_number[OTO_XATTR_SET]);

	RETURN(0);
}

static inline int __osd_xattr_get(struct inode *inode, struct dentry *dentry,
				  const char *name, void *buf, int len)
{
	if (inode == NULL)
		return -EINVAL;

	dentry->d_inode = inode;
	dentry->d_sb = inode->i_sb;
	return inode->i_op->getxattr(dentry, name, buf, len);
}

static inline int __osd_xattr_set(struct osd_thread_info *info,
				  struct btreefs_trans_handle *trans,
				  struct inode *inode, const char *name,
				  const void *buf, int buflen, int fl)
{
	struct dentry *dentry = &info->oti_child_dentry;

	ll_vfs_dq_init(inode);
	dentry->d_inode = inode;
	dentry->d_sb = inode->i_sb;
	return __btreefs_setxattr(trans, inode, name, buf, buflen, fl);
}

/*
 * Get the 64-bit version for an inode.
 */
static int osd_object_version_get(const struct lu_env *env,
                                  struct dt_object *dt, dt_obj_version_t *ver)
{
	struct inode *inode = osd_dt_obj(dt)->oo_inode;

	CDEBUG(D_INODE, "Get version "LPX64" for inode %lu\n",
	       inode->i_version, inode->i_ino);
	*ver = inode->i_version;
	return 0;
}


/*
 * Set the 64-bit version for object
 */
static void osd_object_version_set(const struct lu_env *env,
				   struct dt_object *dt,
				   dt_obj_version_t *new_version)
{
	struct inode *inode = osd_dt_obj(dt)->oo_inode;

	CDEBUG(D_INODE, "Set version "LPX64" (old "LPX64") for inode %lu\n",
	       *new_version, inode->i_version, inode->i_ino);

	inode->i_version = *new_version;
	/** Version is set after all inode operations are finished,
	 *  so we should mark it dirty here */
	/* No s_op->dirty_inode() is defined, so can't use ll_dirty_inode() */
	btreefs_dirty_inode(inode);
}

/*
 * Concurrency: @dt is read locked.
 */
int osd_xattr_get(const struct lu_env *env, struct dt_object *dt,
		  struct lu_buf *buf, const char *name,
		  struct lustre_capa *capa)
{
	struct osd_object      *obj    = osd_dt_obj(dt);
	struct inode           *inode  = obj->oo_inode;
	struct osd_thread_info *info   = osd_oti_get(env);
	struct dentry          *dentry = &info->oti_obj_dentry;

	/* version get is not real XATTR but uses xattr API */
	if (strcmp(name, XATTR_NAME_VERSION) == 0) {
		/* for version we are just using xattr API but change inode
		 * field instead */
		if (buf->lb_len == 0)
			return sizeof(dt_obj_version_t);

		if (buf->lb_len < sizeof(dt_obj_version_t))
			return -ERANGE;

		osd_object_version_get(env, dt, buf->lb_buf);

		return sizeof(dt_obj_version_t);
	}

	LASSERT(dt_object_exists(dt));
	LASSERT(!dt_object_remote(dt));
	LASSERT(inode->i_op != NULL);
	LASSERT(inode->i_op->getxattr != NULL);

#ifdef LIXI
	if (osd_object_auth(env, dt, capa, CAPA_OPC_META_READ))
		return -EACCES;
#endif /* LIXI */ 

	return __osd_xattr_get(inode, dentry, name, buf->lb_buf, buf->lb_len);
}

/*
 * Concurrency: @dt is write locked.
 */
int osd_xattr_set(const struct lu_env *env, struct dt_object *dt,
		  const struct lu_buf *buf, const char *name, int fl,
		  struct thandle *handle, struct lustre_capa *capa)
{
	struct osd_object	*obj      = osd_dt_obj(dt);
	struct inode		*inode    = obj->oo_inode;
	struct osd_thread_info	*info     = osd_oti_get(env);
	int			 fs_flags = 0;
	struct osd_thandle	*oh;
	ENTRY;

	LASSERT(handle != NULL);
	oh = container_of0(handle, struct osd_thandle, ot_super);

	/* version set is not real XATTR */
	if (strcmp(name, XATTR_NAME_VERSION) == 0) {
		/* for version we are just using xattr API but change inode
		* field instead */
		LASSERT(buf->lb_len == sizeof(dt_obj_version_t));
		osd_object_version_set(env, dt, buf->lb_buf);
		return sizeof(dt_obj_version_t);
	}

#ifdef LIXI
	if (osd_object_auth(env, dt, capa, CAPA_OPC_META_WRITE))
		return -EACCES;
#endif /* LIXI */ 

	CDEBUG(D_INODE, DFID" set xattr '%s' with size %zu\n",
	       PFID(lu_object_fid(&dt->do_lu)), name, buf->lb_len);

	if (fl & LU_XATTR_REPLACE)
		fs_flags |= XATTR_REPLACE;

	if (fl & LU_XATTR_CREATE)
		fs_flags |= XATTR_CREATE;

	if (strcmp(name, XATTR_NAME_LMV) == 0) {
#ifdef LIXI
		struct lustre_mdt_attrs *lma = &info->oti_mdt_attrs;
		int			 rc;

		rc = osd_get_lma(info, inode, &info->oti_obj_dentry, lma);
		if (rc != 0)
			RETURN(rc);

		lma->lma_incompat |= LMAI_STRIPED;
		lustre_lma_swab(lma);
		rc = __osd_xattr_set(info, oh->ot_handle, inode,
				     XATTR_NAME_LMA, lma,
				     sizeof(*lma), XATTR_REPLACE);
		if (rc != 0)
			RETURN(rc);
#else /* LIXI */ 
		LBUG();
#endif /* LIXI */ 
	}

	if (OBD_FAIL_CHECK(OBD_FAIL_LFSCK_LINKEA_OVERFLOW) &&
	    strcmp(name, XATTR_NAME_LINK) == 0)
		return -ENOSPC;

	return __osd_xattr_set(info, oh->ot_handle, inode,
			       name, buf->lb_buf, buf->lb_len, fs_flags);
}
