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

#include <lustre_fid.h>
#include <lustre_disk.h>
#include "osd_internal.h"
#include <btrfs/btreefs_inode.h>
#include <btrfs/object-index.h>

static inline int fid_is_fs_root(const struct lu_fid *fid)
{
        /* Map root inode to special local object FID */
        return (unlikely(fid_seq(fid) == FID_SEQ_LOCAL_FILE &&
                         fid_oid(fid) == OSD_FS_ROOT_OID));
}

/**
 * Lookup an existing OI by the given fid.
 */
int
osd_oi_lookup(const struct lu_env *env, struct osd_device *osd,
	      const struct lu_fid *fid, struct osd_inode_id *id)
{
	struct dentry		*dir;
	struct dentry		*dentry;
	struct inode		*inode;
	int		 	 rc;
	struct osd_thread_info	*info = osd_oti_get(env);

	if (fid_is_fs_root(fid)) {
		inode = osd_sb(osd)->s_root->d_inode;
		osd_id_gen(id, btreefs_ino(inode),
			   BTREEFS_I(inode)->generation);
		return 0;
	}

	/*
	 * First, lookup through dentry and then object index
	 * LIXI TODO: We could remove lookup through dentry since OI has all
	 */
	rc = osd_get_idx_and_name(info, osd, fid, &dir,
				  (char *)info->oti_str,
				  sizeof(info->oti_str));
	if (rc)
		return rc;

	if (dir != NULL) {
		int rc2;
		__u64 ino;
		__u64 gen;

		dentry = ll_lookup_one_len(info->oti_str, dir, strlen(info->oti_str));
		if (IS_ERR(dentry))
			return PTR_ERR(dentry);
		else if (dentry->d_inode == NULL)
			GOTO(out, rc = -ENOENT);

		inode = dentry->d_inode;
		if (is_bad_inode(inode))
			GOTO(out, rc = -EIO);

		osd_id_gen(id, btreefs_ino(inode),
			   BTREEFS_I(inode)->generation);
out:
		/* if dentry is accessible after insert it
		 * will still contain NULL inode, so don't keep it in cache */
		d_invalidate(dentry);
		dput(dentry);

		rc2 = btreefs_oi_lookup_with_fid(osd_sb(osd),
						(struct btreefs_lu_fid *)fid,
						&ino, &gen);
		CERROR("osd_oi_lookup with dentry, fid "DFID", "
		       "name = %s/%s, root inode %p,"
		       "rc = %d, rc2 = %d\n",
		       PFID(fid),
		       dir->d_name.name, info->oti_str,
		       osd->od_mnt->mnt_root->d_inode,
		       rc, rc2);
		if (rc == 0) {
			LASSERT(rc2 == 0);
			LASSERT(ino == id->oii_ino);
			LASSERT(gen == id->oii_gen);
		} else if (rc == -ENOENT) {
			LASSERT(rc2 == -ENOENT);
		} else {
			LASSERT(rc2 != 0);
		}

		return rc;
	}

	rc = btreefs_oi_lookup_with_fid(osd_sb(osd),
					(struct btreefs_lu_fid *)fid,
					&id->oii_ino, &id->oii_gen);
	CERROR("osd_oi_lookup, fid "DFID", rc = %d\n",
	       PFID(fid), rc);

	return rc;
}

static struct inode *osd_iget_check(struct osd_thread_info *info,
				    struct osd_device *osd,
				    const struct lu_fid *fid,
				    struct osd_inode_id *id)
{
	/* LIXI TODO: Check generation or so. */
	return btreefs_inode_get(osd->od_mnt->mnt_sb, id->oii_ino);
}

int
osd_fid_lookup(const struct lu_env *env, struct osd_object *obj,
	       const struct lu_fid *fid)
{
	struct lu_device       *ldev   = obj->oo_dt.do_lu.lo_dev;
	struct osd_device      *osd = osd_dev(ldev);
	struct inode *inode;
	struct osd_thread_info *info = osd_oti_get(env);
	struct osd_inode_id *id = &info->oti_id;
	int rc;

	LASSERT(obj->oo_inode == NULL);

	if (OBD_FAIL_CHECK(OBD_FAIL_OST_ENOENT))
		RETURN(-ENOENT);

	/* LIXI TODO: if (unlikely(fid_is_acct(fid))) */
	rc = osd_oi_lookup(env, osd, fid, id);
	if (rc == -ENOENT)
		GOTO(out, rc = 0);
	else if (rc)
		GOTO(out, rc);

	inode = osd_iget_check(info, osd, fid, id);
	if (IS_ERR(inode))
		GOTO(out, rc = PTR_ERR(inode));

	obj->oo_inode = inode;
	LASSERT(obj->oo_inode->i_sb == osd_sb(osd));

	/* LIXI TODO: osd_check_lma(env, obj); */
out:
	LINVRNT(osd_invariant(obj));
	return rc;
}

static void osd_fid2str(char *buf, const struct lu_fid *fid)
{
	sprintf(buf, DFID_NOBRACE, PFID(fid));
}

struct named_oid {
	unsigned long	 oid;
	char		*name;
};


/**
 * Lookup the target index/flags of the fid, so it will know where
 * the object is located (tgt index) and it is MDT or OST object.
 */
int osd_fld_lookup(const struct lu_env *env, struct osd_device *osd,
		   obd_seq seq, struct lu_seq_range *range)
{
	struct seq_server_site	*ss = osd_seq_site(osd);

	if (fid_seq_is_idif(seq)) {
		fld_range_set_ost(range);
		range->lsr_index = idif_ost_idx(seq);
		return 0;
	}

	if (!fid_seq_in_fldb(seq)) {
		fld_range_set_mdt(range);
		if (ss != NULL)
			/* FIXME: If ss is NULL, it suppose not get lsr_index
			 * at all */
			range->lsr_index = ss->ss_node_id;
		return 0;
	}

	LASSERT(ss != NULL);
	fld_range_set_any(range);
	/* OSD will only do local fld lookup */
	return fld_local_lookup(env, ss->ss_server_fld, seq, range);
}

int fid_is_on_ost(const struct lu_env *env, struct osd_device *osd,
		  const struct lu_fid *fid)
{
	struct lu_seq_range	*range = &osd_oti_get(env)->oti_seq_range;
	int			 rc;
	ENTRY;

#ifdef LIXI
	if (flags & OI_KNOWN_ON_OST)
		RETURN(1);
#endif /* LIXI */

	if (unlikely(fid_is_local_file(fid) || fid_is_igif(fid) ||
		     fid_is_llog(fid)) || fid_is_name_llog(fid) ||
		     fid_is_quota(fid))
		RETURN(0);

	if (fid_is_idif(fid) || fid_is_last_id(fid))
		RETURN(1);

#ifdef LIXI
	if (!(flags & OI_CHECK_FLD))
		RETURN(0);
#endif /* LIXI */

	rc = osd_fld_lookup(env, osd, fid_seq(fid), range);
	if (rc != 0) {
		if (rc != -ENOENT)
			CERROR("%s: lookup FLD "DFID": rc = %d\n",
			       osd_name(osd), PFID(fid), rc);
		RETURN(0);
	}

	if (fld_range_is_ost(range))
		RETURN(1);

	RETURN(0);
}

static const struct named_oid oids[] = {
	{ LAST_RECV_OID,		LAST_RCVD },
	{ OFD_LAST_GROUP_OID,		"LAST_GROUP" },
	{ LLOG_CATALOGS_OID,		"CATALOGS" },
	{ MGS_CONFIGS_OID,              "CONFIGS" },
	{ FID_SEQ_SRV_OID,              "seq_srv" },
	{ FID_SEQ_CTL_OID,              "seq_ctl" },
	{ FLD_INDEX_OID,                "fld" },
	{ MDD_LOV_OBJ_OID,		LOV_OBJID },
	{ OFD_HEALTH_CHECK_OID,		HEALTH_CHECK },
	{ ACCT_USER_OID,		"acct_usr_inode" },
	{ ACCT_GROUP_OID,		"acct_grp_inode" },
	{ 0,				NULL }
};

char *named_oid2name(const unsigned long oid)
{
	int i = 0;

	while (oids[i].oid) {
		if (oids[i].oid == oid)
			return oids[i].name;
		i++;
	}
	return NULL;
}

void osd_get_name_from_fid(const struct lu_fid *fid, char *buf)
{
	char *name = named_oid2name(fid_oid(fid));

	if (name)
		strcpy(buf, name);
	else
		osd_fid2str(buf, fid);
}

static int
osd_oi_create(const struct lu_env *env, struct osd_device *osd,
	      struct lu_fid *fid, struct inode *inode)
{
	struct btreefs_trans_handle	*handle;
	int			   	 rc = 0;
	ENTRY;

	handle = btreefs_trans_start(osd_sb(osd),
				     osd_item_number[OTO_INDEX_INSERT]);
	if (IS_ERR(handle))
		GOTO(out, rc = PTR_ERR(handle));

	rc = btreefs_oi_insert(handle, osd_sb(osd),
			       (struct btreefs_lu_fid *)fid,
			       btreefs_ino(inode),
			       BTREEFS_I(inode)->generation);
	btreefs_trans_stop(osd_sb(osd), handle);
out:
	RETURN(rc);
}

static int
osd_oi_find_or_create(const struct lu_env *env, struct osd_device *osd,
		      struct lu_fid *fid,
		      struct inode *inode)
{
	struct osd_inode_id	inode_id;
	int			rc;
	ENTRY;

	rc = btreefs_oi_lookup_with_fid(osd_sb(osd),
					(struct btreefs_lu_fid *)fid,
					&inode_id.oii_ino, &inode_id.oii_gen);
	if (rc == -ENOENT)
		rc = osd_oi_create(env, osd, fid, inode);
	else if (rc)
		GOTO(out, rc);
	/* TODO: LIXI add inode number/generation check */

out:
	RETURN(rc);
}

/**
 * Create /O subdirectory to map legacy OST objects for compatibility.
 */
static int
osd_oi_init_compat(const struct lu_env *env, struct osd_device *osd)
{
	int		 rc;
	struct dentry	*child_dentry;
	const char	*name = MOUNT_CONFIGS_DIR;
	struct lu_fid	 fid = { FID_SEQ_LOCAL_FILE, MGS_CONFIGS_OID, 0 };
	ENTRY;

	/*
	 * Only CONFIGS/mountdata is created in advance by user space utility.
	 * Add the object index for it.
	 */
	child_dentry = ll_lookup_one_len(name,
					 osd->od_mnt->mnt_root,
					 strlen(name));
	if (IS_ERR(child_dentry))
		RETURN(PTR_ERR(child_dentry));
	else if (child_dentry->d_inode == NULL)
		GOTO(out, rc = -ENOENT);

	rc = osd_oi_find_or_create(env, osd, &fid, child_dentry->d_inode);
out:
	dput(child_dentry);
	RETURN(rc);
}

/**
 * Initialize the OIs by either opening or creating them as needed.
 */
int osd_oi_init(const struct lu_env *env, struct osd_device *o)
{
	int rc;
	ENTRY;

	rc = osd_oi_init_compat(env, o);
	if (rc)
		RETURN(rc);

	RETURN(rc);
}
