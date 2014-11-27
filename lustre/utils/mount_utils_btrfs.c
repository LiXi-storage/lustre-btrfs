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
 * lustre/utils/mount_utils_btrfs.c
 *
 * Author: Li Xi <lixi@ddn.com>
 */

/* This source file is compiled into both mkfs.lustre and tunefs.lustre */

#if HAVE_CONFIG_H
#  include "config.h"
#endif /* HAVE_CONFIG_H */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "mount_utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <mntent.h>
#include <glob.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/utsname.h>

#include <string.h>
#include <getopt.h>
#include <limits.h>
#include <ctype.h>

/* libcfs.h is not really needed here, but on SLES10/PPC, fs.h includes idr.h
 * which requires BITS_PER_LONG to be defined */
#include <libcfs/libcfs.h>
#ifndef BLKGETSIZE64
#include <linux/fs.h> /* for BLKGETSIZE64 */
#endif
#include <linux/version.h>
#include <lustre_disk.h>
#include <lustre_param.h>
#include <lnet/lnetctl.h>
#include <lustre_ver.h>

#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#endif

int btrfs_read_ldd(char *dev, struct lustre_disk_data *mo_ldd)
{
	return filesystem_read_ldd(dev, mo_ldd,
				   "/usr/sbin/btrfs filesystem label");
}

/* Write the server config files */
int btrfs_write_ldd(struct mkfs_opts *mop)
{
	return filesystem_write_ldd(mop);
}

/* Check whether the device has already been used with lustre */
int btrfs_is_lustre(char *dev, unsigned *mount_type)
{
	int ret;
	struct lustre_disk_data	mo_ldd;

	mo_ldd.ldd_mount_type = LDD_MT_BTRFS;
	mo_ldd.ldd_mount_opts[0] = '\0';
	ret = filesystem_have_file(MOUNT_DATA_FILE, dev, &mo_ldd);
	if (ret) {
		/* in the -1 case, 'extents' means IS a lustre target */
		*mount_type = LDD_MT_BTRFS;
		return 1;
	}

	ret = filesystem_have_file(LAST_RCVD, dev, &mo_ldd);
	if (ret) {
		*mount_type = LDD_MT_BTRFS;
		return 1;
	}

	return 0;
}

#define MKFS_BTRFS "mkfs.btrfs"

/* Build fs according to type */
int btrfs_make_lustre(struct mkfs_opts *mop)
{
	__u64 device_kb = mop->mo_device_kb;
	char mkfs_cmd[PATH_MAX];
	char *dev;
	int ret;

	if (!(mop->mo_flags & MO_IS_LOOP)) {
		mop->mo_device_kb = get_device_size(mop->mo_device);

		if (mop->mo_device_kb == 0)
			return ENODEV;

		/* Compare to real size */
		if (device_kb == 0 || device_kb > mop->mo_device_kb)
			device_kb = mop->mo_device_kb;
		else
			mop->mo_device_kb = device_kb;
	}

	/* For loop device format the dev, not the filename */
	dev = mop->mo_device;
	if (mop->mo_flags & MO_IS_LOOP)
		dev = mop->mo_loopdev;

	/* Allow reformat of an existing filesystem. */
	strscat(mop->mo_mkfsopts, " -f", sizeof(mop->mo_mkfsopts));

	vprint("formatting backing filesystem %s on %s\n",
	       MT_STR(&mop->mo_ldd), dev);
	vprint("\ttarget name  %s\n", mop->mo_ldd.ldd_svname);
	vprint("\toptions       %s\n", mop->mo_mkfsopts);

	snprintf(mkfs_cmd, sizeof(mkfs_cmd),
		 "%s -L %s ", MKFS_BTRFS, mop->mo_ldd.ldd_svname);
	strscat(mkfs_cmd, mop->mo_mkfsopts, sizeof(mkfs_cmd));
	strscat(mkfs_cmd, " ", sizeof(mkfs_cmd));
	strscat(mkfs_cmd, dev, sizeof(mkfs_cmd));

	vprint("mkfs_cmd = %s\n", mkfs_cmd);
	ret = run_command(mkfs_cmd, sizeof(mkfs_cmd));
	if (ret) {
		fatal();
		fprintf(stderr, "Unable to build fs %s (%d)\n", dev, ret);
	}
	return ret;
}

int btrfs_prepare_lustre(struct mkfs_opts *mop,
			 char *default_mountopts, int default_len,
			 char *always_mountopts, int always_len)
{
	int ret;

	/* Set MO_IS_LOOP to indicate a loopback device is needed */
	ret = is_block(mop->mo_device);
	if (ret < 0)
		return errno;
	else if (ret == 0)
		mop->mo_flags |= MO_IS_LOOP;

	return 0;
}

int btrfs_tune_lustre(char *dev, struct mount_opts *mop)
{
	fprintf(stderr, "%s: btrfs tune_lustre is not supported\n",
		progname);
	return -1;
}

int btrfs_label_lustre(struct mount_opts *mop)
{
	fprintf(stderr, "%s: btrfs label_lustre is not supported\n",
		progname);
	return -1;
}

/* Enable quota accounting */
int btrfs_enable_quota(struct mkfs_opts *mop)
{
	fprintf(stderr, "%s: btrfs enable_lustre is not supported\n",
		progname);
	return -1;
}

int btrfs_init(void)
{
	return 0;
}

void btrfs_fini(void)
{
	return;
}
