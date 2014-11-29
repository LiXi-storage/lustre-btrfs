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

#define DEBUG_SUBSYSTEM S_OSD

#include <linux/module.h>

/* LUSTRE_VERSION_CODE */
#include <lustre_ver.h>
/* prerequisite for linux/xattr.h */
#include <linux/types.h>
/* prerequisite for linux/xattr.h */
#include <linux/fs.h>
/* XATTR_{REPLACE,CREATE} */
#include <linux/xattr.h>

/*
 * struct OBD_{ALLOC,FREE}*()
 * OBD_FAIL_CHECK
 */
#include <obd_support.h>
/* struct ptlrpc_thread */
#include <lustre_net.h>
#include <lustre_fid.h>
/* process_config */
#include <lustre_param.h>

/* llo_* api support */
#include <md_object.h>
#include <lustre_quota.h>

/* class_process_* */
#include <obd_class.h>

/* lu_* api support */
#include <lu_object.h>

#include "osd_internal.h"

/*
 * Concurrency: shouldn't matter.
 */
static int osd_sync(const struct lu_env *env, struct dt_device *d)
{
	int rc;

	CDEBUG(D_CACHE, "syncing OSD %s\n", LUSTRE_OSD_BTRFS_NAME);
	/* LIXI TODO: add actual sync function */
	rc = 0;
	CDEBUG(D_CACHE, "synced OSD %s: rc = %d\n",
	       LUSTRE_OSD_BTRFS_NAME, rc);

	return rc;
}

static void osd_umount(const struct lu_env *env, struct osd_device *o)
{
	ENTRY;

	if (o->od_mnt != NULL) {
		shrink_dcache_sb(osd_sb(o));
		osd_sync(env, &o->od_dt_dev);

		mntput(o->od_mnt);
		o->od_mnt = NULL;
	}

	EXIT;
}

static int osd_mount(const struct lu_env *env,
		     struct osd_device *o, struct lustre_cfg *cfg)
{
	const char		*name  = lustre_cfg_string(cfg, 0);
	const char		*dev  = lustre_cfg_string(cfg, 1);
	const char              *opts;
	unsigned long            page, s_flags, lmd_flags = 0;
	struct page             *__page;
	struct file_system_type *type;
	char                    *options = NULL;
	char			*str;
	int			 option_size = PAGE_SIZE;
#ifdef LIXI
	struct osd_thread_info	*info = osd_oti_get(env);
	struct lu_fid		*fid = &info->oti_fid;
	struct inode		*inode;
#endif /* LIXI */
	int			 rc = 0;
	ENTRY;

	if (o->od_mnt != NULL)
		RETURN(0);

	if (strlen(dev) >= sizeof(o->od_mntdev))
		RETURN(-E2BIG);
	strncpy(o->od_mntdev, dev, sizeof(o->od_mntdev));

	__page = alloc_page(GFP_IOFS);
	if (__page == NULL)
		GOTO(out, rc = -ENOMEM);

	str = lustre_cfg_string(cfg, 2);
	s_flags = simple_strtoul(str, NULL, 0);
	str = strstr(str, ":");
	if (str)
		lmd_flags = simple_strtoul(str + 1, NULL, 0);
	opts = lustre_cfg_string(cfg, 3);
	page = (unsigned long)page_address(__page);
	options = (char *)page;
	*options = '\0';
	if (opts != NULL)
		strncat(options, opts, option_size);

	type = get_fs_type("btreefs");
	if (!type) {
		CERROR("%s: cannot find btrfs module\n", name);
		GOTO(out, rc = -ENODEV);
	}

	o->od_mnt = vfs_kern_mount(type, s_flags, dev, options);
	module_put(type->owner);

	if (IS_ERR(o->od_mnt)) {
		rc = PTR_ERR(o->od_mnt);
		o->od_mnt = NULL;
		CERROR("%s: can't mount %s: %d\n", name, dev, rc);
		GOTO(out, rc);
	}

#ifdef HAVE_DEV_SET_RDONLY
	if (dev_check_rdonly(o->od_mnt->mnt_sb->s_bdev)) {
		CERROR("%s: underlying device %s is marked as read-only. "
		       "Setup failed\n", name, dev);
		GOTO(out_mnt, rc = -EROFS);
	}
#endif

#ifdef LIXI
	inode = osd_sb(o)->s_root->d_inode;
	lu_local_obj_fid(fid, OSD_FS_ROOT_OID);
	rc = osd_ea_fid_set(info, inode, fid, LMAC_NOT_IN_OI, 0);
	if (rc != 0) {
		CERROR("%s: failed to set lma on %s root inode\n", name, dev);
		GOTO(out_mnt, rc);
	}

	if (lmd_flags & LMD_FLG_NOSCRUB)
		o->od_noscrub = 1;
#endif /* LIXI */

	GOTO(out, rc = 0);

#ifdef HAVE_DEV_SET_RDONLY /* LIXI */
out_mnt:
#endif /* LIXI */
	mntput(o->od_mnt);
	o->od_mnt = NULL;

out:
	if (__page)
		__free_page(__page);

	return rc;
}

static int osd_shutdown(const struct lu_env *env, struct osd_device *o)
{
	ENTRY;

#ifdef LIXI
	/* shutdown quota slave instance associated with the device */
	if (o->od_quota_slave != NULL) {
		qsd_fini(env, o->od_quota_slave);
		o->od_quota_slave = NULL;
	}

	osd_fid_fini(env, o);
#endif /* LIXI */

	RETURN(0);
}

/*
 * Concurrency: doesn't access mutable data
 */
static int osd_root_get(const struct lu_env *env,
                        struct dt_device *dev, struct lu_fid *f)
{
        lu_local_obj_fid(f, OSD_FS_ROOT_OID);
        return 0;
}

/*
 * Concurrency: doesn't access mutable data.
 */
static void osd_conf_get(const struct lu_env *env,
			 const struct dt_device *dev,
			 struct dt_device_param *param)
{
        struct super_block *sb = osd_sb(osd_dt_dev(dev));
#ifdef LIXI
	struct osd_device *osd = osd_dt_dev(dev);
#endif /* LIXI */

	/* TODO LIXI: configure reasonable numbers */
	/*
	 * XXX should be taken from not-yet-existing fs abstraction layer.
	 */
#ifdef LIXI
	param->ddp_max_name_len	= MAXNAMELEN;
#else /* LIXI */
	param->ddp_max_name_len	= 1024;
#endif /* LIXI */
	param->ddp_max_nlink	= 1 << 31; /* it's 8byte on a disk */
	param->ddp_mount_type	= LDD_MT_BTRFS;

#ifdef LIXI
	param->ddp_mntopts	= MNTOPT_USERXATTR;
	if (osd->od_posix_acl)
		param->ddp_mntopts |= MNTOPT_ACL;
	param->ddp_max_ea_size	= DXATTR_MAX_ENTRY_SIZE;
#else /* LIXI */
	param->ddp_mntopts	= 0;
	param->ddp_max_ea_size	= 1024;
#endif /* LIXI */

	/* for maxbytes, report same value as ZPL */
	param->ddp_maxbytes	= MAX_LFS_FILESIZE;

	/* inodes are dynamically allocated, so we report the per-inode space
	 * consumption to upper layers. This static value is not really accurate
	 * and we should use the same logic as in udmu_objset_statfs() to
	 * estimate the real size consumed by an object */
	param->ddp_inodespace = 1024;

	/* 256M (64k blocks) */
	param->ddp_max_extent_blks = 65536;
	param->ddp_extent_tax = 6 * sb->s_blocksize;
}

/*
 * Concurrency: shouldn't matter.
 */
int osd_statfs(const struct lu_env *env, struct dt_device *d,
               struct obd_statfs *sfs)
{
        struct osd_device  *osd = osd_dt_dev(d);
        struct super_block *sb = osd_sb(osd);
        struct kstatfs     *ksfs;
        int result = 0;

	if (unlikely(osd->od_mnt == NULL))
		return -EINPROGRESS;

        /* osd_lproc.c call this without env, allocate ksfs for that case */
        if (unlikely(env == NULL)) {
                OBD_ALLOC_PTR(ksfs);
                if (ksfs == NULL)
                        return -ENOMEM;
        } else {
                ksfs = &osd_oti_get(env)->oti_ksfs;
        }

	spin_lock(&osd->od_osfs_lock);
	result = sb->s_op->statfs(sb->s_root, ksfs);
	if (likely(result == 0)) { /* N.B. statfs can't really fail */
		statfs_pack(sfs, ksfs);
		if (sb->s_flags & MS_RDONLY)
			sfs->os_state = OS_STATE_READONLY;
	}

	spin_unlock(&osd->od_osfs_lock);

	if (unlikely(env == NULL))
                OBD_FREE_PTR(ksfs);

#ifdef LIXI
	/* Reserve a small amount of space for local objects like last_rcvd,
	 * llog, quota files, ... */
	if (sfs->os_bavail <= GRANT_FOR_LOCAL_OIDS) {
		sfs->os_bavail = 0;
	} else {
		sfs->os_bavail -= GRANT_FOR_LOCAL_OIDS;
		/** Take out metadata overhead for indirect blocks */
		sfs->os_bavail -= sfs->os_bavail >> (sb->s_blocksize_bits - 3);
	}
#endif /* LIXI */

        return result;
}

static const struct dt_device_operations osd_dt_ops = {
#ifdef LIXI
	.dt_root_get       = osd_root_get,
	.dt_statfs         = osd_statfs,
	.dt_trans_create   = osd_trans_create,
	.dt_trans_start    = osd_trans_start,
	.dt_trans_stop     = osd_trans_stop,
	.dt_trans_cb_add   = osd_trans_cb_add,
	.dt_conf_get       = osd_conf_get,
	.dt_sync           = osd_sync,
	.dt_ro             = osd_ro,
	.dt_commit_async   = osd_commit_async,
#else /* LIXI */
	.dt_root_get       = osd_root_get,
	.dt_statfs         = osd_statfs,
	.dt_trans_create   = NULL,
	.dt_trans_start    = NULL,
	.dt_trans_stop     = NULL,
	.dt_trans_cb_add   = NULL,
	.dt_conf_get       = osd_conf_get,
	.dt_sync           = NULL,
	.dt_ro             = NULL,
	.dt_commit_async   = NULL,
#endif /* LIXI */
};

static int osd_device_init0(const struct lu_env *env,
			    struct osd_device *o,
			    struct lustre_cfg *cfg)
{
	struct lu_device	*l = osd2lu_dev(o);
	struct osd_thread_info *info;
	int			rc;
	int			cplen = 0;

	/* if the module was re-loaded, env can loose its keys */
	rc = lu_env_refill((struct lu_env *) env);
	if (rc)
		GOTO(out, rc);
	info = osd_oti_get(env);
	LASSERT(info);

	l->ld_ops = &osd_lu_ops;
	o->od_dt_dev.dd_ops = &osd_dt_ops;

	spin_lock_init(&o->od_osfs_lock);
#ifdef LIXI
	mutex_init(&o->od_otable_mutex);
#endif /* LIXI */

#ifdef LIXI
	o->od_read_cache = 1;
	o->od_writethrough_cache = 1;
	o->od_readcache_max_filesize = OSD_MAX_CACHE_SIZE;
#endif /* LIXI */

	cplen = strlcpy(o->od_svname, lustre_cfg_string(cfg, 4),
			sizeof(o->od_svname));
	if (cplen >= sizeof(o->od_svname)) {
		rc = -E2BIG;
		GOTO(out, rc);
	}

	if (server_name_is_ost(o->od_svname))
		o->od_is_ost = 1;

	rc = osd_mount(env, o, cfg);
	if (rc != 0)
		GOTO(out, rc);

#ifdef LIXI
	rc = osd_obj_map_init(env, o);
	if (rc != 0)
		GOTO(out_mnt, rc);
#endif /* LIXI */

	rc = lu_site_init(&o->od_site, l);
	if (rc != 0)
#ifdef LIXI
		GOTO(out_compat, rc);
#else /* LIXI */
		GOTO(out_mnt, rc);
#endif /* LIXI */
	o->od_site.ls_bottom_dev = l;

	rc = lu_site_init_finish(&o->od_site);
	if (rc != 0)
		GOTO(out_site, rc);

#ifdef LIXI
	/* self-repair LMA by default */
	o->od_lma_self_repair = 1;

	INIT_LIST_HEAD(&o->od_ios_list);
	/* setup scrub, including OI files initialization */
	rc = osd_scrub_setup(env, o);
	if (rc < 0)
		GOTO(out_site, rc);
#endif /* LIXI */

	rc = osd_procfs_init(o, o->od_svname);
	if (rc != 0) {
		CERROR("%s: can't initialize procfs: rc = %d\n",
		       o->od_svname, rc);
#ifdef LIXI
		GOTO(out_scrub, rc);
#else /* LIXI */
		GOTO(out_site, rc);
#endif /* LIXI */
	}

	LASSERT(l->ld_site->ls_linkage.next != NULL);
	LASSERT(l->ld_site->ls_linkage.prev != NULL);

#ifdef LIXI
	/* initialize quota slave instance */
	o->od_quota_slave = qsd_init(env, o->od_svname, &o->od_dt_dev,
				     o->od_proc_entry);
	if (IS_ERR(o->od_quota_slave)) {
		rc = PTR_ERR(o->od_quota_slave);
		o->od_quota_slave = NULL;
		GOTO(out_procfs, rc);
	}
#endif /* LIXI */

	RETURN(0);

#ifdef LIXI
out_procfs:
	osd_procfs_fini(o);
out_scrub:
	osd_scrub_cleanup(env, o);
#endif /* LIXI */
out_site:
	lu_site_fini(&o->od_site);
#ifdef LIXI
out_compat:
	osd_obj_map_fini(o);
#endif /* LIXI */
out_mnt:
	osd_umount(env, o);
out:
	return rc;
}

static struct lu_device *osd_device_alloc(const struct lu_env *env,
					  struct lu_device_type *t,
					  struct lustre_cfg *cfg)
{
	struct osd_device *o;
	int                rc;

	OBD_ALLOC_PTR(o);
	if (o == NULL)
		return ERR_PTR(-ENOMEM);

	rc = dt_device_init(&o->od_dt_dev, t);
	if (rc == 0) {
		/* Because the ctx might be revived in dt_device_init,
		 * refill the env here */
		lu_env_refill((struct lu_env *)env);
		rc = osd_device_init0(env, o, cfg);
		if (rc)
			dt_device_fini(&o->od_dt_dev);
	}

	if (unlikely(rc != 0))
		OBD_FREE_PTR(o);

	return rc == 0 ? osd2lu_dev(o) : ERR_PTR(rc);
}

static struct lu_device *osd_device_free(const struct lu_env *env,
					 struct lu_device *d)
{
	struct osd_device *o = osd_dev(d);
	ENTRY;

	/* XXX: make osd top device in order to release reference */
	d->ld_site->ls_top_dev = d;
	lu_site_purge(env, d->ld_site, -1);
	if (!cfs_hash_is_empty(d->ld_site->ls_obj_hash)) {
		LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, D_ERROR, NULL);
		lu_site_print(env, d->ld_site, &msgdata, lu_cdebug_printer);
	}
	lu_site_fini(&o->od_site);
	dt_device_fini(&o->od_dt_dev);
	OBD_FREE_PTR(o);
	RETURN(NULL);
}

/*
 * To be removed, setup is performed by osd_device_{init,alloc} and
 * cleanup is performed by osd_device_{fini,free).
 */
static int osd_process_config(const struct lu_env *env,
			      struct lu_device *d, struct lustre_cfg *cfg)
{
	struct osd_device		*o = osd_dev(d);
	int				rc;
	ENTRY;

	switch (cfg->lcfg_command) {
	case LCFG_SETUP:
		rc = osd_mount(env, o, cfg);
		break;
	case LCFG_CLEANUP:
		lu_dev_del_linkage(d->ld_site, d);
		rc = osd_shutdown(env, o);
		break;
	case LCFG_PARAM:
		LASSERT(&o->od_dt_dev);
		rc = class_process_proc_param(PARAM_OSD, lprocfs_osd_obd_vars,
					      cfg, &o->od_dt_dev);
		if (rc > 0 || rc == -ENOSYS)
			rc = class_process_proc_param(PARAM_OST,
						      lprocfs_osd_obd_vars,
						      cfg, &o->od_dt_dev);
		break;
	default:
		rc = -ENOSYS;
	}

	RETURN(rc);
}

static int osd_recovery_complete(const struct lu_env *env, struct lu_device *d)
{
#ifdef LIXI
	struct osd_device	*osd = osd_dev(d);
	int			 rc = 0;
	ENTRY;

	if (osd->od_quota_slave == NULL)
		RETURN(0);

	/* start qsd instance on recovery completion, this notifies the quota
	 * slave code that we are about to process new requests now */
	rc = qsd_start(env, osd->od_quota_slave);
	RETURN(rc);
#else /* LIXI */
	ENTRY;
	RETURN(0);
#endif /* LIXI */
}

/*
 * we use exports to track all osd users
 */
static int osd_obd_connect(const struct lu_env *env, struct obd_export **exp,
			   struct obd_device *obd, struct obd_uuid *cluuid,
			   struct obd_connect_data *data, void *localdata)
{
	struct osd_device    *osd = osd_dev(obd->obd_lu_dev);
	struct lustre_handle  conn;
	int                   rc;
	ENTRY;

	CDEBUG(D_CONFIG, "connect #%d\n", osd->od_connects);

	rc = class_connect(&conn, obd, cluuid);
	if (rc)
		RETURN(rc);

	*exp = class_conn2export(&conn);

	spin_lock(&osd->od_osfs_lock);
	osd->od_connects++;
	spin_unlock(&osd->od_osfs_lock);

	RETURN(0);
}

/*
 * once last export (we don't count self-export) disappeared
 * osd can be released
 */
static int osd_obd_disconnect(struct obd_export *exp)
{
	struct obd_device *obd = exp->exp_obd;
	struct osd_device *osd = osd_dev(obd->obd_lu_dev);
	int                rc, release = 0;
	ENTRY;

	/* Only disconnect the underlying layers on the final disconnect. */
	spin_lock(&osd->od_osfs_lock);
	osd->od_connects--;
	if (osd->od_connects == 0)
		release = 1;
	spin_unlock(&osd->od_osfs_lock);

	rc = class_disconnect(exp); /* bz 9811 */

	if (rc == 0 && release)
		class_manual_cleanup(obd);
	RETURN(rc);
}

static int osd_prepare(const struct lu_env *env, struct lu_device *pdev,
		       struct lu_device *dev)
{
#ifdef LIXI
	struct osd_device	*osd = osd_dev(dev);
	int			 rc = 0;
	ENTRY;

	if (osd->od_quota_slave != NULL)
		/* set up quota slave objects */
		rc = qsd_prepare(env, osd->od_quota_slave);

	RETURN(rc);
#else /* LIXI */
	ENTRY;
	RETURN(0);
#endif /* LIXI */
}

/*
 * Btrfs OSD device type methods
 */
static int osd_type_init(struct lu_device_type *t)
{
	LU_CONTEXT_KEY_INIT(&osd_key);
	return lu_context_key_register(&osd_key);
}

static void osd_type_fini(struct lu_device_type *t)
{
	lu_context_key_degister(&osd_key);
}

static void osd_type_start(struct lu_device_type *t)
{
}

static void osd_type_stop(struct lu_device_type *t)
{
}

static void *osd_key_init(const struct lu_context *ctx,
			  struct lu_context_key *key)
{
	struct osd_thread_info *info;

	OBD_ALLOC_PTR(info);
	if (info != NULL)
		info->oti_env = container_of(ctx, struct lu_env, le_ctx);
	else
		info = ERR_PTR(-ENOMEM);
	return info;
}

static void osd_key_fini(const struct lu_context *ctx,
			 struct lu_context_key *key, void *data)
{
	struct osd_thread_info *info = data;

	OBD_FREE_PTR(info);
}

static void osd_key_exit(const struct lu_context *ctx,
			 struct lu_context_key *key, void *data)
{
	struct osd_thread_info *info = data;

	memset(info, 0, sizeof(*info));
}

struct lu_context_key osd_key = {
	.lct_tags = LCT_DT_THREAD | LCT_MD_THREAD | LCT_MG_THREAD | LCT_LOCAL,
	.lct_init = osd_key_init,
	.lct_fini = osd_key_fini,
	.lct_exit = osd_key_exit
};

static int osd_device_init(const struct lu_env *env, struct lu_device *d,
			   const char *name, struct lu_device *next)
{
	struct osd_device *osd = osd_dev(d);

	if (strlcpy(osd->od_svname, name, sizeof(osd->od_svname))
	    >= sizeof(osd->od_svname))
		return -E2BIG;
	return osd_procfs_init(osd, name);
}

static struct lu_device *osd_device_fini(const struct lu_env *env,
					 struct lu_device *d)
{
	struct osd_device *o = osd_dev(d);
	ENTRY;

	osd_shutdown(env, o);
	osd_procfs_fini(o);
#ifdef LIXI
	osd_scrub_cleanup(env, o);
	osd_obj_map_fini(o);
#endif /* LIXI */
	osd_umount(env, o);

	RETURN(NULL);
}

const struct lu_device_operations osd_lu_ops = {
#ifdef LIXI
	.ldo_object_alloc      = osd_object_alloc,
#else /* LIXI */
	.ldo_object_alloc      = NULL,
#endif /* LIXI */
	.ldo_process_config    = osd_process_config,
	.ldo_recovery_complete = osd_recovery_complete,
	.ldo_prepare           = osd_prepare,
};

static const struct lu_device_type_operations osd_device_type_ops = {
	.ldto_init = osd_type_init,
	.ldto_fini = osd_type_fini,

	.ldto_start = osd_type_start,
	.ldto_stop  = osd_type_stop,

	.ldto_device_alloc = osd_device_alloc,
	.ldto_device_free  = osd_device_free,

	.ldto_device_init    = osd_device_init,
	.ldto_device_fini    = osd_device_fini
};

struct lu_device_type osd_device_type = {
	.ldt_tags     = LU_DEVICE_DT,
	.ldt_name     = LUSTRE_OSD_BTRFS_NAME,
	.ldt_ops      = &osd_device_type_ops,
	.ldt_ctx_tags = LCT_LOCAL,
};

static struct obd_ops osd_obd_device_ops = {
	.o_owner       = THIS_MODULE,
	.o_connect	= osd_obd_connect,
	.o_disconnect	= osd_obd_disconnect,
#ifdef LIXI
	.o_fid_alloc	= osd_fid_alloc
#endif /* LIXI */
};

static int __init osd_mod_init(void)
{
	int rc;

	rc = class_register_type(&osd_obd_device_ops, NULL, true, NULL,
				 LUSTRE_OSD_BTRFS_NAME, &osd_device_type);
	return 0;
}

static void __exit osd_mod_exit(void)
{
	class_unregister_type(LUSTRE_OSD_BTRFS_NAME);
}

MODULE_AUTHOR("DataDirect Networks, Inc. <http://www.ddn.com/>");
MODULE_DESCRIPTION("Lustre Object Storage Device ("LUSTRE_OSD_BTRFS_NAME")");
MODULE_LICENSE("GPL");
MODULE_VERSION(LUSTRE_VERSION_STRING);

module_init(osd_mod_init);
module_exit(osd_mod_exit);
