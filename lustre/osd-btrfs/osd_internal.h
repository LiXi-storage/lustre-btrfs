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

/*
 * osd device.
 */
struct osd_device {
	/* super-class */
	struct dt_device          od_dt_dev;
	/* information about underlying file system */
	struct vfsmount          *od_mnt;
	/* service name associated with the osd device */
	char                      od_svname[MAX_OBD_NAME];
	char                      od_mntdev[MAX_OBD_NAME];
	int			  od_connects;
	struct lu_site		  od_site;
	spinlock_t		  od_osfs_lock;
	struct hlist_head	 *od_capa_hash;
	unsigned int              od_is_ost:1;
	struct proc_dir_entry	 *od_proc_entry;
};

struct osd_thread_info {
	const struct lu_env	*oti_env;
	struct kstatfs		 oti_ksfs;
};

/*
 * Helpers.
 */
extern const struct lu_device_operations  osd_lu_ops;

static inline int lu_device_is_osd(const struct lu_device *d)
{
	return ergo(d != NULL && d->ld_ops != NULL, d->ld_ops == &osd_lu_ops);
}

#ifdef LIXI
static inline struct osd_object *osd_obj(const struct lu_object *o)
{
	LASSERT(lu_device_is_osd(o->lo_dev));
	return container_of0(o, struct osd_object, oo_dt.do_lu);
}
#endif /* LIXI */

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

#ifdef LIXI
static inline struct osd_object *osd_dt_obj(const struct dt_object *d)
{
	return osd_obj(&d->do_lu);
}

static inline struct osd_device *osd_obj2dev(const struct osd_object *o)
{
	return osd_dev(o->oo_dt.do_lu.lo_dev);
}
#endif /* LIXI */

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

static inline struct seq_server_site *osd_seq_site(struct osd_device *osd)
{
	return osd->od_dt_dev.dd_lu_dev.ld_site->ld_seq_site;
}

static inline char *osd_name(struct osd_device *osd)
{
	return osd->od_dt_dev.dd_lu_dev.ld_obd->obd_name;
}
#endif /* LIXI */

extern struct lu_context_key osd_key;

static inline struct osd_thread_info *osd_oti_get(const struct lu_env *env)
{
	return lu_context_key_get(&env->le_ctx, &osd_key);
}

#ifdef LPROCFS
/* osd_lproc.c */
extern struct lprocfs_seq_vars lprocfs_osd_obd_vars[];
int osd_procfs_init(struct osd_device *osd, const char *name);
int osd_procfs_fini(struct osd_device *osd);
#endif

#endif /* _OSD_INTERNAL_H */
