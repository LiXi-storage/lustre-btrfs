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

static ssize_t osd_declare_write(const struct lu_env *env, struct dt_object *dt,
				 const struct lu_buf *buf, loff_t _pos,
				 struct thandle *handle)
{
	struct osd_thandle	*oh;
	struct osd_object	*obj = osd_dt_obj(dt);
	struct inode		*inode = obj->oo_inode;
	struct super_block	*sb = osd_sb(osd_obj2dev(obj));
	int			 bits = sb->s_blocksize_bits;
	int			 bs = 1 << bits;
	int			 item_number;
	ENTRY;

	oh = container_of0(handle, struct osd_thandle, ot_super);
	LASSERT(oh->ot_handle == NULL);

	/* LIXI TODO: This is a estimate far from precise enough */
	item_number = (buf->lb_len + bs - 1) >> bits;
	if (inode != NULL && _pos != -1) {
		osd_trans_declare_op(oh, osd_item_number[OTO_TRUNCATE_BASE]);
		item_number += (i_size_read(inode) + bs - 1) >> bits;
	}
	osd_trans_declare_op(oh, item_number);

	RETURN(0);
}

static ssize_t osd_write(const struct lu_env *env, struct dt_object *dt,
                         const struct lu_buf *buf, loff_t *pos,
                         struct thandle *handle, int ignore_quota)
{
	struct inode		*inode = osd_dt_obj(dt)->oo_inode;
	struct osd_thandle	*oh;
	ssize_t			 rc;

	LASSERT(dt_object_exists(dt));


	LASSERT(handle != NULL);
	LASSERT(inode != NULL);
#ifdef LIXI
	ll_vfs_dq_init(inode);
#endif /* LIXI */

	oh = container_of(handle, struct osd_thandle, ot_super);

	/* LIXI TODO: symlink support */
	LASSERT(!S_ISLNK(dt->do_lu.lo_header->loh_attr));
	rc = btreefs_write_record(inode, buf->lb_buf, buf->lb_len, pos);
        return rc;
}

static ssize_t osd_read(const struct lu_env *env, struct dt_object *dt,
                        struct lu_buf *buf, loff_t *pos)
{
	struct inode *inode = osd_dt_obj(dt)->oo_inode;
	int           rc;


	LASSERT(!S_ISLNK(dt->do_lu.lo_header->loh_attr));
	rc = btreefs_read_record(inode, buf->lb_buf, buf->lb_len, pos);

        return rc;
}

/*
 * in some cases we may need declare methods for objects being created
 * e.g., when we create symlink
 */
const struct dt_body_operations osd_body_ops_new = {
        .dbo_declare_write = osd_declare_write,
};

const struct dt_body_operations osd_body_ops = {
#ifdef LIXI
	.dbo_read                 = osd_read,
	.dbo_declare_write        = osd_declare_write,
	.dbo_write                = osd_write,
	.dbo_bufs_get             = osd_bufs_get,
	.dbo_bufs_put             = osd_bufs_put,
	.dbo_write_prep           = osd_write_prep,
	.dbo_declare_write_commit = osd_declare_write_commit,
	.dbo_write_commit         = osd_write_commit,
	.dbo_read_prep            = osd_read_prep,
	.dbo_declare_punch        = osd_declare_punch,
	.dbo_punch                = osd_punch,
	.dbo_fiemap_get           = osd_fiemap_get,
#else /* LIXI */
	.dbo_read                 = osd_read,
	.dbo_declare_write        = osd_declare_write,
	.dbo_write                = osd_write,
	.dbo_bufs_get             = NULL,
	.dbo_bufs_put             = NULL,
	.dbo_write_prep           = NULL,
	.dbo_declare_write_commit = NULL,
	.dbo_write_commit         = NULL,
	.dbo_read_prep            = NULL,
	.dbo_declare_punch        = NULL,
	.dbo_punch                = NULL,
	.dbo_fiemap_get           = NULL,
#endif /* LIXI */
};
