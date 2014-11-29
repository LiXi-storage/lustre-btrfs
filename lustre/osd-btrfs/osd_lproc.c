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

#include <lprocfs_status.h>
#include <lustre/lustre_idl.h>
#include <obd_class.h>

#include "osd_internal.h"

#if CONFIG_PROC_FS
struct lprocfs_vars lprocfs_osd_obd_vars[] = {
};

int osd_procfs_init(struct osd_device *osd, const char *name)
{
	struct obd_type	*type;
	int		 rc = 0;
	ENTRY;

	if (osd->od_proc_entry)
		RETURN(0);

	/* at the moment there is no linkage between lu_type
	 * and obd_type, so we lookup obd_type this way */
	type = class_search_type(LUSTRE_OSD_BTRFS_NAME);

	LASSERT(name != NULL);
	LASSERT(type != NULL);

	/* Find the type procroot and add the proc entry for this device */
	osd->od_proc_entry = lprocfs_register(name, type->typ_procroot,
						  lprocfs_osd_obd_vars,
						  &osd->od_dt_dev);
	if (IS_ERR(osd->od_proc_entry)) {
		rc = PTR_ERR(osd->od_proc_entry);
		CERROR("Error %d setting up lprocfs for %s\n",
		       rc, name);
		osd->od_proc_entry = NULL;
		GOTO(out, rc);
	}

#ifdef LIXI
	rc = osd_stats_init(osd);
#endif /* LIXI */

	EXIT;
out:
	if (rc)
		osd_procfs_fini(osd);
	return rc;
}

int osd_procfs_fini(struct osd_device *osd)
{
#ifdef LIXI
	if (osd->od_stats)
		lprocfs_free_stats(&osd->od_stats);
#endif /* LIXI */

	if (osd->od_proc_entry)
		lprocfs_remove(&osd->od_proc_entry);
	RETURN(0);
}
#endif
