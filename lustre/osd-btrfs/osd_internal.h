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

#ifndef _OSD_INTERNAL_H
#define _OSD_INTERNAL_H


/* struct mutex */
#include <linux/mutex.h>
/* struct rw_semaphore */
#include <linux/rwsem.h>
/* struct dentry */
#include <linux/dcache.h>
/* struct dirent64 */
#include <linux/dirent.h>
#include <linux/statfs.h>

#include <dt_object.h>

/* LUSTRE_OSD_NAME */
#include <obd.h>

#include <btrfs/object-index.h>
#include <btrfs/btreefs_inode.h>

/*
 * Storage cookie. Datum uniquely identifying inode on the underlying file
 * system.
 *
 * osd_inode_id is the internal btrfs identifier for an object. It should
 * not be visible outside of the osd-btrfs. Other OSDs may have different
 * identifiers, so this cannot form any part of the OSD API.
 */
struct osd_inode_id {
	__u64 oii_ino; /* inode number */
	__u64 oii_gen; /* inode generation */
};

struct osd_obj_seq {
	/* protects on-fly initialization */
	 /* subdir count for each seq */
	int			  oos_subdir_count;
	/* O/<seq> */
	struct dentry		 *oos_root;
	/* O/<seq>/d0-dXX */
	struct dentry		**oos_dirs;
	/* seq number */
	__u64			  oos_seq;
	/* list to seq_list */
	struct list_head	  oos_seq_list;
};

struct osd_obj_map {
	/* dentry for /O */
	struct dentry		*om_root;
	/* lock for seq_list */
	rwlock_t		 om_seq_list_lock;
	/* list head for seq */
	struct list_head	 om_seq_list;
	int			 om_subdir_count;
	struct mutex		 om_dir_init_mutex;
};

struct osd_mdobj {
	struct dentry	*om_root;      /* AGENT/<index> */
	__u64		 om_index;     /* mdt index */
	struct list_head om_list;      /* list to omm_list */
};

struct osd_mdobj_map {
	struct dentry	*omm_remote_parent;
};

#define OSD_OST_MAP_SIZE	32

/*
 * osd device.
 */
struct osd_device {
	/* super-class */
	struct dt_device	 od_dt_dev;
	/* information about underlying file system */
	struct vfsmount		*od_mnt;
	/* service name associated with the osd device */
	char			 od_svname[MAX_OBD_NAME];
	char			 od_mntdev[MAX_OBD_NAME];
	int			 od_connects;
	struct lu_site		 od_site;
	spinlock_t		 od_osfs_lock;
	struct hlist_head	*od_capa_hash;
	unsigned int		 od_is_ost:1;
	struct proc_dir_entry	*od_proc_entry;
	struct osd_obj_map	*od_ost_map;
	struct osd_mdobj_map	*od_mdt_map;
};

struct osd_object {
	struct dt_object	 oo_dt;
	/**
	 * Inode for file system object represented by this osd_object. This
	 * inode is pinned for the whole duration of lu_object life.
	 *
	 * Not modified concurrently (either setup early during object
	 * creation, or assigned by osd_object_create() under write lock).
	 */
	struct inode		*oo_inode;
	/** write/read lock of object. */
	struct rw_semaphore	 oo_sem;
	/** protects inode attributes. */
	spinlock_t		 oo_guard;
	/** Btrfs index */
	u64			 oo_index;
};

struct osd_it_index {
	struct osd_object	*oii_obj;
};

struct osd_iobuf {
	int		  dr_max_pages;
	int		  dr_npages;
	struct page	**dr_pages;
	struct lu_buf	  dr_pg_buf;
};

struct osd_thread_info {
	const struct lu_env	 *oti_env;
	struct kstatfs		  oti_ksfs;
	struct osd_inode_id	  oti_id;
	/**
	 * used for index operations.
	 */
	struct dentry		  oti_obj_dentry;
	struct dentry		  oti_child_dentry;
	struct qstr		  oti_qstr;
	char			  oti_str[64];
	/**
	 * Max page number per I/O iteration
	 */
	int			  oti_nrptrs;
	/**
	 * Buffer to save page address when I/O
	 */
	struct page		**oti_pages;
	struct lu_seq_range	  oti_seq_range;
	struct ost_id		  oti_ostid;
	unsigned int		  oti_it_inline:1;
	struct osd_it_index	  oti_it_index;
	/*
	 * XXX temporary: for ->i_op calls.
	*/
	struct timespec		  oti_time;
	/** 0-copy IO */
	struct osd_iobuf	  oti_iobuf;
	/*
	 * XXX temporary: fake struct file for osd_object_sync
	 */
	struct file		  oti_file;
};

struct osd_thandle {
	struct thandle			 ot_super;
	struct btreefs_trans_handle	*ot_handle;
	int				 ot_item_number;
	struct btreefs_trans_cb_entry	 ot_callback;
	struct list_head		 ot_dcb_list;
	/* Link to the device, for debugging. */
	struct lu_ref_link		 ot_dev_link;
};

/**
 * Basic transaction item number
 */
enum osd_trans_op {
	OTO_INDEX_INSERT = 0,		/* Insert OI */
	OTO_INDEX_DELETE,		/* Delete OI */
	OTO_OBJECT_CREATE,		/* Create inode */
	OTO_OBJECT_DELETE,		/* Remove inode */
	OTO_DENTRY_ADD,			/* Add a dentry to inode */
	OTO_DENTRY_DELTE,		/* Delete a dentry of inode */
	OTO_ATTR_SET_BASE,
	OTO_XATTR_SET,
	//OTO_WRITE_BASE,
	//OTO_WRITE_BLOCK,
	//OTO_ATTR_SET_CHOWN,
	OTO_TRUNCATE_BASE,
	OTO_NR
};

extern const int osd_item_number[OTO_NR];

extern const struct dt_body_operations osd_body_ops;
extern const struct dt_body_operations osd_body_ops_new;

static inline void osd_trans_declare_op(struct osd_thandle *oh,
					int item_number)
{
	LASSERT(oh->ot_handle == NULL);
	oh->ot_item_number += item_number;
}

static inline int osd_invariant(const struct osd_object *obj)
{
	return 1;
}

/*
 * Helpers.
 */
extern const struct lu_device_operations  osd_lu_ops;

static inline int lu_device_is_osd(const struct lu_device *d)
{
	return ergo(d != NULL && d->ld_ops != NULL, d->ld_ops == &osd_lu_ops);
}

static inline struct osd_object *osd_obj(const struct lu_object *o)
{
	LASSERT(lu_device_is_osd(o->lo_dev));
	return container_of0(o, struct osd_object, oo_dt.do_lu);
}

static inline struct osd_device *osd_dt_dev(const struct dt_device *d)
{
	LASSERT(lu_device_is_osd(&d->dd_lu_dev));
	return container_of0(d, struct osd_device, od_dt_dev);
}

static inline struct osd_device *osd_dev(const struct lu_device *d)
{
	LASSERT(lu_device_is_osd(d));
	return osd_dt_dev(container_of0(d, struct dt_device, dd_lu_dev));
}

static inline struct osd_object *osd_dt_obj(const struct dt_object *d)
{
	return osd_obj(&d->do_lu);
}

static inline struct osd_device *osd_obj2dev(const struct osd_object *o)
{
	return osd_dev(o->oo_dt.do_lu.lo_dev);
}

static inline struct super_block *osd_sb(const struct osd_device *dev)
{
	return dev->od_mnt->mnt_sb;
}

static inline struct lu_device *osd2lu_dev(struct osd_device *osd)
{
	return &osd->od_dt_dev.dd_lu_dev;
}

#ifdef LIXI
static inline struct objset *osd_dtobj2objset(struct dt_object *o)
{
	return osd_dev(o->do_lu.lo_dev)->od_objset.os;
}

static inline int osd_invariant(const struct osd_object *obj)
{
	return 1;
}

static inline int osd_object_invariant(const struct lu_object *l)
{
	return osd_invariant(osd_obj(l));
}
#endif /* LIXI */

static inline struct seq_server_site *osd_seq_site(struct osd_device *osd)
{
	return osd->od_dt_dev.dd_lu_dev.ld_site->ld_seq_site;
}

static inline char *osd_name(struct osd_device *osd)
{
	return osd->od_dt_dev.dd_lu_dev.ld_obd->obd_name;
}

extern struct lu_context_key osd_key;

static inline struct osd_thread_info *osd_oti_get(const struct lu_env *env)
{
	return lu_context_key_get(&env->le_ctx, &osd_key);
}

#ifdef CONFIG_PROC_FS
/* osd_lproc.c */
extern struct lprocfs_vars lprocfs_osd_obd_vars[];
int osd_procfs_init(struct osd_device *osd, const char *name);
int osd_procfs_fini(struct osd_device *osd);
#endif

int
osd_oi_lookup(const struct lu_env *env, struct osd_device *osd,
	      const struct lu_fid *fid, struct osd_inode_id *id);
int
osd_fid_lookup(const struct lu_env *env, struct osd_object *obj,
	       const struct lu_fid *fid);
struct lu_object *osd_object_alloc(const struct lu_env *env,
				   const struct lu_object_header *hdr,
				   struct lu_device *d);
extern struct dt_index_operations osd_dir_ops;
int osd_index_try(const struct lu_env *env, struct dt_object *dt,
		  const struct dt_index_features *feat);
static inline void osd_id_gen(struct osd_inode_id *id, __u64 ino, __u64 gen)
{
	id->oii_ino = ino;
	id->oii_gen = gen;
}

void osd_get_name_from_fid(const struct lu_fid *fid, char *buf);

int osd_declare_xattr_set(const struct lu_env *env, struct dt_object *dt,
			  const struct lu_buf *buf, const char *name,
			  int fl, struct thandle *handle);
int osd_xattr_get(const struct lu_env *env, struct dt_object *dt,
		  struct lu_buf *buf, const char *name);
int osd_xattr_set(const struct lu_env *env, struct dt_object *dt,
		  const struct lu_buf *buf, const char *name, int fl,
		  struct thandle *handle);

int osd_oi_init(const struct lu_env *env, struct osd_device *o);
struct osd_obj_seq *osd_seq_load(struct osd_thread_info *info,
				 struct osd_device *osd, __u64 seq);
int osd_obj_map_init(const struct lu_env *env, struct osd_device *dev);
void osd_obj_map_fini(struct osd_device *dev);
int fid_is_on_ost(const struct lu_env *env, struct osd_device *osd,
		  const struct lu_fid *fid);
char *named_oid2name(const unsigned long oid);
int osd_get_idx_and_name(struct osd_thread_info *info, struct osd_device *osd,
			 const struct lu_fid *fid, struct dentry **dir,
			 char *buf, size_t buf_size);
#endif /* _OSD_INTERNAL_H */
