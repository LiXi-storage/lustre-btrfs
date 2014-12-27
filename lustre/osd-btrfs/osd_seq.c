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

static void osd_push_ctxt(const struct osd_device *dev,
                          struct lvfs_run_ctxt *newctxt,
                          struct lvfs_run_ctxt *save)
{
	OBD_SET_CTXT_MAGIC(newctxt);
	newctxt->pwdmnt = dev->od_mnt;
	newctxt->pwd = dev->od_mnt->mnt_root;
	newctxt->fs = get_ds();
	newctxt->umask = current_umask();
	newctxt->dt = NULL;

	push_ctxt(save, newctxt);
}

/* utility to make a directory */
static struct dentry *simple_mkdir(struct dentry *dir, struct vfsmount *mnt,
				   const char *name, int mode, int fix)
{
	struct dentry *dchild;
	int err = 0;
	ENTRY;

	// ASSERT_KERNEL_CTXT("kernel doing mkdir outside kernel context\n");
	CDEBUG(D_INODE, "creating directory %.*s\n", (int)strlen(name), name);
	dchild = ll_lookup_one_len(name, dir, strlen(name));
	if (IS_ERR(dchild))
		GOTO(out_up, dchild);

	if (dchild->d_inode) {
		int old_mode = dchild->d_inode->i_mode;
		if (!S_ISDIR(old_mode)) {
			CERROR("found %s (%lu/%u) is mode %o\n", name,
			       dchild->d_inode->i_ino,
			       dchild->d_inode->i_generation, old_mode);
			GOTO(out_err, err = -ENOTDIR);
		}

		/* Fixup directory permissions if necessary */
		if (fix && (old_mode & S_IALLUGO) != (mode & S_IALLUGO)) {
			CDEBUG(D_CONFIG,
			       "fixing permissions on %s from %o to %o\n",
			       name, old_mode, mode);
			dchild->d_inode->i_mode = (mode & S_IALLUGO) |
						  (old_mode & ~S_IALLUGO);
			mark_inode_dirty(dchild->d_inode);
		}
		GOTO(out_up, dchild);
	}

	err = vfs_mkdir(dir->d_inode, dchild, mode);
	if (err)
		GOTO(out_err, err);

	RETURN(dchild);

out_err:
	dput(dchild);
	dchild = ERR_PTR(err);
out_up:
	return dchild;
}

static const char remote_parent_dir[] = "REMOTE_PARENT_DIR";
static int osd_mdt_init(const struct lu_env *env, struct osd_device *dev)
{
	struct lvfs_run_ctxt	new;
	struct lvfs_run_ctxt	save;
	struct dentry		*parent;
	struct osd_mdobj_map	*omm;
	struct dentry		*d;
#ifdef LIXI
	struct osd_thread_info	*info = osd_oti_get(env);
	struct lu_fid		*fid = &info->oti_fid3;
#endif
	int			rc = 0;
	ENTRY;

	OBD_ALLOC_PTR(dev->od_mdt_map);
	if (dev->od_mdt_map == NULL)
		RETURN(-ENOMEM);

	omm = dev->od_mdt_map;

	parent = osd_sb(dev)->s_root;
	osd_push_ctxt(dev, &new, &save);

	d = simple_mkdir(parent, dev->od_mnt, remote_parent_dir,
			 0755, 1);
	if (IS_ERR(d))
		GOTO(cleanup, rc = PTR_ERR(d));

	omm->omm_remote_parent = d;

#ifdef LIXI
	/* Set LMA for remote parent inode */
	lu_local_obj_fid(fid, REMOTE_PARENT_DIR_OID);
	rc = osd_ea_fid_set(info, d->d_inode, fid, LMAC_NOT_IN_OI, 0);
#endif

	GOTO(cleanup, rc);

cleanup:
	pop_ctxt(&save, &new);
	if (rc) {
		if (omm->omm_remote_parent != NULL)
			dput(omm->omm_remote_parent);
		OBD_FREE_PTR(omm);
		dev->od_mdt_map = NULL;
	}
	return rc;
}

static void osd_mdt_fini(struct osd_device *osd)
{
	struct osd_mdobj_map *omm = osd->od_mdt_map;

	if (omm == NULL)
		return;

	if (omm->omm_remote_parent)
		dput(omm->omm_remote_parent);

	OBD_FREE_PTR(omm);
	osd->od_ost_map = NULL;
}

/*
 * directory structure on legacy OST:
 *
 * O/<seq>/d0-31/<objid>
 * O/<seq>/LAST_ID
 * last_rcvd
 * LAST_GROUP
 * CONFIGS
 *
 */
static int osd_ost_init(const struct lu_env *env, struct osd_device *dev)
{
	struct lvfs_run_ctxt	 new;
	struct lvfs_run_ctxt	 save;
	struct dentry		*rootd = osd_sb(dev)->s_root;
	struct dentry		*d;
#ifdef LIXI
	struct osd_thread_info	*info = osd_oti_get(env);
#endif /* LIXI */
	struct inode		*inode;
#ifdef LIXI
	struct lu_fid		*fid = &info->oti_fid3;
#endif /* LIXI */
	int			 rc = 0;
	ENTRY;

	OBD_ALLOC_PTR(dev->od_ost_map);
	if (dev->od_ost_map == NULL)
		RETURN(-ENOMEM);

#ifdef LIXI
	/* to get subdir count from last_rcvd */
	rc = osd_last_rcvd_subdir_count(dev);
	if (rc < 0) {
		OBD_FREE_PTR(dev->od_ost_map);
		RETURN(rc);
	}

	dev->od_ost_map->om_subdir_count = rc;
        rc = 0;
#endif /* LIXI */

	dev->od_ost_map->om_subdir_count = OSD_OST_MAP_SIZE;
	INIT_LIST_HEAD(&dev->od_ost_map->om_seq_list);
	rwlock_init(&dev->od_ost_map->om_seq_list_lock);
	mutex_init(&dev->od_ost_map->om_dir_init_mutex);

        osd_push_ctxt(dev, &new, &save);

	d = ll_lookup_one_len("O", rootd, strlen("O"));
	if (IS_ERR(d))
		GOTO(cleanup, rc = PTR_ERR(d));
	if (d->d_inode == NULL) {
		dput(d);
		/* The lookup() may be called again inside simple_mkdir().
		 * Since the repeated lookup() only be called for "/O" at
		 * mount time, it will not affect the whole performance. */
		d = simple_mkdir(rootd, dev->od_mnt, "O", 0755, 1);
		if (IS_ERR(d))
			GOTO(cleanup, rc = PTR_ERR(d));

#ifdef LII
		/* It is quite probably that the device is new formatted. */
		dev->od_maybe_new = 1;
#endif /* LIXI */
	}

	inode = d->d_inode;
	dev->od_ost_map->om_root = d;

#ifdef LIXI
	/* 'What the @fid is' is not imporatant, because the object
	 * has no OI mapping, and only is visible inside the OSD.*/
	lu_igif_build(fid, inode->i_ino, inode->i_generation);
	rc = osd_ea_fid_set(info, inode, fid,
			    LMAC_NOT_IN_OI | LMAC_FID_ON_OST, 0);
#endif /* LIXI */

	GOTO(cleanup, rc);

cleanup:
	pop_ctxt(&save, &new);
        if (IS_ERR(d)) {
                OBD_FREE_PTR(dev->od_ost_map);
                RETURN(PTR_ERR(d));
        }
	return rc;
}

static void osd_seq_free(struct osd_obj_map *map,
			 struct osd_obj_seq *osd_seq)
{
	int j;

	list_del_init(&osd_seq->oos_seq_list);

	if (osd_seq->oos_dirs) {
		for (j = 0; j < osd_seq->oos_subdir_count; j++) {
			if (osd_seq->oos_dirs[j])
				dput(osd_seq->oos_dirs[j]);
                }
		OBD_FREE(osd_seq->oos_dirs,
			 sizeof(struct dentry *) * osd_seq->oos_subdir_count);
        }

	if (osd_seq->oos_root)
		dput(osd_seq->oos_root);

	OBD_FREE_PTR(osd_seq);
}

static void osd_ost_fini(struct osd_device *osd)
{
	struct osd_obj_seq    *osd_seq;
	struct osd_obj_seq    *tmp;
	struct osd_obj_map    *map = osd->od_ost_map;
	ENTRY;

	if (map == NULL)
		return;

	write_lock(&map->om_seq_list_lock);
	list_for_each_entry_safe(osd_seq, tmp, &map->om_seq_list,
				 oos_seq_list) {
		osd_seq_free(map, osd_seq);
	}
	write_unlock(&map->om_seq_list_lock);
	if (map->om_root)
		dput(map->om_root);
	OBD_FREE_PTR(map);
	osd->od_ost_map = NULL;
	EXIT;
}

int osd_obj_map_init(const struct lu_env *env, struct osd_device *dev)
{
	int rc;
	ENTRY;

	/* prepare structures for OST */
	rc = osd_ost_init(env, dev);
	if (rc)
		RETURN(rc);

	/* prepare structures for MDS */
	rc = osd_mdt_init(env, dev);

        RETURN(rc);
}

struct osd_obj_seq *osd_seq_find_locked(struct osd_obj_map *map, __u64 seq)
{
	struct osd_obj_seq *osd_seq;

	list_for_each_entry(osd_seq, &map->om_seq_list, oos_seq_list) {
		if (osd_seq->oos_seq == seq)
			return osd_seq;
	}
	return NULL;
}

struct osd_obj_seq *osd_seq_find(struct osd_obj_map *map, __u64 seq)
{
	struct osd_obj_seq *osd_seq;

	read_lock(&map->om_seq_list_lock);
	osd_seq = osd_seq_find_locked(map, seq);
	read_unlock(&map->om_seq_list_lock);
	return osd_seq;
}

void osd_obj_map_fini(struct osd_device *dev)
{
	osd_ost_fini(dev);
	osd_mdt_fini(dev);
}

/**
 * Use LPU64 for legacy OST sequences, but use LPX64i for new
 * sequences names, so that the O/{seq}/dN/{oid} more closely
 * follows the DFID/PFID format. This makes it easier to map from
 * debug messages to objects in the future, and the legacy space
 * of FID_SEQ_OST_MDT0 will be unused in the future.
 **/
static inline void osd_seq_name(char *seq_name, size_t name_size, __u64 seq)
{
	snprintf(seq_name, name_size,
		 (fid_seq_is_rsvd(seq) ||
		  fid_seq_is_mdt0(seq)) ? LPU64 : LPX64i,
		 fid_seq_is_idif(seq) ? 0 : seq);
}

static inline void osd_oid_name(char *name, size_t name_size,
				const struct lu_fid *fid, __u64 id)
{
	snprintf(name, name_size,
		 (fid_seq_is_rsvd(fid_seq(fid)) ||
		  fid_seq_is_mdt0(fid_seq(fid)) ||
		  fid_seq_is_idif(fid_seq(fid))) ? LPU64 : LPX64i, id);
}

/* external locking is required */
static int osd_seq_load_locked(struct osd_thread_info *info,
			       struct osd_device *osd,
			       struct osd_obj_seq *osd_seq)
{
	struct osd_obj_map  *map = osd->od_ost_map;
	struct dentry       *seq_dir;
	struct inode	    *inode;
#ifdef LIXI
	struct lu_fid	    *fid = &info->oti_fid3;
#endif /* LIXI */
	int		    rc = 0;
	int		    i;
	char		    dir_name[32];
	ENTRY;

	if (osd_seq->oos_root != NULL)
		RETURN(0);

	LASSERT(map);
	LASSERT(map->om_root);

	osd_seq_name(dir_name, sizeof(dir_name), osd_seq->oos_seq);

	seq_dir = simple_mkdir(map->om_root, osd->od_mnt, dir_name, 0755, 1);
	if (IS_ERR(seq_dir))
		GOTO(out_err, rc = PTR_ERR(seq_dir));
	else if (seq_dir->d_inode == NULL)
		GOTO(out_put, rc = -EFAULT);

	inode = seq_dir->d_inode;
	osd_seq->oos_root = seq_dir;

#ifdef LIXI
	/* 'What the @fid is' is not imporatant, because the object
	 * has no OI mapping, and only is visible inside the OSD.*/
	lu_igif_build(fid, inode->i_ino, inode->i_generation);
	rc = osd_ea_fid_set(info, inode, fid,
			    LMAC_NOT_IN_OI | LMAC_FID_ON_OST, 0);
	if (rc != 0)
		GOTO(out_put, rc);
#endif /* LIXI */

	LASSERT(osd_seq->oos_dirs == NULL);
	OBD_ALLOC(osd_seq->oos_dirs,
		  sizeof(seq_dir) * osd_seq->oos_subdir_count);
	if (osd_seq->oos_dirs == NULL)
		GOTO(out_put, rc = -ENOMEM);

	for (i = 0; i < osd_seq->oos_subdir_count; i++) {
		struct dentry   *dir;

		snprintf(dir_name, sizeof(dir_name), "d%u", i);
		dir = simple_mkdir(osd_seq->oos_root, osd->od_mnt, dir_name,
				   0700, 1);
		if (IS_ERR(dir)) {
			GOTO(out_free, rc = PTR_ERR(dir));
		} else if (dir->d_inode == NULL) {
			dput(dir);
			GOTO(out_free, rc = -EFAULT);
		}

		inode = dir->d_inode;
		osd_seq->oos_dirs[i] = dir;

#ifdef LIXI
		/* 'What the @fid is' is not imporatant, because the object
		 * has no OI mapping, and only is visible inside the OSD.*/
		lu_igif_build(fid, inode->i_ino, inode->i_generation);
		rc = osd_ea_fid_set(info, inode, fid,
				    LMAC_NOT_IN_OI | LMAC_FID_ON_OST, 0);
		if (rc != 0)
			GOTO(out_free, rc);
#endif /* LIXI */
	}

	if (rc != 0) {
out_free:
		for (i = 0; i < osd_seq->oos_subdir_count; i++) {
			if (osd_seq->oos_dirs[i] != NULL)
				dput(osd_seq->oos_dirs[i]);
		}
		OBD_FREE(osd_seq->oos_dirs,
			 sizeof(seq_dir) * osd_seq->oos_subdir_count);
out_put:
		dput(seq_dir);
		osd_seq->oos_root = NULL;
	}
out_err:
	RETURN(rc);
}

struct osd_obj_seq *osd_seq_load(struct osd_thread_info *info,
				 struct osd_device *osd, __u64 seq)
{
	struct osd_obj_map	*map;
	struct osd_obj_seq	*osd_seq;
	int			rc = 0;
	ENTRY;

	map = osd->od_ost_map;
	LASSERT(map);
	LASSERT(map->om_root);

	osd_seq = osd_seq_find(map, seq);
	if (likely(osd_seq != NULL))
		RETURN(osd_seq);

	/* Serializing init process */
	mutex_lock(&map->om_dir_init_mutex);

	/* Check whether the seq has been added */
	read_lock(&map->om_seq_list_lock);
	osd_seq = osd_seq_find_locked(map, seq);
	if (osd_seq != NULL) {
		read_unlock(&map->om_seq_list_lock);
		GOTO(cleanup, rc = 0);
	}
	read_unlock(&map->om_seq_list_lock);

	OBD_ALLOC_PTR(osd_seq);
	if (osd_seq == NULL)
		GOTO(cleanup, rc = -ENOMEM);

	INIT_LIST_HEAD(&osd_seq->oos_seq_list);
	osd_seq->oos_seq = seq;
	/* Init subdir count to be 32, but each seq can have
	 * different subdir count */
	osd_seq->oos_subdir_count = map->om_subdir_count;
	rc = osd_seq_load_locked(info, osd, osd_seq);
	if (rc != 0)
		GOTO(cleanup, rc);

	write_lock(&map->om_seq_list_lock);
	list_add(&osd_seq->oos_seq_list, &map->om_seq_list);
	write_unlock(&map->om_seq_list_lock);

cleanup:
	mutex_unlock(&map->om_dir_init_mutex);
	if (rc != 0) {
		if (osd_seq != NULL)
			OBD_FREE_PTR(osd_seq);
		RETURN(ERR_PTR(rc));
	}

	RETURN(osd_seq);
}

int osd_get_idx_and_name(struct osd_thread_info *info, struct osd_device *osd,
			 const struct lu_fid *fid, struct dentry **dir,
			 char *buf, size_t buf_size)
{
	struct osd_obj_map	*map = osd->od_ost_map;
	char			*name;
	struct ost_id		*ostid = &info->oti_ostid;
	__u64			 oid;
	struct osd_obj_seq	*osd_seq;
	int			 dirn;

	LASSERT(map);
	CERROR("osd_oi_lookup LIXI XXX fid "DFID"\n",
	       PFID(fid));
	if (unlikely(fid_seq(fid) == FID_SEQ_LOCAL_FILE)) {
		name = named_oid2name(fid_oid(fid));
		if (name) {
			/* Special FIDs */
			strcpy(buf, name);
			*dir = osd->od_mnt->mnt_root;
			return 0;
		} else {
			/* LIXI TODO */
			LBUG();
		}
	} else if (unlikely(fid_is_name_llog(fid) &&
		   (!fid_is_last_id(fid)))) {
		/* The dentry shouldn't be determined by FID */
		*dir = NULL;
		return 0;
	}

	/* LIXI TODO */
	LASSERT(fid_is_on_ost(info->oti_env, osd, fid) ||
		fid_seq(fid) == FID_SEQ_ECHO ||
		fid_is_llog(fid) ||
		fid_is_last_id(fid));

	if (fid_is_last_id(fid)) {
		/* on creation of LAST_ID we create O/<seq> hierarchy */
		osd_seq = osd_seq_load(info, osd, fid_seq(fid));
		if (IS_ERR(osd_seq))
			RETURN(PTR_ERR(osd_seq));

		strcpy(buf, "LAST_ID");
		*dir = osd_seq->oos_root;
	} else {
		/* map fid to seq:objid */
		fid_to_ostid(fid, ostid);

		oid = ostid_id(ostid);
		osd_seq = osd_seq_load(info, osd, ostid_seq(ostid));
		if (IS_ERR(osd_seq))
			RETURN(PTR_ERR(osd_seq));
		dirn = oid & (osd_seq->oos_subdir_count - 1);
		*dir = osd_seq->oos_dirs[dirn];
		LASSERT(*dir);
		osd_oid_name(buf, buf_size, fid, oid);
	}

	return 0;
}
