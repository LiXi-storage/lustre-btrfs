/*
 *  linux/fs/ext2_obd/ext2_obd.c
 *
 * Copyright (C) 1999  Stelias Computing, Inc.
 * Copyright (C) 1999  Seagate Technology, Inc.
 * Copyright (C) 2001  Cluster File Systems, Inc.
 *
 * This code is issued under the GNU General Public License.
 * See the file COPYING in this distribution
 *
 * This is the object based disk driver based on ext2 
 * written by Peter Braam <braam@clusterfs.com>, Phil Schwan <phil@off.net>
 * Andreas Dilger <adilger@turbolinux.com>
 *
 */

#define EXPORT_SYMTAB

#include <linux/version.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/locks.h>
#include <linux/ext2_fs.h>
#include <linux/quotaops.h>
#include <asm/unistd.h>
#include <linux/obd_support.h>
#include <linux/obd_class.h>
#include <linux/obd_ext2.h>



extern struct obd_device obd_dev[MAX_OBD_DEVICES];
long filter_memory;

void push_ctxt(struct run_ctxt *save, struct run_ctxt *new)
{ 
	save->fs = get_fs();
	save->pwd = dget(current->fs->pwd);
	save->pwdmnt = mntget(current->fs->pwdmnt);

	set_fs(new->fs);
	set_fs_pwd(current->fs, new->pwdmnt, new->pwd);
}

void pop_ctxt(struct run_ctxt *saved)
{
	set_fs(saved->fs);
	set_fs_pwd(current->fs, saved->pwdmnt, saved->pwd);

	dput(saved->pwd);
	mntput(saved->pwdmnt);
}

static void filter_prep(struct obd_device *obddev)
{
	struct run_ctxt saved;
	long rc;
	int fd;
	struct stat64 buf;
	__u64 lastino = 0;

	push_ctxt(&saved, &obddev->u.filter.fo_ctxt);
	rc = sys_mkdir("O", 0700);
	rc = sys_mkdir("P", 0700);
	rc = sys_mkdir("D", 0700);
	if ( (fd = sys_open("D/status", O_RDWR | O_CREAT, 0700)) == -1 ) {
		printk("OBD filter: cannot create status file\n");
		goto out;
	}
	if ( (rc = sys_fstat64(fd, &buf, 0)) ) { 
		printk("OBD filter: cannot stat status file\n");
		goto out_close;
	}
	if (buf.st_size == 0) { 
		rc = sys_write(fd, (char *)&lastino, sizeof(lastino));
		if (rc != sizeof(lastino)) { 
			printk("OBD filter: error writing lastino\n");
			goto out_close;
		}
	} else { 
		rc = sys_read(fd, (char *)&lastino, sizeof(lastino));
		if (rc != sizeof(lastino)) { 
			printk("OBD filter: error writing lastino\n");
			goto out_close;
		}
	}
	obddev->u.filter.fo_lastino = lastino;
	
 out_close:
	rc = sys_close(fd);
	if (rc) { 
		printk("OBD filter: cannot close status file\n");
	}
 out:
	pop_ctxt(&saved);
}

static void filter_post(struct obd_device *obddev)
{
	struct run_ctxt saved;
	long rc;
	int fd;

	push_ctxt(&saved, &obddev->u.filter.fo_ctxt);
	if ( (fd = sys_open("D/status", O_RDWR | O_CREAT, 0700)) == -1 ) {
		printk("OBD filter: cannot create status file\n");
		goto out;
	}
	rc = sys_write(fd, (char *)&obddev->u.filter.fo_lastino, 
		       sizeof(obddev->u.filter.fo_lastino));
	if (rc != sizeof(sizeof(obddev->u.filter.fo_lastino)) ) { 
		printk("OBD filter: error writing lastino\n");
	}

	rc = sys_close(fd);
	if (rc) { 
		printk("OBD filter: cannot close status file\n");
	}
 out:
	pop_ctxt(&saved);
}


/* release per client resources */
static int filter_disconnect(struct obd_conn *conn)
{
	/* XXX cleanup preallocated inodes */
	return gen_disconnect(conn);
} /* ext2obd_disconnect */




/* 
 *   to initialize a particular /dev/obdNNN to simulated OBD type
 *   *data holds the device of the ext2 disk partition we will use. 
 */ 
static int filter_setup(struct obd_device *obddev, struct obd_ioctl_data* data)
{
	struct vfsmount *mnt;
	int err; 
        ENTRY;
        
	
	mnt = do_kern_mount(data->ioc_inlbuf2, 0, 
			    data->ioc_inlbuf1, NULL); 
	err = PTR_ERR(mnt);
	if (IS_ERR(mnt)) { 
		EXIT;
		return err;
	}

	obddev->u.filter.fo_sb = mnt->mnt_root->d_inode->i_sb;
  	if (!obddev->u.filter.fo_sb) {
  		EXIT;
  		return -ENODEV;
  	}

	obddev->u.filter.fo_vfsmnt = mnt;
	obddev->u.filter.fo_fstype = strdup(data->ioc_inlbuf2);

	obddev->u.filter.fo_ctxt.pwdmnt = mnt;
	obddev->u.filter.fo_ctxt.pwd = mnt->mnt_root;
	obddev->u.filter.fo_ctxt.fs = KERNEL_DS;

	filter_prep(obddev);
	
        MOD_INC_USE_COUNT;
        EXIT; 
        return 0;
} 

static int filter_cleanup(struct obd_device * obddev)
{
        struct super_block *sb;

        ENTRY;

        if ( !(obddev->obd_flags & OBD_SET_UP) ) {
                EXIT;
                return 0;
        }

        if ( !list_empty(&obddev->obd_gen_clients) ) {
                printk(KERN_WARNING __FUNCTION__ ": still has clients!\n");
                EXIT;
                return -EBUSY;
        }

        sb = obddev->u.filter.fo_sb;
        if (!obddev->u.filter.fo_sb){
                EXIT;
                return 0;
        }
	filter_post(obddev);

	unlock_kernel();
	mntput(obddev->u.filter.fo_vfsmnt); 
        obddev->u.filter.fo_sb = 0;
	kfree(obddev->u.filter.fo_fstype);

	lock_kernel();


        MOD_DEC_USE_COUNT;
        EXIT;
        return 0;
}

static struct inode *inode_from_obdo(struct obd_device *obddev, 
				     struct obdo *oa)
{
	char id[16];
	struct super_block *sb;
	struct inode *inode; 
	struct run_ctxt saved;
	struct stat64 st;

	sb = obddev->u.filter.fo_sb;
        if (!sb || !sb->s_dev) {
                CDEBUG(D_SUPER, "fatal: device not initialized.\n");
                EXIT;
                return NULL;
        }

        if ( !oa->o_id ) {
                CDEBUG(D_INODE, "fatal: invalid obdo %lu\n", (long)oa->o_id);
                EXIT;
                return NULL;
        }

	sprintf(id, "O/%Ld", oa->o_id);

	push_ctxt(&saved, &obddev->u.filter.fo_ctxt);
	if (sys_stat64(id, &st, 0)) { 
		EXIT;
		return NULL;
	}
	pop_ctxt(&saved);

        inode = iget(sb, st.st_ino);
        if (!inode || inode->i_nlink == 0 || is_bad_inode(inode)) {
                printk("from obdo - fatal: invalid inode %ld (%s).\n",
                       (long)oa->o_id, inode ? inode->i_nlink ? "bad inode" :
                       "no links" : "NULL");
                if (inode)
                        iput(inode);
                EXIT;
                return NULL;
        }
	return inode;
}

static inline void filter_from_inode(struct obdo *oa, struct inode *inode)
{
        ENTRY;

        CDEBUG(D_INFO, "src inode %ld, dst obdo %ld valid 0x%08x\n",
               inode->i_ino, (long)oa->o_id, oa->o_valid);
        obdo_from_inode(oa, inode);

        if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
		obd_rdev rdev = kdev_t_to_nr(inode->i_rdev);
                CDEBUG(D_INODE, "copying device %x from inode to obdo\n",
		       rdev);
                *((obd_rdev *)oa->o_inline) = rdev;
                oa->o_obdflags |= OBD_FL_INLINEDATA;
                oa->o_valid |= OBD_MD_FLINLINE;
        }

#if 0
 else if (ext2obd_has_inline(inode)) {
                CDEBUG(D_INFO, "copying inline from inode to obdo\n");
                memcpy(oa->o_inline, inode->u.ext2_i.i_data,
                       MIN(sizeof(inode->u.ext2_i.i_data),OBD_INLINESZ));
                oa->o_obdflags |= OBD_FL_INLINEDATA;
                oa->o_valid |= OBD_MD_FLINLINE;
        }

        if (ext2obd_has_obdmd(inode)) {
                /* XXX this will change when we don't store the obdmd in data */
                CDEBUG(D_INFO, "copying obdmd from inode to obdo\n");
                memcpy(oa->o_obdmd, inode->u.ext2_i.i_data,
                       MIN(sizeof(inode->u.ext2_i.i_data),OBD_INLINESZ));
                oa->o_obdflags |= OBD_FL_OBDMDEXISTS;
                oa->o_valid |= OBD_MD_FLOBDMD;
        }
#endif
        EXIT;
}

static int filter_getattr(struct obd_conn *conn, struct obdo *oa)
{
        struct inode *inode;

        ENTRY;
        if ( !gen_client(conn) ) {
                CDEBUG(D_IOCTL, "fatal: invalid client %u\n", conn->oc_id);
                EXIT;
                return -EINVAL;
        }

	if ( !(inode = inode_from_obdo(conn->oc_dev, oa)) ) { 
		EXIT;
		return -ENOENT;
	}

        filter_from_inode(oa, inode);
        iput(inode);
        EXIT;
        return 0;
} 

static int filter_setattr(struct obd_conn *conn, struct obdo *oa)
{
	struct inode *inode;
	struct iattr iattr;
	int rc;
	struct dentry de;

        if (!gen_client(conn)) {
                CDEBUG(D_IOCTL, "invalid client %u\n", conn->oc_id);
                return -EINVAL;
        }

	inode = inode_from_obdo(conn->oc_dev, oa); 
	if ( !inode ) { 
		EXIT;
		return -ENOENT;
	}

	iattr_from_obdo(&iattr, oa);
	de.d_inode = inode;
	if ( inode->i_op->setattr ) {
		rc = inode->i_op->setattr(&de, &iattr);
	} else { 
		rc = inode_setattr(inode, &iattr);
	}

	iput(inode);
	EXIT;
	return rc;
}

static int filter_create (struct obd_conn* conn, struct obdo *oa)
{
	char name[64];
	struct run_ctxt saved;
	struct obd_device *obddev = conn->oc_dev;
	struct iattr;
	int rc;

        if (!gen_client(conn)) {
                CDEBUG(D_IOCTL, "invalid client %u\n", conn->oc_id);
                return -EINVAL;
        }

	conn->oc_dev->u.filter.fo_lastino++;
	oa->o_id = conn->oc_dev->u.filter.fo_lastino;
	sprintf(name, "O/%Ld", oa->o_id);
	push_ctxt(&saved, &obddev->u.filter.fo_ctxt);
	if (sys_mknod(name, 010644, 0)) { 
		printk("Error mknod %s\n", name);
		return -ENOENT;
	}
	pop_ctxt(&saved);

	rc = filter_setattr(conn, oa); 
	if ( rc ) { 
		EXIT;
		return -EINVAL;
	}
	
        /* Set flags for fields we have set in ext2_new_inode */
        oa->o_valid |= OBD_MD_FLID | OBD_MD_FLBLKSZ | OBD_MD_FLBLOCKS |
                 OBD_MD_FLMTIME | OBD_MD_FLATIME | OBD_MD_FLCTIME |
                 OBD_MD_FLUID | OBD_MD_FLGID;
        return 0;
}

static int filter_destroy(struct obd_conn *conn, struct obdo *oa)
{
        struct obd_device * obddev;
        struct obd_client * cli;
        struct inode * inode;
	struct run_ctxt saved;
	char id[128];

        if (!(cli = gen_client(conn))) {
                CDEBUG(D_IOCTL, "invalid client %u\n", conn->oc_id);
                EXIT;
                return -EINVAL;
        }

        obddev = conn->oc_dev;
	inode = inode_from_obdo(obddev, oa);

	if (!inode) { 
		EXIT;
		return -ENOENT;
	}

        inode->i_nlink = 1;
	inode->i_mode = 010000;
	iput(inode);

	sprintf(id, "O/%Ld", oa->o_id);
	push_ctxt(&saved, &obddev->u.filter.fo_ctxt);
	if (sys_unlink(id)) { 
		EXIT;
		return -EPERM;
	}
	pop_ctxt(&saved);

	EXIT;
        return 0;
}

static int filter_read(struct obd_conn *conn, struct obdo *oa, char *buf,
                        obd_size *count, obd_off offset)
{
        struct super_block *sb;
        struct inode * inode;
        struct file * f;
        struct file fake_file;
        struct dentry fake_dentry;
        unsigned long retval;
        int err;


        if (!gen_client(conn)) {
                CDEBUG(D_IOCTL, "invalid client %u\n", conn->oc_id);
                EXIT;
                return -EINVAL;
        }

        sb = conn->oc_dev->u.ext2.ext2_sb;
	if ( !(inode = inode_from_obdo(conn->oc_dev, oa)) ) { 
		EXIT;
		return -ENOENT;
	}

        if (!S_ISREG(inode->i_mode)) {
                iput(inode);
                CDEBUG(D_INODE, "fatal: not regular file %ld (mode=%o).\n",
                       inode->i_ino, inode->i_mode);
                EXIT;
                return -EINVAL;
        }

        memset(&fake_file, 0, sizeof(fake_file));
        memset(&fake_dentry, 0, sizeof(fake_dentry));

        f = &fake_file;
        f->f_dentry = &fake_dentry;
        f->f_dentry->d_inode = inode;
	f->f_flags = O_LARGEFILE;
        f->f_op = &ext2_file_operations;
	inode->i_mapping->a_ops = &ext2_aops;

        /* count doubles as retval */
        retval = f->f_op->read(f, buf, *count, &offset);
        iput(inode);
        if ( retval >= 0 ) {
                err = 0;
                *count = retval;
        } else {
                err = retval;
                *count = 0;
        }

        return err;
} /* ext2obd_read */


static int filter_write(struct obd_conn *conn, struct obdo *oa, char *buf, 
                         obd_size *count, obd_off offset)
{
        int err;
        struct super_block *sb;
        struct inode * inode;
        struct file fake_file;
        struct dentry fake_dentry;
        struct file * f;
        unsigned long retval;

        ENTRY;

        if (!gen_client(conn)) {
                CDEBUG(D_IOCTL, "invalid client %u\n", conn->oc_id);
                EXIT;
                return -EINVAL;
        }

        sb = conn->oc_dev->u.ext2.ext2_sb;
	if ( !(inode = inode_from_obdo(conn->oc_dev, oa)) ) { 
		EXIT;
		return -ENOENT;
	}

        if (!S_ISREG(inode->i_mode)) {
                CDEBUG(D_INODE, "fatal: not regular file.\n");
                iput(inode);
                EXIT;
                return -EINVAL;
        }

        memset(&fake_file, 0, sizeof(fake_file));
        memset(&fake_dentry, 0, sizeof(fake_dentry));

        f = &fake_file;
        f->f_dentry = &fake_dentry;
        f->f_dentry->d_inode = inode;
        f->f_op = &ext2_file_operations;
	f->f_flags = O_LARGEFILE;
	inode->i_mapping->a_ops = &ext2_aops;

        /* count doubles as retval */
	if (f->f_op->write)
		retval = f->f_op->write(f, buf, *count, &(offset));
	else 
		retval = -EINVAL;
        CDEBUG(D_INFO, "Result %ld\n", retval);

        oa->o_valid = OBD_MD_FLBLOCKS | OBD_MD_FLCTIME | OBD_MD_FLMTIME;
        obdo_from_inode(oa, inode);
        iput(inode);

        if ( retval >= 0 ) {
                err = 0;
                *count = retval;
                EXIT;
        } else {
                err = retval;
                *count = 0;
                EXIT;
        }

        return err;
} /* ext2obd_write */

void ___wait_on_page(struct page *page)
{
        struct task_struct *tsk = current;
        DECLARE_WAITQUEUE(wait, tsk);

        add_wait_queue(&page->wait, &wait);
        do {
                run_task_queue(&tq_disk);
                set_task_state(tsk, TASK_UNINTERRUPTIBLE);
                if (!PageLocked(page))
                        break;
                schedule();
        } while (PageLocked(page));
        tsk->state = TASK_RUNNING;
        remove_wait_queue(&page->wait, &wait);
}

static inline int actor_from_kernel(char *dst, char *src, size_t len)
{
	ENTRY;
	memcpy(dst, src, len);
	EXIT;
	return 0;
}

int kernel_read_actor(read_descriptor_t * desc, struct page *page, unsigned long offset, unsigned long size)
{
	char *kaddr;
	unsigned long count = desc->count;
	ENTRY;
	if (desc->buf == NULL) {
		printk("ALERT: desc->buf == NULL\n");
		desc->error = -EIO;
		return -EIO;
	}
	
	if (size > count)
		size = count;

	kaddr = kmap(page);
	memcpy(desc->buf, kaddr + offset, size);
	kunmap(page);
	
	desc->count = count - size;
	desc->written += size;
	desc->buf += size;
	EXIT;
	return size;
}

static int filter_pgcache_brw(int rw, struct obd_conn *conn, 
			       obd_count num_oa,
			       struct obdo **oa, 
			       obd_count *oa_bufs, 
			       struct page **pages,
			       obd_size *count, 
			       obd_off *offset, 
			       obd_flag *flags)
{
        struct super_block      *sb;
        int                      onum;          /* index to oas */
        int                      pnum;          /* index to pages (bufs) */
        unsigned long            retval;
        int                      err;
	struct file fake_file;
	struct dentry fake_dentry; 
	struct file *f;

        ENTRY;

        if (!gen_client(conn)) {
                CDEBUG(D_IOCTL, "invalid client %u\n", conn->oc_id);
                EXIT;
                return -EINVAL;
        }

        sb = conn->oc_dev->u.ext2.ext2_sb;

        pnum = 0; /* pnum indexes buf 0..num_pages */
        for (onum = 0; onum < num_oa; onum++) {
                struct inode    *inode;
                int              pg;

                if ( rw == READ ) 
                        *flags &= ~OBD_BRW_CREATE;

                if (! (inode = inode_from_obdo(conn->oc_dev, oa[onum])) ) {
                        EXIT;
                        return -ENOENT;
                }

		CDEBUG(D_INODE, "ino %ld, i_count %d\n", 
		       inode->i_ino, atomic_read(&inode->i_count)); 
		memset(&fake_file, 0, sizeof(fake_file));
		memset(&fake_dentry, 0, sizeof(fake_dentry));
		
		f = &fake_file;
		f->f_dentry = &fake_dentry;
		f->f_dentry->d_inode = inode;
		f->f_op = &ext2_file_operations;
		f->f_flags = O_LARGEFILE;
		inode->i_mapping->a_ops = &ext2_aops;

		/* count doubles as retval */
                for (pg = 0; pg < oa_bufs[onum]; pg++) {
			CDEBUG(D_INODE, "OP %d obdo no/pno: (%d,%d) (%ld,%ld) off count (%Ld,%Ld)\n", 
			       rw, onum, pnum, inode->i_ino,
			       (unsigned long)offset[pnum] >> PAGE_CACHE_SHIFT,
			       offset[pnum], count[pnum]);
			if (rw == WRITE) { 
				loff_t off; 
				char *buffer;
				off = offset[pnum]; 
				buffer = kmap(pages[pnum]); 
				retval = do_generic_file_write
					(f, buffer, count[pnum], &off, 
					 actor_from_kernel);
				kunmap(pages[pnum]);
				CDEBUG(D_INODE, "retval %ld\n", retval); 
			} else { 
				loff_t off; 
				read_descriptor_t desc;
				char *buffer = kmap(pages[pnum]);

				desc.written = 0;
				desc.count = count[pnum];
				desc.buf = buffer;
				desc.error = 0;
				off = offset[pnum]; 

				off = offset[pnum]; 
				if (off >= inode->i_size) {
					memset(buffer, 0, PAGE_SIZE);
				} else {
					do_generic_file_read
						(f, &off, &desc, 
						 kernel_read_actor);
				} 
				kunmap(pages[pnum]);
				retval = desc.written;
				if ( !retval ) {
					iput(inode);
					retval = desc.error; 
					EXIT;
					goto ERROR;
				}
				CDEBUG(D_INODE, "retval %ld\n", retval); 
			}
			pnum++;
		}
		/* sizes and blocks are set by generic_file_write */
		/* ctimes/mtimes will follow with a setattr call */ 

                //oa[onum]->o_blocks = inode->i_blocks;
                //oa[onum]->o_valid = OBD_MD_FLBLOCKS;
		/* perform the setattr on the inode */
		//ext2obd_to_inode(inode, oa[onum]);
		//inode->i_size = oa[onum]->o_size;
		//mark_inode_dirty(inode); 
                iput(inode);
	}
	
	EXIT;
 ERROR:
	err = (retval >= 0) ? 0 : retval;
	return err;
}

static int filter_statfs (struct obd_conn *conn, struct statfs * statfs)
{
	struct super_block *sb;
        int err;

        ENTRY;

        if (!gen_client(conn)) {
                CDEBUG(D_IOCTL, "invalid client %u\n", conn->oc_id);
                EXIT;
                return -EINVAL;
        }

        sb = conn->oc_dev->u.filter.fo_sb;

        err = sb->s_op->statfs(sb, statfs);
        EXIT;
        return err;
} /* ext2obd_statfs */


static int  filter_get_info(struct obd_conn *conn, obd_count keylen,
                             void *key, obd_count *vallen, void **val)
{
        struct obd_device *obddev;
        struct obd_client * cli;
	ENTRY;

        if (!(cli = gen_client(conn))) {
                CDEBUG(D_IOCTL, "invalid client %u\n", conn->oc_id);
                return -EINVAL;
        }

        obddev = conn->oc_dev;
        
        if ( keylen == strlen("blocksize") &&
             memcmp(key, "blocksize", keylen) == 0 ) {
                *vallen = sizeof(int);
                *val = (void *)obddev->u.filter.fo_sb->s_blocksize;
		EXIT;
                return 0;
        }

        if ( keylen == strlen("blocksize_bits") &&
             memcmp(key, "blocksize_bits", keylen) == 0 ){
                *vallen = sizeof(int);
                *val = (void *)(int)obddev->u.filter.fo_sb->s_blocksize_bits;
		EXIT;
                return 0;
        }

        if ( keylen == strlen("root_ino") &&
             memcmp(key, "root_ino", keylen) == 0 ){
                *vallen = sizeof(int);
                *val = (void *)(int)
			obddev->u.filter.fo_sb->s_root->d_inode->i_ino;
		EXIT;
                return 0;
        }
        
        CDEBUG(D_IOCTL, "invalid key\n");
        return -EINVAL;
}


struct obd_ops filter_obd_ops = {
        o_iocontrol:   NULL,
        o_get_info:    filter_get_info,
        o_setup:       filter_setup,
        o_cleanup:     filter_cleanup,
        o_connect:     gen_connect,
        o_disconnect:  filter_disconnect,
        o_statfs:      filter_statfs,
        o_getattr:     filter_getattr,
        o_create:      filter_create,
	o_setattr:     filter_setattr,
        o_destroy:     filter_destroy,
        o_read:        filter_read,
        o_write:       filter_write,
	o_brw:         filter_pgcache_brw,
#if 0
        o_preallocate: ext2obd_preallocate_inodes,
        o_setattr:     ext2obd_setattr,
        o_punch:       ext2obd_punch,
        o_migrate:     ext2obd_migrate,
        o_copy:        gen_copy_data,
        o_iterate:     ext2obd_iterate
#endif
};


#ifdef MODULE

void init_module(void)
{
        printk(KERN_INFO "Filtering OBD driver  v0.001, braam@clusterfs.com\n");
        obd_register_type(&filter_obd_ops, OBD_FILTER_DEVICENAME);
}

void cleanup_module(void)
{
        obd_unregister_type(OBD_FILTER_DEVICENAME);
        CDEBUG(D_MALLOC, "FILTER mem used %ld\n", filter_memory);
}

#endif
