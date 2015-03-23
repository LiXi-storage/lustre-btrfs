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

#include <linux/quotaops.h>
#include <libcfs/libcfs.h>
#include "osd_internal.h"

#define LIXI_RESERVE 1

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
                         struct thandle *handle, struct lustre_capa *capa,
                         int ignore_quota)
{
	struct inode		*inode = osd_dt_obj(dt)->oo_inode;
	struct osd_thandle	*oh;
	ssize_t			 rc;

	LASSERT(dt_object_exists(dt));

#ifdef LIXI
	if (osd_object_auth(env, dt, capa, CAPA_OPC_BODY_WRITE))
		return -EACCES;
#endif /* LIXI */

	LASSERT(handle != NULL);
	LASSERT(inode != NULL);
#ifdef LIXI
	ll_vfs_dq_init(inode);
#endif /* LIXI */

	oh = container_of(handle, struct osd_thandle, ot_super);

	/* LIXI TODO: symlink support */
	LASSERT(!S_ISLNK(dt->do_lu.lo_header->loh_attr));
	rc = lbtrfs_write_record(inode, buf->lb_buf, buf->lb_len, pos);
        return rc;
}

static ssize_t osd_read(const struct lu_env *env, struct dt_object *dt,
                        struct lu_buf *buf, loff_t *pos,
                        struct lustre_capa *capa)
{
	struct inode *inode = osd_dt_obj(dt)->oo_inode;
	int           rc;

#ifdef LIXI
	if (osd_object_auth(env, dt, capa, CAPA_OPC_BODY_READ))
		RETURN(-EACCES);
#endif /* LIXI */

	LASSERT(!S_ISLNK(dt->do_lu.lo_header->loh_attr));
	rc = lbtrfs_read_record(inode, buf->lb_buf, buf->lb_len, pos);

        return rc;
}

static int osd_declare_punch(const struct lu_env *env, struct dt_object *dt,
                             __u64 start, __u64 end, struct thandle *th)
{
	struct osd_thandle *oh;
	ENTRY;

	LASSERT(th);
	oh = container_of(th, struct osd_thandle, ot_super);

	osd_trans_declare_op(oh, osd_item_number[OTO_XATTR_SET]);

	RETURN(0);
}

static int osd_punch(const struct lu_env *env, struct dt_object *dt,
		     __u64 start, __u64 end, struct thandle *th,
		     struct lustre_capa *capa)
{
	struct osd_thandle *oh;
	struct osd_object  *obj = osd_dt_obj(dt);
	struct inode       *inode = obj->oo_inode;
	int		   rc = 0;
	ENTRY;

	LASSERT(end == OBD_OBJECT_EOF);
	LASSERT(dt_object_exists(dt));
	LASSERT(osd_invariant(obj));
	LASSERT(inode != NULL);
	ll_vfs_dq_init(inode);

	LASSERT(th);
	oh = container_of(th, struct osd_thandle, ot_super);

	i_size_write(inode, start);
	ll_truncate_pagecache(inode, start);
	/* Btrfs does not have i_op->truncate() anyway */
	lbtrfs_punch(inode, oh->ot_handle, start, end);

	/*
	 * For a partial-page truncate, flush the page to disk immediately to
	 * avoid data corruption during direct disk write.  b=17397
	 */
	if ((start & ~CFS_PAGE_MASK) != 0)
                rc = filemap_fdatawrite_range(inode->i_mapping, start, start+1);

#ifdef LIXI
        h = journal_current_handle();
        LASSERT(h != NULL);
        LASSERT(h == oh->ot_handle);

        if (tid != h->h_transaction->t_tid) {
                int credits = oh->ot_credits;
                /*
                 * transaction has changed during truncate
                 * we need to restart the handle with our credits
                 */
                if (h->h_buffer_credits < credits) {
                        if (ldiskfs_journal_extend(h, credits))
                                rc2 = ldiskfs_journal_restart(h, credits);
                }
        }

        RETURN(rc == 0 ? rc2 : rc);
#endif /* LIXI */

	RETURN(rc);
}

static int osd_map_remote_to_local(loff_t offset, ssize_t len, int *nrpages,
                                   struct niobuf_local *lnb)
{
	   ENTRY;

	*nrpages = 0;

	while (len > 0) {
		int poff = offset & (PAGE_CACHE_SIZE - 1);
		int plen = PAGE_CACHE_SIZE - poff;

		if (plen > len)
			plen = len;
		lnb->lnb_file_offset = offset;
		lnb->lnb_page_offset = poff;
		lnb->lnb_len = plen;
		/* lnb->lnb_flags = rnb->rnb_flags; */
		lnb->lnb_flags = 0;
		lnb->lnb_page = NULL;
		lnb->lnb_rc = 0;

		LASSERTF(plen <= len, "plen %u, len %lld\n", plen,
			 (long long) len);
		offset += plen;
		len -= plen;
		lnb++;
		(*nrpages)++;
	}

	RETURN(0);
}

struct page *osd_get_page(struct dt_object *dt, loff_t offset, int rw)
{
	struct inode      *inode = osd_dt_obj(dt)->oo_inode;
#ifdef LIXI
	struct osd_device *d = osd_obj2dev(osd_dt_obj(dt));
#endif /* LIXI */
	struct page       *page;

	LASSERT(inode);

	page = find_or_create_page(inode->i_mapping, offset >> PAGE_CACHE_SHIFT,
				   GFP_NOFS | __GFP_HIGHMEM);
#ifdef LIXI
	if (unlikely(page == NULL))
		lprocfs_counter_add(d->od_stats, LPROC_OSD_NO_PAGE, 1);
#endif /* LIXI */

	return page;
}

/*
 * there are following "locks":
 * journal_start
 * i_mutex
 * page lock

 * osd write path
    * lock page(s)
    * journal_start
    * truncate_sem

 * ext4 vmtruncate:
    * lock pages, unlock
    * journal_start
    * lock partial page
    * i_data_sem

*/
int osd_bufs_get(const struct lu_env *env, struct dt_object *d, loff_t pos,
                 ssize_t len, struct niobuf_local *lnb, int rw,
                 struct lustre_capa *capa)
{
        struct osd_object   *obj    = osd_dt_obj(d);
        int npages, i, rc = 0;

        LASSERT(obj->oo_inode);

        osd_map_remote_to_local(pos, len, &npages, lnb);

        for (i = 0; i < npages; i++, lnb++) {
		lnb->lnb_page = osd_get_page(d, lnb->lnb_file_offset, rw);
		if (lnb->lnb_page == NULL)
			GOTO(cleanup, rc = -ENOMEM);

		/* DLM locking protects us from write and truncate competing
		 * for same region, but truncate can leave dirty page in the
		 * cache. it's possible the writeout on a such a page is in
		 * progress when we access it. it's also possible that during
		 * this writeout we put new (partial) data, but then won't
		 * be able to proceed in filter_commitrw_write(). thus let's
		 * just wait for writeout completion, should be rare enough.
		 * -bzzz */
		wait_on_page_writeback(lnb->lnb_page);
		BUG_ON(PageWriteback(lnb->lnb_page));

                lu_object_get(&d->do_lu);
        }
        rc = i;

cleanup:
        RETURN(rc);
}

static int osd_bufs_put(const struct lu_env *env, struct dt_object *dt,
			struct niobuf_local *lnb, int npages)
{
	int i;

	for (i = 0; i < npages; i++) {
		if (lnb[i].lnb_page == NULL)
			continue;
		//LASSERT(PageLocked(lnb[i].lnb_page));
		///* btrfs_writepage_start_hook() checks whether PageChecked() is cleared,
		// * so need to calls ClearPageChecked() like btrfs_drop_pages() does */
		//ClearPageChecked(lnb[i].lnb_page);
		/* todo, this is a walkaround fix */
		if (PageLocked(lnb[i].lnb_page))
			unlock_page(lnb[i].lnb_page);
		page_cache_release(lnb[i].lnb_page);
		lu_object_put(env, &dt->do_lu);
		lnb[i].lnb_page = NULL;
	}

	RETURN(0);
}

static int __osd_init_iobuf(struct osd_device *d, struct osd_iobuf *iobuf,
			    int rw, int line, int pages)
{
	int i;
	LASSERT(pages <= PTLRPC_MAX_BRW_PAGES);

	iobuf->dr_npages = 0;

	/* start with 1MB for 4K blocks */
	i = 256;
	while (i <= PTLRPC_MAX_BRW_PAGES && i < pages)
		i <<= 1;

	CDEBUG(D_OTHER, "realloc %u for %u (%u) pages\n",
	       (unsigned)(pages * sizeof(iobuf->dr_pages[0])), i, pages);
	pages = i;
	iobuf->dr_max_pages = 0;

	lu_buf_realloc(&iobuf->dr_pg_buf, pages * sizeof(iobuf->dr_pages[0]));
	iobuf->dr_pages = iobuf->dr_pg_buf.lb_buf;
	if (unlikely(iobuf->dr_pages == NULL))
		return -ENOMEM;

	iobuf->dr_max_pages = pages;

	return 0;
}
#define osd_init_iobuf(dev, iobuf, rw, pages) \
	__osd_init_iobuf(dev, iobuf, rw, __LINE__, pages)

static void osd_iobuf_add_page(struct osd_iobuf *iobuf, struct page *page)
{
        LASSERT(iobuf->dr_npages < iobuf->dr_max_pages);
        iobuf->dr_pages[iobuf->dr_npages++] = page;
}

static int osd_read_prep(const struct lu_env *env, struct dt_object *dt,
                         struct niobuf_local *lnb, int npages)
{
	struct osd_thread_info *oti = osd_oti_get(env);
	 struct inode *inode = osd_dt_obj(dt)->oo_inode;
	struct osd_device *osd = osd_obj2dev(osd_dt_obj(dt));
	struct osd_iobuf *iobuf = &oti->oti_iobuf;
	int rc, i;
	ENTRY;

	rc = osd_init_iobuf(osd, iobuf, 0, npages);
	if (unlikely(rc != 0))
		RETURN(rc);

	for (i = 0; i < npages; i++) {
		if (i_size_read(inode) <= lnb[i].lnb_file_offset)
		/* If there's no more data, abort early.
		 * lnb->lnb_rc == 0, so it's easy to detect later. */
		break;

		if (i_size_read(inode) <
		    lnb[i].lnb_file_offset + lnb[i].lnb_len - 1)
			lnb[i].lnb_rc = i_size_read(inode) -
				lnb[i].lnb_file_offset;
		else
			lnb[i].lnb_rc = lnb[i].lnb_len;

		if (!PageUptodate(lnb[i].lnb_page))
			osd_iobuf_add_page(iobuf, lnb[i].lnb_page);

		/* LIXI TODO: Read cache support, now always cache data */
#ifdef LIXI
		generic_error_remove_page(inode->i_mapping,
					  lnb[i].lnb_page);
#endif /* LIXI */
	}

	if (iobuf->dr_npages)
		rc = lbtrfs_read_prep(inode, iobuf->dr_pages, iobuf->dr_npages);

	RETURN(rc);
}

static int osd_write_prep(const struct lu_env *env, struct dt_object *dt,
                          struct niobuf_local *lnb, int npages)
{
	struct osd_thread_info *oti   = osd_oti_get(env);
	struct osd_iobuf       *iobuf = &oti->oti_iobuf;
	struct inode           *inode = osd_dt_obj(dt)->oo_inode;
	struct osd_device      *osd   = osd_obj2dev(osd_dt_obj(dt));
	ssize_t                 isize;
	__s64                   maxidx;
	int                     rc = 0;
	int                     i;
	u64			reserve_bytes;

	LASSERT(inode);

	rc = osd_init_iobuf(osd, iobuf, 0, npages);
	if (unlikely(rc != 0))
		RETURN(rc);

	isize = i_size_read(inode);
	maxidx = ((isize + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT) - 1;

	for (i = 0; i < npages; i++) {

		/* LIXI TODO: Write cache support, now always cache data */
#ifdef LIXI
		generic_error_remove_page(inode->i_mapping,
					  lnb[i].lnb_page);
#endif /* LIXI */

		/*
		 * till commit the content of the page is undefined
		 * we'll set it uptodate once bulk is done. otherwise
		 * subsequent reads can access non-stable data
		 */
		ClearPageUptodate(lnb[i].lnb_page);

		if (lnb[i].lnb_len == PAGE_CACHE_SIZE)
			continue;

		if (maxidx >= lnb[i].lnb_page->index) {
			osd_iobuf_add_page(iobuf, lnb[i].lnb_page);
		} else {
			long off;
			char *p = kmap(lnb[i].lnb_page);

			off = lnb[i].lnb_page_offset;
			if (off)
				memset(p, 0, off);
			off = (lnb[i].lnb_page_offset + lnb[i].lnb_len) &
			      ~CFS_PAGE_MASK;
			if (off)
				memset(p + off, 0, PAGE_CACHE_SIZE - off);
			kunmap(lnb[i].lnb_page);
		}
	}

	reserve_bytes = npages << PAGE_CACHE_SHIFT;
#ifdef LIXI_RESERVE
	rc = lbtrfs_check_data_free_space(inode, reserve_bytes);
	if (rc) {
		CDEBUG(D_INODE, "no free space, rc = %d\n",  rc);
		RETURN(rc);
	}
	
	rc = lbtrfs_delalloc_reserve_metadata(inode, reserve_bytes);
	if (rc) {
		CDEBUG(D_INODE, "failed to reserve metadata, rc = %d\n",  rc);
		lbtrfs_free_reserved_data_space(inode, reserve_bytes);
		RETURN(rc);
	}
#endif

	rc = lbtrfs_read_prep(inode, iobuf->dr_pages, iobuf->dr_npages);
	if (rc)  {
		CDEBUG(D_INODE, "failed to read inode, rc = %d\n",  rc);
#ifdef LIXI_RESERVE
		lbtrfs_delalloc_release_space(inode, reserve_bytes);
#endif
		RETURN(rc);
	}

        RETURN(rc);
}

static int osd_declare_write_commit(const struct lu_env *env,
                                    struct dt_object *dt,
                                    struct niobuf_local *lnb, int npages,
                                    struct thandle *handle)
{
	struct osd_thandle      *oh;
	int			 rc = 0;
#ifndef LIXI_RESERVE
	long long		 space = npages * PAGE_CACHE_SIZE;
#endif
	ENTRY;

	LASSERT(handle != NULL);
	oh = container_of0(handle, struct osd_thandle, ot_super);
	LASSERT(oh->ot_handle == NULL);

	/* osd_write_prep() has already reserved space */
#ifndef LIXI_RESERVE
	osd_trans_declare_op(oh, space / 4096);
#endif

	RETURN(rc);
}


static void osd_iobuf_add_page_sort(struct osd_iobuf *iobuf, struct page *page)
{
	int start, end, mid;
	LASSERT(iobuf->dr_npages < iobuf->dr_max_pages);
	iobuf->dr_pages[iobuf->dr_npages++] = page;

	end = iobuf->dr_npages - 1;
	if (end == 0)
		return;
	if (likely(page_offset(iobuf->dr_pages[end]) >
	    page_offset(iobuf->dr_pages[end - 1])))
		return;

	start = 0;
	mid = (start + end) / 2;
	while (start < end) {
		if (page_offset(iobuf->dr_pages[mid]) <
		    page_offset(iobuf->dr_pages[iobuf->dr_npages - 1]))
			start = mid + 1;
		else
			end = mid;
		mid = (start + end) / 2;
	}
	memmove(iobuf->dr_pages + mid + 1, iobuf->dr_pages + mid,
		(iobuf->dr_npages - mid) * sizeof(struct page *));
	iobuf->dr_pages[mid] = page;
	return;
}

static int osd_iobuf_dirty_pages(struct osd_iobuf *iobuf,
				  struct inode *inode)
{
	int i;
	int rc = 0;
	int start = 1;
	int npage = iobuf->dr_npages;

	while (npage > 0) {
		for (i = start; i < iobuf->dr_npages; i++) {
			if ((page_offset(iobuf->dr_pages[i - 1])
			    + PAGE_CACHE_SIZE) == page_offset(iobuf->dr_pages[i]))
				continue;
			else
				break;
		}
		rc = lbtrfs_dirty_pages(LBTRFS_I(inode)->root, inode,
				iobuf->dr_pages + start - 1, i - start + 1,
				page_offset(iobuf->dr_pages[start - 1]),
				(i - start + 1) << PAGE_CACHE_SHIFT, NULL);
		if (rc)
			return rc;
		npage -= (i - start + 1);
		start = i;
	}
	return rc;
}

/* Check if a block is allocated or not */
static int osd_write_commit(const struct lu_env *env, struct dt_object *dt,
                            struct niobuf_local *lnb, int npages,
                            struct thandle *th)
{
        struct osd_thread_info *oti = osd_oti_get(env);
        struct osd_iobuf *iobuf = &oti->oti_iobuf;
        struct inode *inode = osd_dt_obj(dt)->oo_inode;
        struct osd_device  *osd = osd_obj2dev(osd_dt_obj(dt));
        loff_t isize;
        loff_t current_isize = i_size_read(inode);
        int rc = 0, i;
        struct osd_thandle *oh;

        LASSERT(inode);
        LASSERT(th != NULL);

	oh = container_of0(th, struct osd_thandle, ot_super);
	LASSERT(oh->ot_handle != NULL);

	rc = osd_init_iobuf(osd, iobuf, 1, npages);
	if (unlikely(rc != 0))
		RETURN(rc);

	isize = i_size_read(inode);
	ll_vfs_dq_init(inode);

        for (i = 0; i < npages; i++) {
		if (lnb[i].lnb_rc) { /* ENOSPC, network RPC error, etc. */
			CDEBUG(D_INODE, "Skipping [%d] == %d\n", i,
			       lnb[i].lnb_rc);
			LASSERT(lnb[i].lnb_page);
			generic_error_remove_page(inode->i_mapping,
						  lnb[i].lnb_page);
			continue;
		}

		LASSERT(PageLocked(lnb[i].lnb_page));
		LASSERT(!PageWriteback(lnb[i].lnb_page));

		if (lnb[i].lnb_file_offset + lnb[i].lnb_len > isize)
			isize = lnb[i].lnb_file_offset + lnb[i].lnb_len;

		/*
		 * Since write and truncate are serialized by oo_sem, even
		 * partial-page truncate should not leave dirty pages in the
		 * page cache.
		 */
		LASSERT(!PageDirty(lnb[i].lnb_page));

		SetPageUptodate(lnb[i].lnb_page);

		osd_iobuf_add_page_sort(iobuf, lnb[i].lnb_page);
	}

	/*
	 * __extent_writepage() requires that the page indexes
	 * are in the range of inode size. TODO: remove me
	 */
	if (isize > current_isize)
		i_size_write(inode, isize);

	if (iobuf->dr_npages > 0) {
		rc = osd_iobuf_dirty_pages(iobuf, inode);
		if (rc) {
			CDEBUG(D_INODE, "failed to dirty page [%d], "
			       "rc = %d\n",  i, rc);
			RETURN(rc);
		}

		rc = lbtrfs_write_commit(inode, iobuf->dr_pages,
					 iobuf->dr_npages);
	}

	if (likely(rc == 0)) {
		if (isize > current_isize) {
			LBTRFS_I(inode)->disk_i_size = isize;
			/*
			 * No s_op->dirty_inode() is defined,
			 * so can't use ll_dirty_inode()
			 * We can't call btrfs_dirty_inode() in transaction,
			 * because it might start a transaction.
			 * So change to btrfs_update_inode() instead.
			 */
			lbtrfs_update_inode(oh->ot_handle, LBTRFS_I(inode)->root, inode);
                }      
	} else {
		/* Recover the inode size if write fails */
		i_size_write(inode, current_isize);
                /* if write fails, we should drop pages from the cache */
                for (i = 0; i < npages; i++) {
			if (lnb[i].lnb_page == NULL)
				continue;
			LASSERT(PageLocked(lnb[i].lnb_page));
			generic_error_remove_page(inode->i_mapping,
						  lnb[i].lnb_page);
		}
	}

	RETURN(rc);
}

/*
 * in some cases we may need declare methods for objects being created
 * e.g., when we create symlink
 */
const struct dt_body_operations osd_body_ops_new = {
        .dbo_declare_write = osd_declare_write,
};

const struct dt_body_operations osd_body_ops = {
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
	.dbo_fiemap_get           = NULL,
	//.dbo_fiemap_get           = osd_fiemap_get,
};
