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
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, 2014, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#if HAVE_CONFIG_H
#  include "config.h"
#endif /* HAVE_CONFIG_H */

#include "mount_utils.h"
#include <mntent.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <config.h>
#include <lustre_disk.h>
#include <lustre_ver.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <linux/loop.h>
#include <dlfcn.h>
#include <libcfs/util/string.h>

#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#include <selinux/get_context_list.h>
#endif

extern char *progname;
extern int verbose;

#define vprint(fmt, arg...) if (verbose > 0) printf(fmt, ##arg)
#define verrprint(fmt, arg...) if (verbose >= 0) fprintf(stderr, fmt, ##arg)

static struct module_backfs_ops *backfs_ops[LDD_MT_LAST];

void fatal(void)
{
        verbose = 0;
        fprintf(stderr, "\n%s FATAL: ", progname);
}

int run_command(char *cmd, int cmdsz)
{
        char log[] = "/tmp/run_command_logXXXXXX";
        int fd = -1, rc;

        if ((cmdsz - strlen(cmd)) < 6) {
                fatal();
                fprintf(stderr, "Command buffer overflow: %.*s...\n",
                        cmdsz, cmd);
                return ENOMEM;
        }

        if (verbose > 1) {
                printf("cmd: %s\n", cmd);
        } else {
                if ((fd = mkstemp(log)) >= 0) {
                        close(fd);
                        strcat(cmd, " >");
                        strcat(cmd, log);
                }
        }
        strcat(cmd, " 2>&1");

        /* Can't use popen because we need the rv of the command */
        rc = system(cmd);
        if (rc && (fd >= 0)) {
                char buf[128];
                FILE *fp;
                fp = fopen(log, "r");
                if (fp) {
                        while (fgets(buf, sizeof(buf), fp) != NULL) {
                                printf("   %s", buf);
                        }
                        fclose(fp);
                }
        }
        if (fd >= 0)
                remove(log);
        return rc;
}

int add_param(char *buf, char *key, char *val)
{
	int end = sizeof(((struct lustre_disk_data *)0)->ldd_params);
	int start = strlen(buf);
	int keylen = 0;

	if (key)
		keylen = strlen(key);
	if (start + 1 + keylen + strlen(val) >= end) {
		fprintf(stderr, "%s: params are too long-\n%s %s%s\n",
			progname, buf, key ? key : "", val);
		return 1;
	}

	sprintf(buf + start, " %s%s", key ? key : "", val);
	return 0;
}

int get_param(char *buf, char *key, char **val)
{
	int i, key_len = strlen(key);
	char *ptr;

	ptr = strstr(buf, key);
	if (ptr) {
		*val = strdup(ptr + key_len);
		if (*val == NULL)
			return ENOMEM;

		for (i = 0; i < strlen(*val); i++)
			if (((*val)[i] == ' ') || ((*val)[i] == '\0'))
				break;

		(*val)[i] = '\0';
		return 0;
	}

	return ENOENT;
}

int append_param(char *buf, char *key, char *val, char sep)
{
	int key_len, i, offset, old_val_len;
	char *ptr = NULL, str[1024];

	if (key)
		ptr = strstr(buf, key);

	/* key doesn't exist yet, so just add it */
	if (ptr == NULL)
		return add_param(buf, key, val);

	key_len = strlen(key);

	/* Copy previous values to str */
	for (i = 0; i < sizeof(str); ++i) {
		if ((ptr[i+key_len] == ' ') || (ptr[i+key_len] == '\0'))
			break;
		str[i] = ptr[i+key_len];
	}
	if (i == sizeof(str))
		return E2BIG;
	old_val_len = i;

	offset = old_val_len+key_len;

	/* Move rest of buf to overwrite previous key and value */
	for (i = 0; ptr[i+offset] != '\0'; ++i)
		ptr[i] = ptr[i+offset];

	ptr[i] = '\0';

	snprintf(str+old_val_len, sizeof(str)-old_val_len, "%c%s", sep, val);

	return add_param(buf, key, str);
}

char *strscat(char *dst, char *src, int buflen)
{
	dst[buflen - 1] = 0;
	if (strlen(dst) + strlen(src) >= buflen) {
		fprintf(stderr, "string buffer overflow (max %d): '%s' + '%s'"
			"\n", buflen, dst, src);
		exit(EOVERFLOW);
	}
	return strcat(dst, src);
}

char *strscpy(char *dst, char *src, int buflen)
{
	dst[0] = 0;
	return strscat(dst, src, buflen);
}

int check_mtab_entry(char *spec1, char *spec2, char *mtpt, char *type)
{
	FILE *fp;
	struct mntent *mnt;

	fp = setmntent(MOUNTED, "r");
	if (fp == NULL)
		return 0;

	while ((mnt = getmntent(fp)) != NULL) {
		if ((strcmp(mnt->mnt_fsname, spec1) == 0 ||
		     strcmp(mnt->mnt_fsname, spec2) == 0) &&
		    (mtpt == NULL || strcmp(mnt->mnt_dir, mtpt) == 0) &&
		    (type == NULL || strcmp(mnt->mnt_type, type) == 0)) {
			endmntent(fp);
			return(EEXIST);
		}
	}
	endmntent(fp);

	return 0;
}

#define PROC_DIR	"/proc/"
static int mtab_is_proc(const char *mtab)
{
	char path[16];

	if (readlink(mtab, path, sizeof(path)) < 0)
		return 0;

	if (strncmp(path, PROC_DIR, strlen(PROC_DIR)))
		return 0;

	return 1;
}

int update_mtab_entry(char *spec, char *mtpt, char *type, char *opts,
		int flags, int freq, int pass)
{
	FILE *fp;
	struct mntent mnt;
	int rc = 0;

	/* Don't update mtab if it is linked to any file in /proc direcotry.*/
	if (mtab_is_proc(MOUNTED))
		return 0;

	mnt.mnt_fsname = spec;
	mnt.mnt_dir = mtpt;
	mnt.mnt_type = type;
	mnt.mnt_opts = opts ? opts : "";
	mnt.mnt_freq = freq;
	mnt.mnt_passno = pass;

	fp = setmntent(MOUNTED, "a+");
	if (fp == NULL) {
		fprintf(stderr, "%s: setmntent(%s): %s:",
			progname, MOUNTED, strerror (errno));
		rc = 16;
	} else {
		if ((addmntent(fp, &mnt)) == 1) {
			fprintf(stderr, "%s: addmntent: %s:",
				progname, strerror (errno));
			rc = 16;
		}
		endmntent(fp);
	}

	return rc;
}

/* Search for opt in mntlist, returning true if found.
 */
static int in_mntlist(char *opt, char *mntlist)
{
	char *ml, *mlp, *item, *ctx = NULL;

	if (!(ml = strdup(mntlist))) {
		fprintf(stderr, "%s: out of memory\n", progname);
		exit(1);
	}
	mlp = ml;
	while ((item = strtok_r(mlp, ",", &ctx))) {
		if (!strcmp(opt, item))
			break;
		mlp = NULL;
	}
	free(ml);
	return (item != NULL);
}

/* Issue a message on stderr for every item in wanted_mountopts that is not
 * present in mountopts.  The justwarn boolean toggles between error and
 * warning message.  Return an error count.
 */
int check_mountfsoptions(char *mountopts, char *wanted_mountopts)
{
	char *ml, *mlp, *item, *ctx = NULL;
	int errors = 0;

	if (!(ml = strdup(wanted_mountopts))) {
		fprintf(stderr, "%s: out of memory\n", progname);
		exit(1);
	}
	mlp = ml;
	while ((item = strtok_r(mlp, ",", &ctx))) {
		if (!in_mntlist(item, mountopts)) {
			fprintf(stderr, "%s: Error: mandatory mount option"
				" '%s' is missing\n", progname, item);
			errors++;
		}
		mlp = NULL;
	}
	free(ml);
	return errors;
}

/* Trim embedded white space, leading and trailing commas from string s.
 */
void trim_mountfsoptions(char *s)
{
	char *p;

	for (p = s; *p; ) {
		if (isspace(*p)) {
			memmove(p, p + 1, strlen(p + 1) + 1);
			continue;
		}
		p++;
	}

	while (s[0] == ',')
		memmove(&s[0], &s[1], strlen(&s[1]) + 1);

	p = s + strlen(s) - 1;
	while (p >= s && *p == ',')
		*p-- = '\0';
}

/* Setup a file in the first unused loop_device */
int loop_setup(struct mkfs_opts *mop)
{
	char loop_base[20];
	char l_device[64];
	int i, ret = 0;

	/* Figure out the loop device names */
	if (!access("/dev/loop0", F_OK | R_OK) ||
	    !access("/dev/loop-control", F_OK | R_OK)) {
		strcpy(loop_base, "/dev/loop\0");
	} else if (!access("/dev/loop/0", F_OK | R_OK)) {
		strcpy(loop_base, "/dev/loop/\0");
	} else {
		fprintf(stderr, "%s: can't access loop devices\n", progname);
		return EACCES;
	}

	/* Find unused loop device */
	for (i = 0; i < MAX_LOOP_DEVICES; i++) {
		char cmd[PATH_MAX];
		int cmdsz = sizeof(cmd);

#ifdef HAVE_LOOP_CTL_GET_FREE
		ret = open("/dev/loop-control", O_RDWR);
		if (ret < 0) {
			fprintf(stderr, "%s: can't access loop control\n", progname);
			return EACCES;
		}
		/* find or allocate a free loop device to use */
		i = ioctl(ret, LOOP_CTL_GET_FREE);
		if (i < 0) {
			fprintf(stderr, "%s: access loop control error\n", progname);
			return EACCES;
		}
		sprintf(l_device, "%s%d", loop_base, i);
#else
		sprintf(l_device, "%s%d", loop_base, i);
		if (access(l_device, F_OK | R_OK))
			break;
#endif
		snprintf(cmd, cmdsz, "losetup %s > /dev/null 2>&1", l_device);
		ret = system(cmd);

		/* losetup gets 1 (ret=256) for non-set-up device */
		if (ret) {
			/* Set up a loopback device to our file */
			snprintf(cmd, cmdsz, "losetup %s %s", l_device,
				 mop->mo_device);
			ret = run_command(cmd, cmdsz);
			if (ret == 256)
				/* someone else picked up this loop device
				 * behind our back */
				continue;
			if (ret) {
				fprintf(stderr, "%s: error %d on losetup: %s\n",
					progname, ret,
					ret >= 0 ? strerror(ret) : "");
				return ret;
			}
			strscpy(mop->mo_loopdev, l_device,
				sizeof(mop->mo_loopdev));
			return ret;
		}
	}

	fprintf(stderr, "%s: out of loop devices!\n", progname);
	return EMFILE;
}

int loop_cleanup(struct mkfs_opts *mop)
{
	char cmd[150];
	int ret = 0;

	if ((mop->mo_flags & MO_IS_LOOP) && *mop->mo_loopdev) {
		int tries;

		sprintf(cmd, "losetup -d %s", mop->mo_loopdev);
		for (tries = 0; tries < 3; tries++) {
			ret = run_command(cmd, sizeof(cmd));
			if (ret == 0)
				break;
			sleep(1);
		}
	}

	if (ret != 0)
		fprintf(stderr, "cannot cleanup %s: rc = %d\n",
			mop->mo_loopdev, ret);
	return ret;
}

int loop_format(struct mkfs_opts *mop)
{
	int fd;

	if (mop->mo_device_kb == 0) {
		fatal();
		fprintf(stderr, "loop device requires a --device-size= "
			"param\n");
		return EINVAL;
	}

	fd = creat(mop->mo_device, S_IRUSR|S_IWUSR);
	if (fd < 0) {
		fatal();
		fprintf(stderr, "%s: Unable to create backing store: %s\n",
			progname, strerror(errno));
		return errno;
	}

	if (ftruncate(fd, mop->mo_device_kb * 1024) != 0) {
		close(fd);
		fatal();
		fprintf(stderr, "%s: Unable to truncate backing store: %s\n",
			progname, strerror(errno));
		return errno;
	}

	close(fd);
	return 0;
}

#define DLSYM(prefix, sym, func)					\
	do {								\
		char _fname[64];					\
		snprintf(_fname, sizeof(_fname), "%s_%s", prefix, #func); \
		sym->func = (typeof(sym->func))dlsym(sym->dl_handle, _fname); \
	} while (0)

/**
 * Load plugin for a given mount_type from ${pkglibdir}/mount_osd_FSTYPE.so and
 * return struct of function pointers (will be freed in unloack_backfs_module).
 *
 * \param[in] mount_type	Mount type to load module for.
 * \retval Value of backfs_ops struct
 * \retval NULL if no module exists
 */
struct module_backfs_ops *load_backfs_module(enum ldd_mount_type mount_type)
{
	void *handle;
	char *error, filename[512], fsname[512], *name;
	struct module_backfs_ops *ops;

	/* This deals with duplicate ldd_mount_types resolving to same OSD layer
	 * plugin (e.g. ext3/ldiskfs/ldiskfs2 all being ldiskfs) */
	strncpy(fsname, mt_type(mount_type), sizeof(fsname));
	name = fsname + sizeof("osd-") - 1;

	/* change osd- to osd_ */
	fsname[sizeof("osd-") - 2] = '_';

	snprintf(filename, sizeof(filename), PLUGIN_DIR"/mount_%s.so", fsname);

	handle = dlopen(filename, RTLD_LAZY);

	/* Check for $LUSTRE environment variable from test-framework.
	 * This allows using locally built modules to be used.
	 */
	if (handle == NULL) {
		char *dirname;
		dirname = getenv("LUSTRE");
		if (dirname) {
			snprintf(filename, sizeof(filename),
				 "%s/utils/.libs/mount_%s.so",
				 dirname, fsname);
			handle = dlopen(filename, RTLD_LAZY);
		}
	}

	/* Do not clutter up console with missing types */
	if (handle == NULL)
		return NULL;

	ops = malloc(sizeof(*ops));
	if (ops == NULL) {
		dlclose(handle);
		return NULL;
	}

	ops->dl_handle = handle;
	dlerror(); /* Clear any existing error */

	DLSYM(name, ops, init);
	DLSYM(name, ops, fini);
	DLSYM(name, ops, read_ldd);
	DLSYM(name, ops, write_ldd);
	DLSYM(name, ops, is_lustre);
	DLSYM(name, ops, make_lustre);
	DLSYM(name, ops, prepare_lustre);
	DLSYM(name, ops, tune_lustre);
	DLSYM(name, ops, label_lustre);
	DLSYM(name, ops, enable_quota);

	error = dlerror();
	if (error != NULL) {
		fatal();
		fprintf(stderr, "%s\n", error);
		dlclose(handle);
		free(ops);
		return NULL;
	}

	/* optional methods */
	DLSYM(name, ops, fix_mountopts);

	return ops;
}

/**
 * Unload plugin and free backfs_ops structure. Must be called the same number
 * of times as load_backfs_module is.
 */
void unload_backfs_module(struct module_backfs_ops *ops)
{
	if (ops == NULL)
		return;

	dlclose(ops->dl_handle);
	free(ops);
}

/* Return true if backfs_ops has operations for the given mount_type. */
int backfs_mount_type_okay(enum ldd_mount_type mount_type)
{
	if (unlikely(mount_type >= LDD_MT_LAST || mount_type < 0)) {
		fatal();
		fprintf(stderr, "fs type out of range %d\n", mount_type);
		return 0;
	}
	if (backfs_ops[mount_type] == NULL) {
		fatal();
		fprintf(stderr, "unhandled/unloaded fs type %d '%s'\n",
			mount_type, mt_str(mount_type));
		return 0;
	}
	return 1;
}

/* Write the server config files */
int osd_write_ldd(struct mkfs_opts *mop)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	int ret;

	if (backfs_mount_type_okay(ldd->ldd_mount_type))
		ret = backfs_ops[ldd->ldd_mount_type]->write_ldd(mop);

	else
		ret = EINVAL;

	return ret;
}

/* Read the server config files */
int osd_read_ldd(char *dev, struct lustre_disk_data *ldd)
{
	int ret;

	if (backfs_mount_type_okay(ldd->ldd_mount_type))
		ret = backfs_ops[ldd->ldd_mount_type]->read_ldd(dev, ldd);

	else
		ret = EINVAL;

	return ret;
}

/* Was this device formatted for Lustre */
int osd_is_lustre(char *dev, unsigned *mount_type)
{
	int i;

	vprint("checking for existing Lustre data: ");

	for (i = 0; i < LDD_MT_LAST; ++i) {
		if (backfs_ops[i] != NULL &&
		    backfs_ops[i]->is_lustre(dev, mount_type)) {
			vprint("found\n");
			return 1;
		}
	}

	vprint("not found\n");
	return 0;
}

/* Build fs according to type */
int osd_make_lustre(struct mkfs_opts *mop)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	int ret;

	if (backfs_mount_type_okay(ldd->ldd_mount_type))
		ret = backfs_ops[ldd->ldd_mount_type]->make_lustre(mop);

	else
		ret = EINVAL;

	return ret;
}

int osd_prepare_lustre(struct mkfs_opts *mop,
		       char *wanted_mountopts, size_t len)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	int ret;

	if (backfs_mount_type_okay(ldd->ldd_mount_type))
		ret = backfs_ops[ldd->ldd_mount_type]->prepare_lustre(mop,
							wanted_mountopts, len);

	else
		ret = EINVAL;

	return ret;
}

int osd_fix_mountopts(struct mkfs_opts *mop, char *mountopts, size_t len)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;

	if (!backfs_mount_type_okay(ldd->ldd_mount_type))
		return EINVAL;

	if (backfs_ops[ldd->ldd_mount_type]->fix_mountopts == NULL)
		return 0;

	return backfs_ops[ldd->ldd_mount_type]->fix_mountopts(mop, mountopts,
							      len);
}

int osd_tune_lustre(char *dev, struct mount_opts *mop)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	int ret;

	if (backfs_mount_type_okay(ldd->ldd_mount_type))
		ret = backfs_ops[ldd->ldd_mount_type]->tune_lustre(dev, mop);

	else
		ret = EINVAL;

	return ret;
}

int osd_label_lustre(struct mount_opts *mop)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	int ret;

	if (backfs_mount_type_okay(ldd->ldd_mount_type))
		ret = backfs_ops[ldd->ldd_mount_type]->label_lustre(mop);

	else
		ret = EINVAL;

	return ret;
}

/* Enable quota accounting */
int osd_enable_quota(struct mkfs_opts *mop)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	int ret;

	if (backfs_mount_type_okay(ldd->ldd_mount_type))
		ret = backfs_ops[ldd->ldd_mount_type]->enable_quota(mop);

	else
		ret = EINVAL;

	return ret;
}

int osd_init(void)
{
	int i, rc, ret = EINVAL;

	for (i = 0; i < LDD_MT_LAST; ++i) {
		rc = 0;
		backfs_ops[i] = load_backfs_module(i);
		if (backfs_ops[i] != NULL)
			rc = backfs_ops[i]->init();
		if (rc != 0) {
			backfs_ops[i]->fini();
			unload_backfs_module(backfs_ops[i]);
			backfs_ops[i] = NULL;
		} else
			ret = 0;
	}

	return ret;
}

void osd_fini(void)
{
	int i;

	for (i = 0; i < LDD_MT_LAST; ++i) {
		if (backfs_ops[i] != NULL) {
			backfs_ops[i]->fini();
			unload_backfs_module(backfs_ops[i]);
			backfs_ops[i] = NULL;
		}
	}
}

__u64 get_device_size(char* device)
{
	int ret, fd;
	__u64 size = 0;

	fd = open(device, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%s: cannot open %s: %s\n",
			progname, device, strerror(errno));
		return 0;
	}

#ifdef BLKGETSIZE64
	/* size in bytes. bz5831 */
	ret = ioctl(fd, BLKGETSIZE64, (void*)&size);
#else
	{
		__u32 lsize = 0;
		/* size in blocks */
		ret = ioctl(fd, BLKGETSIZE, (void*)&lsize);
		size = (__u64)lsize * 512;
	}
#endif
	close(fd);
	if (ret < 0) {
		fprintf(stderr, "%s: size ioctl failed: %s\n",
			progname, strerror(errno));
		return 0;
	}

	vprint("device size = "LPU64"MB\n", size >> 20);
	/* return value in KB */
	return size >> 10;
}

int file_create(char *path, __u64 size)
{
	__u64 size_max;
	int ret;
	int fd;

	/*
	 * Since "size" is in KB, the file offset it represents could overflow
	 * off_t.
	 */
	size_max = (off_t)1 << (_FILE_OFFSET_BITS - 1 - 10);
	if (size >= size_max) {
		fprintf(stderr, "%s: "LPU64" KB: Backing store size must be "
			"smaller than "LPU64" KB\n", progname, size, size_max);
		return EFBIG;
	}

	ret = access(path, F_OK);
	if (ret == 0) {
		ret = unlink(path);
		if (ret != 0)
			return errno;
	}

	fd = creat(path, S_IRUSR|S_IWUSR);
	if (fd < 0) {
		fatal();
		fprintf(stderr, "%s: Unable to create backing store: %s\n",
			progname, strerror(errno));
		return errno;
	}

	ret = ftruncate(fd, size * 1024);
	close(fd);
	if (ret != 0) {
		fatal();
		fprintf(stderr, "%s: Unable to truncate backing store: %s\n",
			progname, strerror(errno));
		return errno;
	}

	return 0;
}

/* return canonicalized absolute pathname, even if the target file does not
 * exist, unlike realpath */
static char *absolute_path(char *devname)
{
	char  buf[PATH_MAX + 1] = "";
	char *path;
	char *ptr;
	int len;

	path = malloc(sizeof(buf));
	if (path == NULL)
		return NULL;

	if (devname[0] != '/') {
		if (getcwd(buf, sizeof(buf) - 1) == NULL) {
			free(path);
			return NULL;
		}
		len = snprintf(path, sizeof(buf), "%s/%s", buf, devname);
		if (len >= sizeof(buf)) {
			free(path);
			return NULL;
		}
	} else {
		len = snprintf(path, sizeof(buf), "%s", devname);
		if (len >= sizeof(buf)) {
			free(path);
			return NULL;
		}
	}

	/* truncate filename before calling realpath */
	ptr = strrchr(path, '/');
	if (ptr == NULL) {
		free(path);
		return NULL;
	}
	*ptr = '\0';
	if (buf != realpath(path, buf)) {
		free(path);
		return NULL;
	}
	/* add the filename back */
	len = snprintf(path, PATH_MAX, "%s/%s", buf, ptr+1);
	if (len >= PATH_MAX) {
		free(path);
		return NULL;
	}
	return path;
}

/* Determine if a device is a block device (as opposed to a file) */
int is_block(char *devname)
{
	struct stat st;
	int	ret = 0;
	char	*devpath;

	devpath = absolute_path(devname);
	if (devpath == NULL) {
		fprintf(stderr, "%s: failed to resolve path to %s\n",
			progname, devname);
		return -1;
	}

	ret = access(devname, F_OK);
	if (ret != 0) {
		if (strncmp(devpath, "/dev/", 5) == 0) {
			/* nobody sane wants to create a loopback file under
			 * /dev. Let's just report the device doesn't exist */
			fprintf(stderr, "%s: %s apparently does not exist\n",
				progname, devpath);
			ret = -1;
			goto out;
		}
		ret = 0;
		goto out;
	}
	ret = stat(devpath, &st);
	if (ret != 0) {
		fprintf(stderr, "%s: cannot stat %s\n", progname, devpath);
		goto out;
	}
	ret = S_ISBLK(st.st_mode);
out:
	free(devpath);
	return ret;
}

/*
 * Concatenate context of the temporary mount point iff selinux is enabled
 */
#ifdef HAVE_SELINUX
static void append_context_for_mount(char *mntpt,
				     struct lustre_disk_data *mo_ldd)
{
	security_context_t fcontext;

	if (getfilecon(mntpt, &fcontext) < 0) {
		/* Continuing with default behaviour */
		fprintf(stderr, "%s: Get file context failed : %s\n",
			progname, strerror(errno));
		return;
	}

	if (fcontext != NULL) {
		strncat(mo_ldd->ldd_mount_opts, ",context=",
			sizeof(mo_ldd->ldd_mount_opts));
		strncat(mo_ldd->ldd_mount_opts, fcontext,
			sizeof(mo_ldd->ldd_mount_opts));
		freecon(fcontext);
	}
}
#endif


/* btrfs mutiple devices rely on this */
static void append_devices_for_mount(struct mkfs_opts *mop, char *mo_mountopts)
{
	if (mop->mo_pool_vdevs == NULL)
		return;

	if (strcmp(MT_STR(&mop->mo_ldd), "lbtrfs"))
		return;

	while (*mop->mo_pool_vdevs != NULL) {
		strscat(mo_mountopts, ",device=",
			sizeof(mo_mountopts));
		strscat(mo_mountopts, *mop->mo_pool_vdevs,
			sizeof(mo_mountopts));
		mop->mo_pool_vdevs++;
	}
}

/* Write the server config files */
int filesystem_write_ldd(struct mkfs_opts *mop)
{
	char mntpt[] = "/tmp/mntXXXXXX";
	char filepnm[128];
	char *dev;
	FILE *filep;
	int ret = 0;
	size_t num;
	char tmp_mountopts[4096] = {0};

	/* Mount this device temporarily in order to write these files */
	if (!mkdtemp(mntpt)) {
		fprintf(stderr, "%s: Can't create temp mount point %s: %s\n",
			progname, mntpt, strerror(errno));
		return errno;
	}

	/*
	 * Append file context to mount options if SE Linux is enabled
	 */
	#ifdef HAVE_SELINUX
	if (is_selinux_enabled() > 0)
		append_context_for_mount(mntpt, &mop->mo_ldd);
	#endif

	dev = mop->mo_device;
	if (mop->mo_flags & MO_IS_LOOP)
		dev = mop->mo_loopdev;

	if (mop->mo_mountopts)
		strlcpy(tmp_mountopts, mop->mo_mountopts, sizeof(tmp_mountopts));
	append_devices_for_mount(mop, tmp_mountopts);
	ret = mount(dev, mntpt, MT_STR(&mop->mo_ldd), 0, tmp_mountopts);
	if (ret) {
		fprintf(stderr, "%s: Unable to mount %s: %s\n",
			progname, dev, strerror(errno));
		ret = errno;
		if (errno == ENODEV) {
			fprintf(stderr, "Is the %s module available?\n",
				MT_STR(&mop->mo_ldd));
		}
		goto out_rmdir;
	}

	/* Set up initial directories */
	snprintf(filepnm, sizeof(filepnm), "%s/%s", mntpt, MOUNT_CONFIGS_DIR);
	ret = mkdir(filepnm, 0777);
	if ((ret != 0) && (errno != EEXIST)) {
		fprintf(stderr, "%s: Can't make configs dir %s (%s)\n",
			progname, filepnm, strerror(errno));
		goto out_umnt;
	} else if (errno == EEXIST) {
		ret = 0;
	}

	/* Save the persistent mount data into a file. Lustre must pre-read
	   this file to get the real mount options. */
	vprint("Writing %s\n", MOUNT_DATA_FILE);
	snprintf(filepnm, sizeof(filepnm), "%s/%s", mntpt, MOUNT_DATA_FILE);
	filep = fopen(filepnm, "w");
	if (!filep) {
		fprintf(stderr, "%s: Unable to create %s file: %s\n",
			progname, filepnm, strerror(errno));
		goto out_umnt;
	}
	num = fwrite(&mop->mo_ldd, sizeof(mop->mo_ldd), 1, filep);
	if (num < 1 && ferror(filep)) {
		fprintf(stderr, "%s: Unable to write to file (%s): %s\n",
			progname, filepnm, strerror(errno));
		fclose(filep);
		goto out_umnt;
	}
	fclose(filep);

out_umnt:
	umount(mntpt);
out_rmdir:
	rmdir(mntpt);
	return ret;
}

static int readcmd(char *cmd, char *buf, int len)
{
	FILE *fp;
	int red;

	fp = popen(cmd, "r");
	if (!fp)
		return errno;

	red = fread(buf, 1, len, fp);
	pclose(fp);

	/* strip trailing newline */
	if (buf[red - 1] == '\n')
		buf[red - 1] = '\0';

	return (red == 0) ? -ENOENT : 0;
}

int filesystem_read_ldd(char *dev, struct lustre_disk_data *mo_ldd,
			const char *label_command)
{
	char mntpt[] = "/tmp/mntXXXXXX";
	char filepnm[128];
	FILE *filep;
	int ret = 0;
	char cmd[PATH_MAX];

	/* Mount this device temporarily in order to write these files */
	if (!mkdtemp(mntpt)) {
		fprintf(stderr, "%s: Can't create temp mount point %s: %s\n",
			progname, mntpt, strerror(errno));
		return errno;
	}

	/*
	 * Append file context to mount options if SE Linux is enabled
	 */
	#ifdef HAVE_SELINUX
	if (is_selinux_enabled() > 0)
		append_context_for_mount(mntpt, mo_ldd);
	#endif

	ret = mount(dev, mntpt, MT_STR(mo_ldd), 0,
		    mo_ldd->ldd_mount_opts);
	if (ret) {
		fprintf(stderr, "%s: Unable to mount %s: %s\n",
			progname, dev, strerror(errno));
		ret = errno;
		if (errno == ENODEV) {
			fprintf(stderr, "Is the %s module available?\n",
				MT_STR(mo_ldd));
		}
		goto out_rmdir;
	}

	snprintf(filepnm, sizeof(filepnm), "%s/"MOUNT_DATA_FILE, mntpt);
	filep = fopen(filepnm, "r");
	if (filep) {
		size_t num_read;
		vprint("Reading %s\n", MOUNT_DATA_FILE);
		num_read = fread(mo_ldd, sizeof(*mo_ldd), 1, filep);
		if (num_read < 1 && ferror(filep)) {
			fprintf(stderr, "%s: Unable to read from file %s: %s\n",
				progname, filepnm, strerror(errno));
		}
		fclose(filep);
	}
	/*
	 * Getting label might need to mount file system.
	 * Umount it in advance.
	 */
	umount(mntpt);

	/* As long as we at least have the label, we're good to go */
	snprintf(cmd, sizeof(cmd), "%s %s", label_command, dev);
	ret = readcmd(cmd, mo_ldd->ldd_svname, sizeof(mo_ldd->ldd_svname) - 1);
	if (ret) {
		fprintf(stderr, "%s: Unable to get label from command '%s'\n",
			progname, cmd);
	}

out_rmdir:
	rmdir(mntpt);
	return ret;
}

/* Check whether the file exists in the device */
int filesystem_have_file(char *file_name, char *dev,
			 struct lustre_disk_data *mo_ldd)
{
	char mntpt[] = "/tmp/mntXXXXXX";
	char filepnm[128];
	int ret = 0;
	struct stat stat_buf;

	/* Mount this device temporarily in order to write these files */
	if (!mkdtemp(mntpt)) {
		fprintf(stderr, "%s: Can't create temp mount point %s: %s\n",
			progname, mntpt, strerror(errno));
		return errno;
	}

	/*
	 * Append file context to mount options if SE Linux is enabled
	 */
	#ifdef HAVE_SELINUX
	if (is_selinux_enabled() > 0)
		append_context_for_mount(mntpt, mo_ldd);
	#endif

	ret = mount(dev, mntpt, MT_STR(mo_ldd), 0,
		    mo_ldd->ldd_mount_opts);
	if (ret) {
		fprintf(stderr, "%s: Unable to mount %s: %s\n",
			progname, dev, strerror(errno));
		ret = errno;
		if (errno == ENODEV) {
			fprintf(stderr, "Is the %s module available?\n",
				MT_STR(mo_ldd));
		}
		goto out_rmdir;
	}

	/* Set up initial directories */
	snprintf(filepnm, sizeof(filepnm), "%s/%s", mntpt, file_name);
	ret = stat(filepnm, &stat_buf);
	if (ret)
		ret = 0;
	else
		ret = 1;

	umount(mntpt);
out_rmdir:
	rmdir(mntpt);
	return ret;
}
