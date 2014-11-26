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
	fprintf(stderr, "%s: btrfs read ldd is not supported\n",
		progname);
	return -1;
}

/* Write the server config files */
int btrfs_write_ldd(struct mkfs_opts *mop)
{
	fprintf(stderr, "%s: btrfs write ldd is not supported\n",
		progname);
	return -1;
}

/* Check whether the device has already been used with lustre */
int btrfs_is_lustre(char *dev, unsigned *mount_type)
{
	fprintf(stderr, "%s: btrfs is_lustre is not supported\n",
		progname);
	return 0;
}

/* Build fs according to type */
int btrfs_make_lustre(struct mkfs_opts *mop)
{
	fprintf(stderr, "%s: btrfs make_lustre is not supported\n",
		progname);
	return -1;
}

int btrfs_prepare_lustre(struct mkfs_opts *mop,
			 char *default_mountopts, int default_len,
			 char *always_mountopts, int always_len)
{
	fprintf(stderr, "%s: btrfs prepare_lustre is not supported\n",
		progname);
	return -1;
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
