#include <linux/module.h>
#include <linux/fs.h>
#include <linux/smp_lock.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/namei.h>
#include <linux/writeback.h>

#include "ceph_fs.h"
#include "ceph_debug.h"

int ceph_debug_inode = -1;
#define DOUT_MASK DOUT_MASK_INODE
#define DOUT_VAR ceph_debug_inode
#define DOUT_PREFIX "inode: "
#include "super.h"
#include "decode.h"

static const struct inode_operations ceph_symlink_iops;


/*
 * find or create an inode, given the ceph ino number
 */
struct inode *ceph_get_inode(struct super_block *sb, struct ceph_vino vino)
{
	struct inode *inode;
	ino_t t = ceph_vino_to_ino(vino);

	inode = iget5_locked(sb, t, ceph_ino_compare, ceph_set_ino_cb, &vino);
	if (inode == NULL)
		return ERR_PTR(-ENOMEM);
	if (inode->i_state & I_NEW) {
		dout(40, "get_inode created new inode %p %llx.%llx ino %llx\n",
		     inode, ceph_vinop(inode), (u64)inode->i_ino);
		unlock_new_inode(inode);
	}

	dout(30, "get_inode on %lu=%llx.%llx got %p\n", inode->i_ino, vino.ino,
	     vino.snap, inode);
	return inode;
}

/*
 * get/constuct snapdir inode for a given directory
 */
struct inode *ceph_get_snapdir(struct inode *parent)
{
	struct ceph_vino vino = {
		.ino = ceph_ino(parent),
		.snap = CEPH_SNAPDIR,
	};
	struct inode *inode = ceph_get_inode(parent->i_sb, vino);
	if (IS_ERR(inode))
		return ERR_PTR(PTR_ERR(inode));
	inode->i_mode = parent->i_mode;
	inode->i_uid = parent->i_uid;
	inode->i_gid = parent->i_gid;
	inode->i_op = &ceph_dir_iops;
	inode->i_fop = &ceph_dir_fops;
	ceph_inode(inode)->i_snap_caps = CEPH_CAP_PIN; /* so we can open */
	return inode;
}


const struct inode_operations ceph_file_iops = {
	.setattr = ceph_setattr,
	.getattr = ceph_getattr,
	.setxattr = ceph_setxattr,
	.getxattr = ceph_getxattr,
	.listxattr = ceph_listxattr,
	.removexattr = ceph_removexattr,
};


/*
 * find/create a frag in the tree
 */
static struct ceph_inode_frag *__get_or_create_frag(struct ceph_inode_info *ci, u32 f)
{
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct ceph_inode_frag *frag;
	int c;

	p = &ci->i_fragtree.rb_node;
	while (*p) {
		parent = *p;
		frag = rb_entry(parent, struct ceph_inode_frag, node);
		c = frag_compare(f, frag->frag);
		if (c < 0)
			p = &(*p)->rb_left;
		else if (c > 0)
			p = &(*p)->rb_right;
		else
			return frag;
	}

	frag = kmalloc(sizeof(*frag), GFP_NOFS);
	if (!frag) {
		derr(0, "ENOMEM on %p %llx.%llx frag %x\n", &ci->vfs_inode,
		     ceph_vinop(&ci->vfs_inode), f);
		return ERR_PTR(-ENOMEM);
	}
	frag->frag = f;
	frag->split_by = 0;
	frag->mds = -1;
	frag->ndist = 0;

	rb_link_node(&frag->node, parent, p);
	rb_insert_color(&frag->node, &ci->i_fragtree);

	dout(20, "get_or_create_frag added %llx.%llx frag %x\n",
	     ceph_vinop(&ci->vfs_inode), f);

	return frag;
}

/*
 * Choose frag containing the given value @v.  If @pfrag is
 * specified, copy the frag delegation info to the caller if
 * it is present.
 */
__u32 ceph_choose_frag(struct ceph_inode_info *ci, u32 v,
		       struct ceph_inode_frag *pfrag,
		       int *found)
{
	u32 t = frag_make(0, 0);
	struct ceph_inode_frag *frag;
	unsigned nway, i;
	u32 n;

	if (found)
		*found = 0;

	mutex_lock(&ci->i_fragtree_mutex);
	while (1) {
		WARN_ON(!frag_contains_value(t, v));
		frag = __ceph_find_frag(ci, t);
		if (!frag)
			break; /* t is a leaf */
		if (frag->split_by == 0) {
			if (pfrag)
				memcpy(pfrag, frag, sizeof(*pfrag));
			if (found)
				*found = 1;
			break;
		}

		/* choose child */
		nway = 1 << frag->split_by;
		dout(30, "choose_frag(%x) %x splits by %d (%d ways)\n", v, t,
		     frag->split_by, nway);
		for (i = 0; i < nway; i++) {
			n = frag_make(frag_bits(t) + frag->split_by,
				      frag_value(t) | (i << frag_bits(t)));
			if (frag_contains_value(n, v)) {
				t = n;
				break;
			}
		}
		BUG_ON(i == nway);
	}
	dout(30, "choose_frag(%x) = %x\n", v, t);

	mutex_unlock(&ci->i_fragtree_mutex);
	return t;
}

/*
 * Process dirfrag (delegation) info from the mds.  Include leaf
 * fragment in tree ONLY if mds >= 0 || ndist > 0.  Otherwise, only
 * branches/splits are included in i_fragtree)
 */
static int ceph_fill_dirfrag(struct inode *inode,
			     struct ceph_mds_reply_dirfrag *dirinfo)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct ceph_inode_frag *frag;
	u32 id = le32_to_cpu(dirinfo->frag);
	int mds = le32_to_cpu(dirinfo->auth);
	int ndist = le32_to_cpu(dirinfo->ndist);
	int i;
	int err = 0;

	mutex_lock(&ci->i_fragtree_mutex);
	if (mds < 0 && ndist == 0) {
		/* no delegation info needed. */
		frag = __ceph_find_frag(ci, id);
		if (!frag)
			goto out;
		if (frag->split_by == 0) {
			/* tree leaf, remove */
			dout(20, "fill_dirfrag removed %llx.%llx frag %x"
			     " (no ref)\n", ceph_vinop(inode), id);
			rb_erase(&frag->node, &ci->i_fragtree);
			kfree(frag);
		} else {
			/* tree branch, keep and clear */
			dout(20, "fill_dirfrag cleared %llx.%llx frag %x"
			     " referral\n", ceph_vinop(inode), id);
			frag->mds = -1;
			frag->ndist = 0;
		}
		goto out;
	}


	/* find/add this frag to store mds delegation info */
	frag = __get_or_create_frag(ci, id);
	if (IS_ERR(frag)) {
		/* this is not the end of the world; we can continue
		   with bad/inaccurate delegation info */
		derr(0, "fill_dirfrag ENOMEM on mds ref %llx.%llx frag %x\n",
		     ceph_vinop(inode), le32_to_cpu(dirinfo->frag));
		err = -ENOMEM;
		goto out;
	}

	frag->mds = mds;
	frag->ndist = min_t(u32, ndist, MAX_DIRFRAG_REP);
	for (i = 0; i < frag->ndist; i++)
		frag->dist[i] = le32_to_cpu(dirinfo->dist[i]);
	dout(20, "fill_dirfrag %llx.%llx frag %x referral mds %d ndist=%d\n",
	     ceph_vinop(inode), frag->frag, frag->mds, frag->ndist);

out:
	mutex_unlock(&ci->i_fragtree_mutex);
	return err;
}


/*
 * initialize a newly allocated inode.
 */
struct inode *ceph_alloc_inode(struct super_block *sb)
{
	struct ceph_inode_info *ci;
	int i;

	ci = kmem_cache_alloc(ceph_inode_cachep, GFP_NOFS);
	if (!ci)
		return NULL;

	dout(10, "alloc_inode %p\n", &ci->vfs_inode);

	ci->i_version = 0;
	ci->i_truncate_seq = 0;
	ci->i_time_warp_seq = 0;
	ci->i_symlink = NULL;

	ci->i_lease_session = NULL;
	ci->i_lease_mask = 0;
	ci->i_lease_ttl = 0;
	INIT_LIST_HEAD(&ci->i_lease_item);

	ci->i_fragtree = RB_ROOT;
	mutex_init(&ci->i_fragtree_mutex);

	ci->i_xattr_len = 0;
	ci->i_xattr_data = NULL;

	ci->i_caps = RB_ROOT;
	for (i = 0; i < CEPH_FILE_MODE_NUM; i++)
		ci->i_nr_by_mode[i] = 0;
	init_waitqueue_head(&ci->i_cap_wq);
	INIT_LIST_HEAD(&ci->i_cap_snaps);
	ci->i_snap_caps = 0;

	ci->i_wanted_max_size = 0;
	ci->i_requested_max_size = 0;

	ci->i_cap_exporting_mds = 0;
	ci->i_cap_exporting_mseq = 0;
	ci->i_cap_exporting_issued = 0;

	ci->i_rd_ref = ci->i_rdcache_ref = 0;
	ci->i_wr_ref = 0;
	ci->i_wrbuffer_ref = 0;
	ci->i_wrbuffer_ref_head = 0;
	ci->i_hold_caps_until = 0;
	INIT_LIST_HEAD(&ci->i_cap_delay_list);

	ci->i_snap_realm = NULL;

	INIT_WORK(&ci->i_wb_work, ceph_inode_writeback);

	ci->i_vmtruncate_to = -1;
	INIT_WORK(&ci->i_vmtruncate_work, ceph_vmtruncate_work);

	return &ci->vfs_inode;
}

void ceph_destroy_inode(struct inode *inode)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct ceph_inode_frag *frag;
	struct rb_node *n;

	dout(30, "destroy_inode %p ino %llx.%llx\n", inode, ceph_vinop(inode));
	kfree(ci->i_symlink);
	while ((n = rb_first(&ci->i_fragtree)) != NULL) {
		frag = rb_entry(n, struct ceph_inode_frag, node);
		rb_erase(n, &ci->i_fragtree);
		kfree(frag);
	}
	kfree(ci->i_xattr_data);
	kmem_cache_free(ceph_inode_cachep, ci);
}


/*
 * Helper to fill in size, ctime, mtime, and atime.  We have to be
 * careful because either the client or MDS may have more up to date
 * info, depending on which capabilities are held, and whether
 * time_warp_seq or truncate_seq have increased.  Ordinarily, mtime
 * and size are monotonically increasing, except when utimes() or
 * truncate() increments the corresponding _seq values on the MDS.
 */
void ceph_fill_file_bits(struct inode *inode, int issued,
			 u64 truncate_seq, u64 size,
			 u64 time_warp_seq, struct timespec *ctime,
			 struct timespec *mtime, struct timespec *atime)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	int warn = 0;

	if (truncate_seq > ci->i_truncate_seq ||
	    (truncate_seq == ci->i_truncate_seq && size > inode->i_size)) {
		dout(10, "size %lld -> %llu\n", inode->i_size, size);
		inode->i_size = size;
		inode->i_blocks = (size + (1<<9) - 1) >> 9;
		ci->i_reported_size = size;
		ci->i_truncate_seq = truncate_seq;
	}

	if (issued & CEPH_CAP_EXCL) {
		/*
		 * if we hold EXCL cap, we have the most up to date
		 * values for everything except possibly ctime.
		 */
		if (timespec_compare(ctime, &inode->i_ctime) > 0)
			inode->i_ctime = *ctime;
		if (time_warp_seq > ci->i_time_warp_seq)
			derr(0, "WARNING: %p mds time_warp_seq %llu > %llu\n",
			     inode, time_warp_seq, ci->i_time_warp_seq);
	} else if (issued & (CEPH_CAP_WR|CEPH_CAP_WRBUFFER)) {
		if (time_warp_seq > ci->i_time_warp_seq) {
			/* the MDS did a utimes() */
			inode->i_ctime = *ctime;
			inode->i_mtime = *mtime;
			inode->i_atime = *atime;
			ci->i_time_warp_seq = time_warp_seq;
		} else if (time_warp_seq == ci->i_time_warp_seq) {
			if (timespec_compare(ctime, &inode->i_ctime) > 0)
				inode->i_ctime = *ctime;
			if (timespec_compare(mtime, &inode->i_mtime) > 0)
				inode->i_mtime = *mtime;
			if (timespec_compare(atime, &inode->i_atime) > 0)
				inode->i_atime = *atime;
		} else
			warn = 1;
	} else {
		/* we have no write caps; whatever the MDS says is true */
		if (time_warp_seq >= ci->i_time_warp_seq) {
			inode->i_ctime = *ctime;
			inode->i_mtime = *mtime;
			inode->i_atime = *atime;
			ci->i_time_warp_seq = time_warp_seq;
		} else
			warn = 1;
	}
	if (warn) /* time_warp_seq shouldn't go backwards */
		dout(10, "%p mds time_warp_seq %llu < %llu\n",
		     inode, time_warp_seq, ci->i_time_warp_seq);
}

/*
 * populate an inode based on info from mds.
 * may be called on new or existing inodes.
 */
int ceph_fill_inode(struct inode *inode,
		    struct ceph_mds_reply_info_in *iinfo,
		    struct ceph_mds_reply_dirfrag *dirinfo)
{
	struct ceph_mds_reply_inode *info = iinfo->in;
	struct ceph_inode_info *ci = ceph_inode(inode);
	int i;
	int issued;
	struct timespec mtime, atime, ctime;
	u32 nsplits;
	void *xattr_data = NULL;
	int err = 0;

	dout(30, "fill_inode %p ino %llx.%llx v %llu had %llu\n",
	     inode, ceph_vinop(inode), le64_to_cpu(info->version),
	     ci->i_version);

	/* prealloc xattr data, if it looks like we'll need it */
	if (iinfo->xattr_len && iinfo->xattr_len != ci->i_xattr_len) {
		xattr_data = kmalloc(iinfo->xattr_len, GFP_NOFS);
		if (!xattr_data)
			derr(10, "ENOMEM on xattr blob %d bytes\n",
			     ci->i_xattr_len);
	}

	spin_lock(&inode->i_lock);

	if (le64_to_cpu(info->version) > 0 &&
	    ci->i_version == le64_to_cpu(info->version))
		goto no_change;

	/* update inode */
	ci->i_version = le64_to_cpu(info->version);
	inode->i_version++;
	inode->i_mode = le32_to_cpu(info->mode);
	inode->i_uid = le32_to_cpu(info->uid);
	inode->i_gid = le32_to_cpu(info->gid);
	inode->i_nlink = le32_to_cpu(info->nlink);
	inode->i_rdev = le32_to_cpu(info->rdev);

	/* be careful with mtime, atime, size */
	ceph_decode_timespec(&atime, &info->atime);
	ceph_decode_timespec(&mtime, &info->mtime);
	ceph_decode_timespec(&ctime, &info->ctime);
	issued = __ceph_caps_issued(ci, NULL);
	ceph_fill_file_bits(inode, issued,
			    le64_to_cpu(info->truncate_seq),
			    le64_to_cpu(info->size),
			    le64_to_cpu(info->time_warp_seq),
			    &ctime, &mtime, &atime);

	ci->i_max_size = le64_to_cpu(info->max_size);
	ci->i_layout = info->layout;
	inode->i_blkbits = fls(le32_to_cpu(info->layout.fl_stripe_unit)) - 1;

	/* xattrs */
	if (iinfo->xattr_len) {
		if (ci->i_xattr_len != iinfo->xattr_len) {
			kfree(ci->i_xattr_data);
			ci->i_xattr_len = iinfo->xattr_len;
			ci->i_xattr_data = xattr_data;
			xattr_data = NULL;
		}
		if (ci->i_xattr_len)
			memcpy(ci->i_xattr_data, iinfo->xattr_data,
			       ci->i_xattr_len);
	}

	ci->i_old_atime = inode->i_atime;

	inode->i_mapping->a_ops = &ceph_aops;
	inode->i_mapping->backing_dev_info = &ceph_client(inode->i_sb)->backing_dev_info;

no_change:
	spin_unlock(&inode->i_lock);

	/* populate frag tree */
	/* FIXME: move me up, if/when version reflects fragtree changes */
	nsplits = le32_to_cpu(info->fragtree.nsplits);
	mutex_lock(&ci->i_fragtree_mutex);
	for (i = 0; i < nsplits; i++) {
		u32 id = le32_to_cpu(info->fragtree.splits[i].frag);
		struct ceph_inode_frag *frag = __get_or_create_frag(ci, id);

		if (IS_ERR(frag))
			continue;
		frag->split_by = le32_to_cpu(info->fragtree.splits[i].by);
		dout(20, " frag %x split by %d\n", frag->frag, frag->split_by);
	}
	mutex_unlock(&ci->i_fragtree_mutex);

	/* update delegation info? */
	if (dirinfo)
		ceph_fill_dirfrag(inode, dirinfo);

	switch (inode->i_mode & S_IFMT) {
	case S_IFIFO:
	case S_IFBLK:
	case S_IFCHR:
	case S_IFSOCK:
		init_special_inode(inode, inode->i_mode, inode->i_rdev);
		inode->i_op = &ceph_file_iops;
		break;
	case S_IFREG:
		inode->i_op = &ceph_file_iops;
		inode->i_fop = &ceph_file_fops;
		break;
	case S_IFLNK:
		inode->i_op = &ceph_symlink_iops;
		if (!ci->i_symlink) {
			int symlen = iinfo->symlink_len;

			BUG_ON(symlen != inode->i_size);
			err = -ENOMEM;
			ci->i_symlink = kmalloc(symlen+1, GFP_NOFS);
			if (!ci->i_symlink)
				goto out;
			memcpy(ci->i_symlink, iinfo->symlink, symlen);
			ci->i_symlink[symlen] = 0;
		}
		break;
	case S_IFDIR:
		inode->i_op = &ceph_dir_iops;
		inode->i_fop = &ceph_dir_fops;

		ci->i_files = le64_to_cpu(info->files);
		ci->i_subdirs = le64_to_cpu(info->subdirs);
		ci->i_rbytes = le64_to_cpu(info->rbytes);
		ci->i_rfiles = le64_to_cpu(info->rfiles);
		ci->i_rsubdirs = le64_to_cpu(info->rsubdirs);
		ceph_decode_timespec(&ci->i_rctime, &info->rctime);

		/* it may be better to set st_size in getattr instead? */
		if (ceph_client(inode->i_sb)->mount_args.flags &
		    CEPH_MOUNT_RBYTES)
			inode->i_size = ci->i_rbytes;
		break;
	default:
		derr(0, "BAD mode 0%o S_IFMT 0%o\n", inode->i_mode,
		     inode->i_mode & S_IFMT);
		err = -EINVAL;
		goto out;
	}
	err = 0;

out:
	kfree(xattr_data);
	return err;
}



/*
 * caller must hold session s_mutex.
 */
static int update_inode_lease(struct inode *inode,
			      struct ceph_mds_reply_lease *lease,
			      struct ceph_mds_session *session,
			      unsigned long from_time)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	int is_new = 0;
	int mask = le16_to_cpu(lease->mask);
	long unsigned duration = le32_to_cpu(lease->duration_ms);
	long unsigned ttl = from_time + (duration * HZ) / 1000;

	dout(10, "update_inode_lease %p mask %d duration %lu ms ttl %lu\n",
	     inode, mask, duration, ttl);

	if (mask == 0)
		return 0;

	spin_lock(&inode->i_lock);
	/*
	 * be careful: we can't remove a lease from a different session
	 * without holding the other session's s_mutex.  and we only
	 * remember one lease per object.  so if one already exists,
	 * don't touch it.
	 */
	if ((ci->i_lease_ttl == 0 || !time_before(ttl, ci->i_lease_ttl) ||
	     ci->i_lease_gen != session->s_cap_gen) &&
	    (!ci->i_lease_session || ci->i_lease_session == session)) {
		ci->i_lease_ttl = ttl;
		ci->i_lease_gen = session->s_cap_gen;
		ci->i_lease_mask = mask;
		if (!ci->i_lease_session) {
			ci->i_lease_session = session;
			is_new = 1;
		}
		list_move_tail(&ci->i_lease_item, &session->s_inode_leases);
	} else
		mask = 0;
	spin_unlock(&inode->i_lock);
	if (is_new)
		igrab(inode);
	return mask;
}

/*
 * check if inode lease is valid for a given mask
 */
int ceph_inode_lease_valid(struct inode *inode, int mask)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	int havemask;
	int valid = 0;
	int ret = 0;

	spin_lock(&inode->i_lock);
	havemask = ci->i_lease_mask;

	/* EXCL cap counts for an ICONTENT lease... check caps? */
	if ((mask & CEPH_LOCK_ICONTENT) &&
	    __ceph_caps_issued(ci, NULL) & CEPH_CAP_EXCL) {
		dout(20, "lease_valid inode %p EXCL cap -> ICONTENT\n", inode);
		havemask |= CEPH_LOCK_ICONTENT;
	}
	/* any ICONTENT bits imply all ICONTENT bits */
	if (havemask & CEPH_LOCK_ICONTENT)
		havemask |= CEPH_LOCK_ICONTENT;

	if (ci->i_lease_session) {
		struct ceph_mds_session *s = ci->i_lease_session;
		spin_lock(&s->s_cap_lock);
		if (s->s_cap_gen == ci->i_lease_gen &&
		    time_before(jiffies, s->s_cap_ttl) &&
		    time_before(jiffies, ci->i_lease_ttl))
			valid = 1;
		spin_unlock(&s->s_cap_lock);
	}
	spin_unlock(&inode->i_lock);

	if (valid && (havemask & mask) == mask)
		ret = 1;

	dout(10, "lease_valid inode %p have %d want %d valid %d = %d\n", inode,
	     havemask, mask, valid, ret);
	return ret;
}


/*
 * caller should hold session s_mutex.
 */
static void update_dentry_lease(struct dentry *dentry,
				struct ceph_mds_reply_lease *lease,
				struct ceph_mds_session *session,
				unsigned long from_time)
{
	struct ceph_dentry_info *di;
	int is_new = 0;
	long unsigned duration = le32_to_cpu(lease->duration_ms);
	long unsigned ttl = from_time + (duration * HZ) / 1000;

	dout(10, "update_dentry_lease %p mask %d duration %lu ms ttl %lu\n",
	     dentry, le16_to_cpu(lease->mask), duration, ttl);
	if (lease->mask == 0) {
		/*
		 * no per-dentry lease.  so, set d_time to match
		 * parent directory version.  if/when we get an
		 * ICONTENT cap (implicit directory-wide lease), we'll
		 * know whether it covers this dentry.
		 */
		struct inode *dir = dentry->d_parent->d_inode;
		dentry->d_time = ceph_inode(dir)->i_version;
		dout(20, " no lease, setting d_time to %lu\n", dentry->d_time);
		return;
	}

	spin_lock(&dentry->d_lock);
	di = ceph_dentry(dentry);
	if (dentry->d_time != 0 &&
	    di && di->lease_gen == session->s_cap_gen &&
	    time_before(ttl, dentry->d_time))
		goto fail_unlock;  /* we already have a newer lease. */

	if (!di) {
		spin_unlock(&dentry->d_lock);
		di = kmalloc(sizeof(struct ceph_dentry_info),
			     GFP_NOFS);
		if (!di)
			return;          /* oh well */
		spin_lock(&dentry->d_lock);
		if (dentry->d_fsdata) {
			kfree(di);   /* lost a race! */
			goto fail_unlock;
		}
		di->dentry = dentry;
		dentry->d_fsdata = di;
		di->lease_session = session;
		di->lease_gen = session->s_cap_gen;
		list_add(&di->lease_item, &session->s_dentry_leases);
		is_new = 1;
	} else {
		/* touch existing */
		if (di->lease_session != session)
			goto fail_unlock;
		list_move_tail(&di->lease_item, &session->s_dentry_leases);
	}
	dentry->d_time = ttl;
	spin_unlock(&dentry->d_lock);
	if (is_new) {
		dout(10, "lease dget on %p\n", dentry);
		dget(dentry);
	}
	return;

fail_unlock:
	spin_unlock(&dentry->d_lock);
}

/*
 * check if dentry lease is valid
 */
int ceph_dentry_lease_valid(struct dentry *dentry)
{
	struct ceph_dentry_info *di;
	struct ceph_mds_session *s;
	int valid = 0;
	u32 gen;
	unsigned long ttl;

	spin_lock(&dentry->d_lock);
	di = ceph_dentry(dentry);
	if (di) {
		s = di->lease_session;
		spin_lock(&s->s_cap_lock);
		gen = s->s_cap_gen;
		ttl = s->s_cap_ttl;
		spin_unlock(&s->s_cap_lock);

		if (di->lease_gen == gen &&
		    time_before(jiffies, dentry->d_time) &&
		    time_before(jiffies, ttl))
			valid = 1;
	}
	spin_unlock(&dentry->d_lock);
	dout(20, "dentry_lease_valid - dentry %p = %d\n", dentry, valid);
	return valid;
}


/*
 * splice a dentry to an inode.
 * caller must hold directory i_mutex for this to be safe.
 *
 * we will only rehash the resulting dentry if @prehash is
 * true; @prehash will be set to false (for the benefit of
 * the caller) if we fail.
 */
static struct dentry *splice_dentry(struct dentry *dn, struct inode *in,
				    bool *prehash)
{
	struct dentry *realdn;

	/* dn must be unhashed */
	if (!d_unhashed(dn))
		d_drop(dn);
	realdn = d_materialise_unique(dn, in);
	if (IS_ERR(realdn)) {
		derr(0, "error splicing %p (%d) inode %p ino %llx.%llx\n",
		     dn, atomic_read(&dn->d_count), in, ceph_vinop(in));
		if (prehash)
			*prehash = false; /* don't rehash on error */
		goto out;
	} else if (realdn) {
		dout(10, "dn %p (%d) spliced with %p (%d) "
		     "inode %p ino %llx.%llx\n",
		     dn, atomic_read(&dn->d_count),
		     realdn, atomic_read(&realdn->d_count),
		     realdn->d_inode, ceph_vinop(realdn->d_inode));
		dput(dn);
		dn = realdn;
		ceph_init_dentry(dn);
	} else
		dout(10, "dn %p attached to %p ino %llx.%llx\n",
		     dn, dn->d_inode, ceph_vinop(dn->d_inode));
	if ((!prehash || *prehash) && d_unhashed(dn))
		d_rehash(dn);
out:
	return dn;
}

/*
 * Assimilate a full trace of inodes and dentries, from the root to
 * the item relevant for this reply, into our cache.  Make any dcache
 * changes needed to properly reflect the completed operation (e.g.,
 * call d_move).  Make note of the distribution of metadata across the
 * mds cluster.
 *
 * Care is taken to (attempt to) take i_mutex before adjusting dentry
 * linkages or leases.
 *
 * FIXME: we should check inode.version to avoid races between traces
 * from multiple MDSs after, say, a ancestor directory is renamed.
 */
int ceph_fill_trace(struct super_block *sb, struct ceph_mds_request *req,
		    struct ceph_mds_session *session)
{
	struct ceph_mds_reply_info *rinfo = &req->r_reply_info;
	int err = 0, mask;
	struct qstr dname;
	struct dentry *dn = sb->s_root;
	struct dentry *parent = NULL;
	struct dentry *existing;
	struct inode *in;
	struct ceph_mds_reply_inode *ininfo;
	int d = 0;
	struct ceph_vino vino;
	int have_icontent;
	bool have_lease;

	if (rinfo->trace_numi == 0) {
		dout(10, "fill_trace reply has empty trace!\n");
		return 0;
	}

#if 0
	/*
	 * if we resend completed ops to a recovering mds, we get no
	 * trace.  pretend this is the case to ensure the 'no trace'
	 * handlers in the callers behave.
	 */
	if (rinfo->head->op & CEPH_MDS_OP_WRITE) {
		dout(0, "fill_trace faking empty trace on %d %s\n",
		     rinfo->head->op,
		     ceph_mds_op_name(rinfo->head->op));
		rinfo->trace_numi = 0;
		rinfo->trace_numd = 0;
		return 0;
	}
#endif

	vino.ino = le64_to_cpu(rinfo->trace_in[0].in->ino);
	vino.snap = le64_to_cpu(rinfo->trace_in[0].in->snapid);
	if (likely(dn)) {
		in = dn->d_inode;
		/* trace should start at root, or have only 1 dentry
		 * (if it is in an mds stray dir) */
		WARN_ON(vino.ino != 1 && rinfo->trace_numd != 1);
	} else {
		/* first reply (i.e. we just mounted) */
		in = ceph_get_inode(sb, vino);
		if (IS_ERR(in))
			return PTR_ERR(in);
		dn = d_alloc_root(in);
		if (dn == NULL) {
			derr(0, "d_alloc_root ENOMEM badness on root dentry\n");
			return -ENOMEM;
		}
	}

	if (vino.ino == 1) {
		err = ceph_fill_inode(in, &rinfo->trace_in[0],
				      rinfo->trace_numd ?
				      rinfo->trace_dir[0]:NULL);
		if (err < 0)
			return err;
		if (rinfo->trace_numd == 0)
			update_inode_lease(in, rinfo->trace_ilease[0],
					   session, req->r_request_started);
		if (unlikely(sb->s_root == NULL))
			sb->s_root = dn;
	}

	have_icontent = 0;
	have_lease = 0;
	dget(dn);
	for (d = 0; d < rinfo->trace_numd; d++) {
		dname.name = rinfo->trace_dname[d];
		dname.len = rinfo->trace_dname_len[d];
		parent = dn;
		dn = NULL;

		dout(10, "fill_trace %d/%d parent %p inode %p: '%.*s'"
		     " ic %d dmask %d\n",
		     (d+1), rinfo->trace_numd, parent, parent->d_inode,
		     (int)dname.len, dname.name,
		     have_icontent, rinfo->trace_dlease[d]->mask);

		/* try to take dir i_mutex */
		if (req->r_locked_dir != parent->d_inode &&
		    mutex_trylock(&parent->d_inode->i_mutex) == 0) {
			dout(0, "fill_trace  FAILED to take %p i_mutex\n",
			     parent->d_inode);
			goto no_dir_mutex;
		}

		/* update inode lease */
		mask = update_inode_lease(in, rinfo->trace_ilease[d],
					  session, req->r_request_started);
		have_icontent = mask & CEPH_LOCK_ICONTENT;

		/* do we have a dn lease? */
		have_lease = have_icontent ||
			(rinfo->trace_dlease[d]->mask & CEPH_LOCK_DN);
		if (!have_lease)
			dout(10, "fill_trace  no icontent|dentry lease\n");

		dout(10, "fill_trace  took %p i_mutex\n", parent->d_inode);

		dname.hash = full_name_hash(dname.name, dname.len);
	retry_lookup:
		/* existing dentry? */
		dn = d_lookup(parent, &dname);
		dout(10, "fill_trace d_lookup of '%.*s' got %p\n",
		     (int)dname.len, dname.name, dn);

		/* use caller provided dentry?  for simplicity,
		 *  - only if there is no existing dn, and
		 *  - only if parent is correct
		 */
		if (d == rinfo->trace_numd-1 && req->r_last_dentry) {
			if (!dn && req->r_last_dentry->d_parent == parent) {
				dn = req->r_last_dentry;
				dout(10, "fill_trace provided dn %p '%.*s'\n",
				     dn, dn->d_name.len, dn->d_name.name);
				ceph_init_dentry(dn);  /* just in case */
			} else if (dn == req->r_last_dentry) {
				dout(10, "fill_trace matches provided dn %p\n",
				     dn);
				dput(req->r_last_dentry);
			} else {
				dout(10, "fill_trace NOT using provided dn %p "
				     "(parent %p)\n", req->r_last_dentry,
				     req->r_last_dentry->d_parent);
				dput(req->r_last_dentry);
			}
			req->r_last_dentry = NULL;
		}

		if (!dn) {
			dn = d_alloc(parent, &dname);
			if (!dn) {
				derr(0, "d_alloc ENOMEM\n");
				err = -ENOMEM;
				goto out_dir_no_inode;
			}
			dout(10, "fill_trace d_alloc %p '%.*s'\n", dn,
			     dn->d_name.len, dn->d_name.name);
			ceph_init_dentry(dn);
		}
		BUG_ON(!dn);

		/* null dentry? */
		if (d+1 == rinfo->trace_numi) {
			dout(10, "fill_trace null dentry\n");
			if (dn->d_inode) {
				dout(20, "d_delete %p\n", dn);
				d_delete(dn);
				dput(dn);
				goto retry_lookup;
			}
			dout(20, "d_instantiate %p NULL\n", dn);
			d_instantiate(dn, NULL);
			if (have_lease && d_unhashed(dn))
				d_rehash(dn);
			update_dentry_lease(dn, rinfo->trace_dlease[d],
					    session, req->r_request_started);
			goto out_dir_no_inode;
		}

		/* rename? */
		if (d == rinfo->trace_numd-1 && req->r_old_dentry) {
			dout(10, " src %p '%.*s' dst %p '%.*s'\n",
			     req->r_old_dentry,
			     req->r_old_dentry->d_name.len,
			     req->r_old_dentry->d_name.name,
			     dn, dn->d_name.len, dn->d_name.name);
			dout(10, "fill_trace doing d_move %p -> %p\n",
			     req->r_old_dentry, dn);
			d_move(req->r_old_dentry, dn);
			dout(10, " src %p '%.*s' dst %p '%.*s'\n",
			     req->r_old_dentry,
			     req->r_old_dentry->d_name.len,
			     req->r_old_dentry->d_name.name,
			     dn, dn->d_name.len, dn->d_name.name);
			dput(dn);  /* dn is dropped */
			dn = req->r_old_dentry;  /* use old_dentry */
			req->r_old_dentry = NULL;
		}

		/* attach proper inode */
		ininfo = rinfo->trace_in[d+1].in;
		vino.ino = le64_to_cpu(ininfo->ino);
		vino.snap = le64_to_cpu(ininfo->snapid);
		if (dn->d_inode) {
			if (ceph_ino(dn->d_inode) != vino.ino ||
			    ceph_snap(dn->d_inode) != vino.snap) {
				dout(10, "dn %p wrong inode %p ino %llx.%llx\n",
				     dn, dn->d_inode, ceph_vinop(dn->d_inode));
				d_delete(dn);
				dput(dn);
				goto retry_lookup;
			}
			dout(10, "dn %p correct %p ino %llx.%llx\n",
			     dn, dn->d_inode, ceph_vinop(dn->d_inode));
			in = dn->d_inode;
		} else {
			in = ceph_get_inode(dn->d_sb, vino);
			if (IS_ERR(in)) {
				derr(30, "get_inode badness\n");
				err = PTR_ERR(in);
				d_delete(dn);
				dn = NULL;
				goto out_dir_no_inode;
			}
			dn = splice_dentry(dn, in, &have_lease);
		}

		if (have_lease)
			update_dentry_lease(dn, rinfo->trace_dlease[d],
					    session, req->r_request_started);

		/* done with dn update */
		if (req->r_locked_dir != parent->d_inode)
			mutex_unlock(&parent->d_inode->i_mutex);

	update_inode:
		BUG_ON(dn->d_inode != in);
		err = ceph_fill_inode(in,
				      &rinfo->trace_in[d+1],
				      rinfo->trace_numd <= d ?
				      rinfo->trace_dir[d+1]:NULL);
		if (err < 0) {
			derr(30, "ceph_fill_inode badness\n");
			d_delete(dn);
			dn = NULL;
			in = NULL;
			break;
		}

		dput(parent);
		parent = NULL;

		/* do we diverge into a snap dir at this point in the trace? */
		if (d == rinfo->trace_numi - rinfo->trace_snapdirpos - 1) {
			struct inode *snapdir = ceph_get_snapdir(in);
			dput(dn);
			dn = d_find_alias(snapdir);
			if (!dn) {
				struct ceph_client *client =
					ceph_sb_to_client(parent->d_sb);

				dname.name = client->mount_args.snapdir_name,
				dname.len = strlen(dname.name);
				dname.hash = full_name_hash(dname.name,
							    dname.len);
				dn = d_alloc(parent, &dname);
				if (!dn) {
					err = -ENOMEM;
					iput(snapdir);
					break;
				}
				d_add(dn, snapdir);
			}
			iput(snapdir);
			dout(10, " snapdir dentry is %p\n", dn);
		}
		continue;


	out_dir_no_inode:
		/* drop i_mutex */
		if (req->r_locked_dir != parent->d_inode)
			mutex_unlock(&parent->d_inode->i_mutex);
		in = NULL;
		break;


	no_dir_mutex:
		/*
		 * we couldn't take i_mutex for this dir, so do not
		 * lookup or relink any existing dentry.
		 */
		if (d == rinfo->trace_numd-1 && req->r_last_dentry) {
			dn = req->r_last_dentry;
			dout(10, "fill_trace using provided dn %p\n", dn);
			ceph_init_dentry(dn);
			req->r_last_dentry = NULL;
		}

		/* null dentry? */
		if (d+1 == rinfo->trace_numi) {
			if (dn && dn->d_inode)
				d_delete(dn);
			in = NULL;
			break;
		}

		/* find existing inode */
		ininfo = rinfo->trace_in[d+1].in;
		vino.ino = le64_to_cpu(ininfo->ino);
		vino.snap = le64_to_cpu(ininfo->snapid);
		in = ceph_get_inode(parent->d_sb, vino);
		if (IS_ERR(in)) {
			derr(30, "ceph_get_inode badness\n");
			err = PTR_ERR(in);
			in = NULL;
			break;
		}
		existing = d_find_alias(in);
		if (existing) {
			if (dn)
				dput(dn);
			iput(in);
			dn = existing;
			dout(10, " using existing %p alias %p\n", in, dn);
		} else {
			if (dn && dn->d_inode == NULL) {
				dout(10, " instantiating provided %p\n", dn);
				d_instantiate(dn, in);
			} else {
				if (dn) {
					dout(10, " ignoring provided dn %p\n",
					     dn);
					dput(dn);
				}
				dn = d_alloc_anon(in);
				iput(in);
				dout(10, " d_alloc_anon new dn %p\n", dn);
			}
		}
		goto update_inode;
	}
	if (parent)
		dput(parent);

	if (in)
		update_inode_lease(dn->d_inode,
				   rinfo->trace_ilease[d],
				   session, req->r_request_started);

	dout(10, "fill_trace done err=%d, last dn %p in %p\n", err, dn, in);
	if (req->r_last_dentry)
		dput(req->r_last_dentry);
	req->r_last_dentry = dn;
	if (req->r_last_inode)
		iput(req->r_last_inode);
	req->r_last_inode = in;
	if (in)
		igrab(in);
	return err;
}

/*
 * prepopulate cache with readdir results, leases, etc.
 */
int ceph_readdir_prepopulate(struct ceph_mds_request *req)
{
	struct dentry *parent = req->r_last_dentry;
	struct ceph_mds_reply_info *rinfo = &req->r_reply_info;
	struct qstr dname;
	struct dentry *dn;
	struct inode *in;
	int err = 0, i;
	struct inode *snapdir = NULL;

	if (le32_to_cpu(rinfo->head->op) == CEPH_MDS_OP_LSSNAP) {
		snapdir = ceph_get_snapdir(parent->d_inode);
		parent = d_find_alias(snapdir);
		dout(10, "readdir_prepopulate %d items under SNAPDIR dn %p\n",
		     rinfo->dir_nr, parent);
	} else {
		dout(10, "readdir_prepopulate %d items under dn %p\n",
		     rinfo->dir_nr, parent);
		if (rinfo->dir_dir)
			ceph_fill_dirfrag(parent->d_inode, rinfo->dir_dir);
	}

	for (i = 0; i < rinfo->dir_nr; i++) {
		struct ceph_vino vino;

		dname.name = rinfo->dir_dname[i];
		dname.len = le32_to_cpu(rinfo->dir_dname_len[i]);
		dname.hash = full_name_hash(dname.name, dname.len);

		vino.ino = le64_to_cpu(rinfo->dir_in[i].in->ino);
		vino.snap = le64_to_cpu(rinfo->dir_in[i].in->snapid);

retry_lookup:
		dn = d_lookup(parent, &dname);
		dout(30, "d_lookup on parent=%p name=%.*s got %p\n",
		     parent, dname.len, dname.name, dn);

		if (!dn) {
			dn = d_alloc(parent, &dname);
			dout(40, "d_alloc %p '%.*s' = %p\n", parent,
			     dname.len, dname.name, dn);
			if (dn == NULL) {
				dout(30, "d_alloc badness\n");
				err = -ENOMEM;
				goto out;
			}
			ceph_init_dentry(dn);
		} else if (dn->d_inode &&
			   (ceph_ino(dn->d_inode) != vino.ino ||
			    ceph_snap(dn->d_inode) != vino.snap)) {
			dout(10, " dn %p points to wrong inode %p\n",
			     dn, dn->d_inode);
			d_delete(dn);
			dput(dn);
			goto retry_lookup;
		}

		/* inode */
		if (dn->d_inode)
			in = dn->d_inode;
		else {
			in = ceph_get_inode(parent->d_sb, vino);
			if (in == NULL) {
				dout(30, "new_inode badness\n");
				d_delete(dn);
				dput(dn);
				err = -ENOMEM;
				goto out;
			}
			dn = splice_dentry(dn, in, NULL);
		}

		if (ceph_fill_inode(in, &rinfo->dir_in[i], NULL) < 0) {
			dout(0, "ceph_fill_inode badness on %p\n", in);
			dput(dn);
			continue;
		}
		update_dentry_lease(dn, rinfo->dir_dlease[i],
				    req->r_session, req->r_request_started);
		update_inode_lease(in, rinfo->dir_ilease[i],
				   req->r_session, req->r_request_started);
		dput(dn);
	}

out:
	if (snapdir) {
		iput(snapdir);
		dput(parent);
	}
	dout(10, "readdir_prepopulate done\n");
	return err;
}


void ceph_inode_set_size(struct inode *inode, loff_t size)
{
	struct ceph_inode_info *ci = ceph_inode(inode);

	spin_lock(&inode->i_lock);
	dout(30, "set_size %p %llu -> %llu\n", inode, inode->i_size, size);
	inode->i_size = size;
	inode->i_blocks = (size + (1 << 9) - 1) >> 9;

	/* tell the MDS if we are approaching max_size */
	if ((size << 1) >= ci->i_max_size &&
	    (ci->i_reported_size << 1) < ci->i_max_size) {
		spin_unlock(&inode->i_lock);
		ceph_check_caps(ci, 0);
	} else
		spin_unlock(&inode->i_lock);
}

/*
 * Drop open file reference.  If we were the last open file,
 * we may need to release capabilities to the MDS (or schedule
 * their delayed release).
 */
void ceph_put_fmode(struct ceph_inode_info *ci, int fmode)
{
	int last = 0;

	spin_lock(&ci->vfs_inode.i_lock);
	dout(20, "put_mode %p fmode %d %d -> %d\n", &ci->vfs_inode, fmode,
	     ci->i_nr_by_mode[fmode], ci->i_nr_by_mode[fmode]-1);
	if (--ci->i_nr_by_mode[fmode] == 0)
		last++;
	spin_unlock(&ci->vfs_inode.i_lock);

	if (last && ci->i_vino.snap == CEPH_NOSNAP)
		ceph_check_caps(ci, 0);
}


/*
 * Write back inode data in a worker thread.  (This can't be done
 * in the message handler context.)
 */
void ceph_inode_writeback(struct work_struct *work)
{
	struct ceph_inode_info *ci = container_of(work, struct ceph_inode_info,
						  i_wb_work);
	struct inode *inode = &ci->vfs_inode;

	dout(10, "writeback %p\n", inode);
	filemap_write_and_wait(&inode->i_data);
}


/*
 * called by trunc_wq; take i_mutex ourselves
 *
 * We also truncation in a separate thread as well.
 */
void ceph_vmtruncate_work(struct work_struct *work)
{
	struct ceph_inode_info *ci = container_of(work, struct ceph_inode_info,
						  i_vmtruncate_work);
	struct inode *inode = &ci->vfs_inode;

	dout(10, "vmtruncate_work %p\n", inode);
	mutex_lock(&inode->i_mutex);
	__ceph_do_pending_vmtruncate(inode);
	mutex_unlock(&inode->i_mutex);
}

/*
 * called with i_mutex held.
 *
 * Make sure any pending truncation is applied before doing anything
 * that may depend on it.
 */
void __ceph_do_pending_vmtruncate(struct inode *inode)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	loff_t to;
	int wrbuffer_refs;

	spin_lock(&inode->i_lock);
	to = ci->i_vmtruncate_to;
	ci->i_vmtruncate_to = -1;
	wrbuffer_refs = ci->i_wrbuffer_ref;
	spin_unlock(&inode->i_lock);

	if (to >= 0) {
		dout(10, "__do_pending_vmtruncate %p to %lld\n", inode, to);
		truncate_inode_pages(inode->i_mapping, to);
		if (wrbuffer_refs == 0)
			ceph_check_caps(ci, 0);
	} else
		dout(10, "__do_pending_vmtruncate %p nothing to do\n", inode);
}

/*
 * symlinks
 */
static void *ceph_sym_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	struct ceph_inode_info *ci = ceph_inode(dentry->d_inode);
	nd_set_link(nd, ci->i_symlink);
	return NULL;
}

static const struct inode_operations ceph_symlink_iops = {
	.readlink = generic_readlink,
	.follow_link = ceph_sym_follow_link,
};


/*
 * Prepare a setattr request.  If we know we have the file open (and
 * thus hold at lease a PIN capability), generate the request without
 * a path name.
 */
static struct ceph_mds_request *prepare_setattr(struct ceph_mds_client *mdsc,
						struct dentry *dentry,
						int ia_valid, int op)
{
	char *path;
	int pathlen;
	struct ceph_mds_request *req;
	u64 pathbase;

	if (ia_valid & ATTR_FILE) {
		dout(5, "prepare_setattr dentry %p (inode %llx.%llx)\n", dentry,
		     ceph_vinop(dentry->d_inode));
		req = ceph_mdsc_create_request(mdsc, op,
					       ceph_ino(dentry->d_inode),
					       "", 0, NULL,
					       dentry, USE_CAP_MDS);
	} else {
		dout(5, "prepare_setattr dentry %p (full path)\n", dentry);
		path = ceph_build_path(dentry, &pathlen, &pathbase, 0);
		if (IS_ERR(path))
			return ERR_PTR(PTR_ERR(path));
		req = ceph_mdsc_create_request(mdsc, op, pathbase, path, 0, NULL,
					       dentry, USE_ANY_MDS);
		kfree(path);
	}
	return req;
}

static int ceph_setattr_chown(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	struct ceph_client *client = ceph_sb_to_client(inode->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	const unsigned int ia_valid = attr->ia_valid;
	struct ceph_mds_request *req;
	struct ceph_mds_request_head *reqh;
	int err;
	int mask = 0;

	req = prepare_setattr(mdsc, dentry, ia_valid, CEPH_MDS_OP_CHOWN);
	if (IS_ERR(req))
		return PTR_ERR(req);
	reqh = req->r_request->front.iov_base;
	if (ia_valid & ATTR_UID) {
		reqh->args.chown.uid = cpu_to_le32(attr->ia_uid);
		mask |= CEPH_CHOWN_UID;
	}
	if (ia_valid & ATTR_GID) {
		reqh->args.chown.gid = cpu_to_le32(attr->ia_gid);
		mask |= CEPH_CHOWN_GID;
	}
	reqh->args.chown.mask = cpu_to_le32(mask);
	ceph_mdsc_lease_release(mdsc, inode, NULL, CEPH_LOCK_IAUTH);
	err = ceph_mdsc_do_request(mdsc, req);
	ceph_mdsc_put_request(req);
	dout(10, "chown result %d\n", err);
	return err;
}

static int ceph_setattr_chmod(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	struct ceph_client *client = ceph_sb_to_client(inode->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req;
	struct ceph_mds_request_head *reqh;
	int err;

	req = prepare_setattr(mdsc, dentry, attr->ia_valid, CEPH_MDS_OP_LCHMOD);
	if (IS_ERR(req))
		return PTR_ERR(req);
	reqh = req->r_request->front.iov_base;
	reqh->args.chmod.mode = cpu_to_le32(attr->ia_mode);
	ceph_mdsc_lease_release(mdsc, inode, NULL, CEPH_LOCK_IAUTH);
	err = ceph_mdsc_do_request(mdsc, req);
	ceph_mdsc_put_request(req);
	dout(10, "chmod result %d\n", err);
	return err;
}

static int ceph_setattr_time(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct ceph_client *client = ceph_sb_to_client(inode->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	const unsigned int ia_valid = attr->ia_valid;
	struct ceph_mds_request *req;
	struct ceph_mds_request_head *reqh;
	int err;

	/* if i hold CAP_EXCL, i can change [am]time any way i like */
	if (ceph_caps_issued(ci) & CEPH_CAP_EXCL) {
		dout(10, "utime holding EXCL, doing locally\n");
		ci->i_time_warp_seq++;
		if (ia_valid & ATTR_ATIME)
			inode->i_atime = attr->ia_atime;
		if (ia_valid & ATTR_MTIME)
			inode->i_mtime = attr->ia_mtime;
		inode->i_ctime = CURRENT_TIME;
		return 0;
	}

	/* if i hold CAP_WR, i can _increase_ [am]time safely */
	if ((ceph_caps_issued(ci) & CEPH_CAP_WR) &&
	    ((ia_valid & ATTR_MTIME) == 0 ||
	     timespec_compare(&inode->i_mtime, &attr->ia_mtime) < 0) &&
	    ((ia_valid & ATTR_ATIME) == 0 ||
	     timespec_compare(&inode->i_atime, &attr->ia_atime) < 0)) {
		dout(10, "utime holding WR, doing [am]time increase locally\n");
		if (ia_valid & ATTR_ATIME)
			inode->i_atime = attr->ia_atime;
		if (ia_valid & ATTR_MTIME)
			inode->i_mtime = attr->ia_mtime;
		inode->i_ctime = CURRENT_TIME;
		return 0;
	}

	/* if i have valid values, this may be a no-op */
	if (ceph_inode_lease_valid(inode, CEPH_LOCK_ICONTENT) &&
	    !(((ia_valid & ATTR_ATIME) &&
	       !timespec_equal(&inode->i_atime, &attr->ia_atime)) ||
	      ((ia_valid & ATTR_MTIME) &&
	       !timespec_equal(&inode->i_mtime, &attr->ia_mtime)))) {
		dout(10, "lease indicates utimes is a no-op\n");
		return 0;
	}

	req = prepare_setattr(mdsc, dentry, ia_valid, CEPH_MDS_OP_LUTIME);
	if (IS_ERR(req))
		return PTR_ERR(req);
	reqh = req->r_request->front.iov_base;
	ceph_encode_timespec(&reqh->args.utime.mtime, &attr->ia_mtime);
	ceph_encode_timespec(&reqh->args.utime.atime, &attr->ia_atime);

	reqh->args.utime.mask = 0;
	if (ia_valid & ATTR_ATIME)
		reqh->args.utime.mask |= CEPH_UTIME_ATIME;
	if (ia_valid & ATTR_MTIME)
		reqh->args.utime.mask |= CEPH_UTIME_MTIME;

	ceph_mdsc_lease_release(mdsc, inode, NULL, CEPH_LOCK_ICONTENT);
	err = ceph_mdsc_do_request(mdsc, req);
	ceph_mdsc_put_request(req);
	dout(10, "utime result %d\n", err);
	return err;
}

static int ceph_setattr_size(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct ceph_client *client = ceph_sb_to_client(inode->i_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	const unsigned int ia_valid = attr->ia_valid;
	struct ceph_mds_request *req;
	struct ceph_mds_request_head *reqh;
	int err;

	dout(10, "truncate: ia_size %d i_size %d\n",
	     (int)attr->ia_size, (int)inode->i_size);
	if (ceph_caps_issued(ci) & CEPH_CAP_EXCL &&
	    attr->ia_size > inode->i_size) {
		dout(10, "holding EXCL, doing truncate (fwd) locally\n");
		err = vmtruncate(inode, attr->ia_size);
		if (err)
			return err;
		spin_lock(&inode->i_lock);
		inode->i_size = attr->ia_size;
		inode->i_ctime = attr->ia_ctime;
		ci->i_reported_size = attr->ia_size;
		spin_unlock(&inode->i_lock);
		return 0;
	}
	if (ceph_inode_lease_valid(inode, CEPH_LOCK_ICONTENT) &&
	    attr->ia_size == inode->i_size) {
		dout(10, "lease indicates truncate is a no-op\n");
		return 0;
	}
	req = prepare_setattr(mdsc, dentry, ia_valid, CEPH_MDS_OP_LTRUNCATE);
	if (IS_ERR(req))
		return PTR_ERR(req);
	reqh = req->r_request->front.iov_base;
	reqh->args.truncate.length = cpu_to_le64(attr->ia_size);
	ceph_mdsc_lease_release(mdsc, inode, NULL, CEPH_LOCK_ICONTENT);
	err = ceph_mdsc_do_request(mdsc, req);
	ceph_mdsc_put_request(req);
	dout(10, "truncate result %d\n", err);
	__ceph_do_pending_vmtruncate(inode);
	return err;
}


int ceph_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	const unsigned int ia_valid = attr->ia_valid;
	int err;

	if (ceph_snap(inode) != CEPH_NOSNAP)
		return -EROFS;

	__ceph_do_pending_vmtruncate(inode);

	err = inode_change_ok(inode, attr);
	if (err != 0)
		return err;

	/* gratuitous debug output */
	if (ia_valid & ATTR_UID)
		dout(10, "setattr: %p uid %d -> %d\n", inode,
		     inode->i_uid, attr->ia_uid);
	if (ia_valid & ATTR_GID)
		dout(10, "setattr: %p gid %d -> %d\n", inode,
		     inode->i_uid, attr->ia_uid);
	if (ia_valid & ATTR_MODE)
		dout(10, "setattr: %p mode 0%o -> 0%o\n", inode, inode->i_mode,
		     attr->ia_mode);
	if (ia_valid & ATTR_SIZE)
		dout(10, "setattr: %p size %lld -> %lld\n", inode,
		     inode->i_size, attr->ia_size);
	if (ia_valid & ATTR_ATIME)
		dout(10, "setattr: %p atime %ld.%ld -> %ld.%ld\n", inode,
		     inode->i_atime.tv_sec, inode->i_atime.tv_nsec,
		     attr->ia_atime.tv_sec, attr->ia_atime.tv_nsec);
	if (ia_valid & ATTR_MTIME)
		dout(10, "setattr: %p mtime %ld.%ld -> %ld.%ld\n", inode,
		     inode->i_mtime.tv_sec, inode->i_mtime.tv_nsec,
		     attr->ia_mtime.tv_sec, attr->ia_mtime.tv_nsec);
	if (ia_valid & ATTR_MTIME)
		dout(10, "setattr: %p ctime %ld.%ld -> %ld.%ld\n", inode,
		     inode->i_ctime.tv_sec, inode->i_ctime.tv_nsec,
		     attr->ia_ctime.tv_sec, attr->ia_ctime.tv_nsec);
	if (ia_valid & ATTR_FILE)
		dout(10, "setattr: %p ATTR_FILE ... hrm!\n", inode);

	if (ia_valid & (ATTR_UID|ATTR_GID))
		err = ceph_setattr_chown(dentry, attr);
	if (ia_valid & ATTR_MODE)
		err = ceph_setattr_chmod(dentry, attr);
	if (ia_valid & (ATTR_ATIME|ATTR_MTIME))
		err = ceph_setattr_time(dentry, attr);
	if (ia_valid & ATTR_SIZE)
		err = ceph_setattr_size(dentry, attr);
	return err;
}

/*
 * Verify that we have a lease on the given mask.  If not,
 * do a getattr against an mds.
 */
int ceph_do_getattr(struct dentry *dentry, int mask)
{
	int on_inode = 0;
	struct dentry *ret;

	if (ceph_snap(dentry->d_inode) == CEPH_SNAPDIR) {
		dout(30, "getattr dentry %p inode %p SNAPDIR\n", dentry,
		     dentry->d_inode);
		return 0;
	}

	dout(30, "getattr dentry %p inode %p mask %d\n", dentry,
	     dentry->d_inode, mask);
	if (ceph_inode_lease_valid(dentry->d_inode, mask))
		return 0;

	/*
	 * if the dentry is unhashed AND we have a cap, stat
	 * the ino directly.  (if its unhashed and we don't have a
	 * cap, we may be screwed anyway.)
	 */
	if (d_unhashed(dentry)) {
		if (ceph_get_cap_mds(dentry->d_inode) >= 0)
			on_inode = 1;
		else
			derr(0, "WARNING: getattr on unhashed cap-less"
			     " dentry %p %.*s\n", dentry,
			     dentry->d_name.len, dentry->d_name.name);
	}
	ret = ceph_do_lookup(dentry->d_inode->i_sb, dentry, mask,
			     on_inode, 0);
	if (IS_ERR(ret))
		return PTR_ERR(ret);
	if (ret)
		dentry = ret;
	if (!dentry->d_inode)
		return -ENOENT;
	return 0;
}

/*
 * Get all attributes.  Hopefully somedata we'll have a statlite()
 * and can limit the fields we require to be accurate.
 */
int ceph_getattr(struct vfsmount *mnt, struct dentry *dentry,
		 struct kstat *stat)
{
	int err;

	err = ceph_do_getattr(dentry, CEPH_STAT_MASK_INODE_ALL);
	dout(30, "getattr returned %d\n", err);
	if (!err) {
		generic_fillattr(dentry->d_inode, stat);
		stat->ino = ceph_ino(dentry->d_inode);
		if (ceph_snap(dentry->d_inode) != CEPH_NOSNAP)
			stat->dev = ceph_snap(dentry->d_inode);
		else
			stat->dev = 0;
	}
	return err;
}

/*
 * (virtual) xattrs
 *
 * These define virtual xattrs exposing the recursive directory statistics.
 */
struct _ceph_vir_xattr_cb {
	char *name;
	size_t (*getxattr_cb)(struct ceph_inode_info *ci, char *val,
			      size_t size);
};

static size_t _ceph_vir_xattrcb_entries(struct ceph_inode_info *ci, char *val,
					size_t size)
{
	return snprintf(val, size, "%lld", ci->i_files + ci->i_subdirs);
}

static size_t _ceph_vir_xattrcb_files(struct ceph_inode_info *ci, char *val,
				      size_t size)
{
	return snprintf(val, size, "%lld", ci->i_files);
}

static size_t _ceph_vir_xattrcb_subdirs(struct ceph_inode_info *ci, char *val,
					size_t size)
{
	return snprintf(val, size, "%lld", ci->i_subdirs);
}

static size_t _ceph_vir_xattrcb_rentries(struct ceph_inode_info *ci, char *val,
					 size_t size)
{
	return snprintf(val, size, "%lld", ci->i_rfiles + ci->i_rsubdirs);
}

static size_t _ceph_vir_xattrcb_rfiles(struct ceph_inode_info *ci, char *val,
				       size_t size)
{
	return snprintf(val, size, "%lld", ci->i_rfiles);
}

static size_t _ceph_vir_xattrcb_rsubdirs(struct ceph_inode_info *ci, char *val,
					 size_t size)
{
	return snprintf(val, size, "%lld", ci->i_subdirs);
}

static size_t _ceph_vir_xattrcb_rbytes(struct ceph_inode_info *ci, char *val,
				       size_t size)
{
	return snprintf(val, size, "%lld", ci->i_rbytes);
}

static size_t _ceph_vir_xattrcb_rctime(struct ceph_inode_info *ci, char *val,
				       size_t size)
{
	return snprintf(val, size, "%ld.%ld", (long)ci->i_rctime.tv_sec,
	                        (long)ci->i_rctime.tv_nsec);
}

static struct _ceph_vir_xattr_cb _ceph_vir_xattr_recs[] = {
	{ "user.ceph.dir.entries", _ceph_vir_xattrcb_entries},
	{ "user.ceph.dir.files", _ceph_vir_xattrcb_files},
	{ "user.ceph.dir.subdirs", _ceph_vir_xattrcb_subdirs},
	{ "user.ceph.dir.rentries", _ceph_vir_xattrcb_rentries},
	{ "user.ceph.dir.rfiles", _ceph_vir_xattrcb_rfiles},
	{ "user.ceph.dir.rsubdirs", _ceph_vir_xattrcb_rsubdirs},
	{ "user.ceph.dir.rbytes", _ceph_vir_xattrcb_rbytes},
	{ "user.ceph.dir.rctime", _ceph_vir_xattrcb_rctime},
	{ NULL, NULL }
};

static struct _ceph_vir_xattr_cb *_ceph_match_vir_xattr(const char *name)
{
	struct _ceph_vir_xattr_cb *xattr_rec = _ceph_vir_xattr_recs;

	do {
		if (strcmp(xattr_rec->name, name) == 0)
			return xattr_rec;
		xattr_rec++;
	} while (xattr_rec->name);

	return NULL;
}

ssize_t ceph_getxattr(struct dentry *dentry, const char *name, void *value,
		      size_t size)
{
	struct inode *inode = dentry->d_inode;
	struct ceph_inode_info *ci = ceph_inode(inode);
	int namelen = strlen(name);
	u32 numattr;
	int err;
	void *p, *end;
	struct _ceph_vir_xattr_cb *vir_xattr;

	/* let's see if a virtual xattr was requested */
	vir_xattr = _ceph_match_vir_xattr(name);
	if (vir_xattr)
		return (vir_xattr->getxattr_cb)(ci, value, size);

	/* get xattrs from mds (if we don't already have them) */
	err = ceph_do_getattr(dentry, CEPH_STAT_MASK_XATTR);
	if (err)
		return err;

	spin_lock(&inode->i_lock);

	err = -ENODATA;  /* == ENOATTR */
	if (!ci->i_xattr_len)
		goto out;

	/* find attr name */
	p = ci->i_xattr_data;
	end = p + ci->i_xattr_len;
	ceph_decode_32_safe(&p, end, numattr, bad);
	while (numattr--) {
		u32 len;
		int match;
		ceph_decode_32_safe(&p, end, len, bad);
		match = (len == namelen && strncmp(name, p, len) == 0);
		p += len;
		ceph_decode_32_safe(&p, end, len, bad);
		if (match) {
			err = -ERANGE;
			if (size && size < len)
				goto out;
			err = len;
			if (size == 0)
				goto out;
			memcpy(value, p, len);
			goto out;
		}
		p += len;
	}

out:
	spin_unlock(&inode->i_lock);
	return err;

bad:
	derr(10, "corrupt xattr info on %p %llx.%llx\n", dentry->d_inode,
	     ceph_vinop(dentry->d_inode));
	err = -EIO;
	goto out;
}

ssize_t ceph_listxattr(struct dentry *dentry, char *names, size_t size)
{
	struct inode *inode = dentry->d_inode;
	struct ceph_inode_info *ci = ceph_inode(inode);
	int namelen = 0;
	u32 numattr = 0;
	void *p, *end;
	int err;
	u32 len;
	int i;

	err = ceph_do_getattr(dentry, CEPH_STAT_MASK_XATTR);
	if (err)
		return err;

	spin_lock(&inode->i_lock);

	/* measure len of names */
	if (ci->i_xattr_len) {
		p = ci->i_xattr_data;
		end = p + ci->i_xattr_len;
		ceph_decode_32_safe(&p, end, numattr, bad);
		while (numattr--) {
			ceph_decode_32_safe(&p, end, len, bad);
			namelen += len + 1;
			p += len;
			ceph_decode_32_safe(&p, end, len, bad);
			p += len;
		}
	} else
		namelen = 0;

	/* include virtual dir xattrs */
	if ((inode->i_mode & S_IFMT) == S_IFDIR)
		for (i=0;  _ceph_vir_xattr_recs[i].name; i++)
			namelen += strlen(_ceph_vir_xattr_recs[i].name) + 1;

	err = -ERANGE;
	if (size && namelen > size)
		goto out;
	err = namelen;
	if (size == 0)
		goto out;

	/* copy names */
	if (ci->i_xattr_len) {
		p = ci->i_xattr_data;
		ceph_decode_32(&p, numattr);
		while (numattr--) {
			ceph_decode_32(&p, len);
			memcpy(names, p, len);
			names[len] = '\0';
			names += len + 1;
			p += len;
			ceph_decode_32(&p, len);
			p += len;
		}
	} else
		names[0] = 0;

	/* virtual xattr names, too */
	if ((inode->i_mode & S_IFMT) == S_IFDIR)
		for (i=0;  _ceph_vir_xattr_recs[i].name; i++) {
			len = sprintf(names, _ceph_vir_xattr_recs[i].name);
			names += len + 1;
		}

out:
	spin_unlock(&inode->i_lock);
	return err;

bad:
	derr(10, "corrupt xattr info on %p %llx.%llx\n", dentry->d_inode,
	     ceph_vinop(dentry->d_inode));
	err = -EIO;
	goto out;
}

int ceph_setxattr(struct dentry *dentry, const char *name,
		  const void *value, size_t size, int flags)
{
	struct ceph_client *client = ceph_client(dentry->d_sb);
	struct inode *inode = dentry->d_inode;
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct ceph_mds_request *req;
	struct ceph_mds_request_head *rhead;
	char *path;
	int pathlen;
	u64 pathbase;
	int err;
	int i, nr_pages;
	struct page **pages = NULL;
	void *kaddr;

	if (ceph_snap(inode) != CEPH_NOSNAP)
		return -EROFS;

	/* only support user.* xattrs, for now */
	if (strncmp(name, "user.", 5) != 0)
		return -EOPNOTSUPP;

	if (_ceph_match_vir_xattr(name) != NULL)
		return -EOPNOTSUPP;

	/* copy value into some pages */
	nr_pages = calc_pages_for(0, size);
	if (nr_pages) {
		pages = kmalloc(sizeof(pages)*nr_pages, GFP_NOFS);
		if (!pages)
			return -ENOMEM;
		err = -ENOMEM;
		for (i = 0; i < nr_pages; i++) {
			pages[i] = alloc_page(GFP_NOFS);
			if (!pages[i]) {
				nr_pages = i;
				goto out;
			}
			kaddr = kmap(pages[i]);
			memcpy(kaddr, value + i*PAGE_CACHE_SIZE,
			       min(PAGE_CACHE_SIZE, size-i*PAGE_CACHE_SIZE));
		}
	}

	/* do request */
	path = ceph_build_path(dentry, &pathlen, &pathbase, 0);
	if (IS_ERR(path))
		return PTR_ERR(path);
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_LSETXATTR,
				       pathbase, path, 0, name,
				       dentry, USE_AUTH_MDS);
	kfree(path);
	if (IS_ERR(req))
		return PTR_ERR(req);

	rhead = req->r_request->front.iov_base;
	rhead->args.setxattr.flags = cpu_to_le32(flags);

	req->r_request->pages = pages;
	req->r_request->nr_pages = nr_pages;
	req->r_request->hdr.data_len = cpu_to_le32(size);
	req->r_request->hdr.data_off = cpu_to_le32(0);

	ceph_mdsc_lease_release(mdsc, inode, NULL, CEPH_LOCK_IXATTR);
	err = ceph_mdsc_do_request(mdsc, req);
	ceph_mdsc_put_request(req);

out:
	if (pages) {
		for (i = 0; i < nr_pages; i++)
			__free_page(pages[i]);
		kfree(pages);
	}
	return err;
}

int ceph_removexattr(struct dentry *dentry, const char *name)
{
	struct ceph_client *client = ceph_client(dentry->d_sb);
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct inode *inode = dentry->d_inode;
	struct ceph_mds_request *req;
	char *path;
	int pathlen;
	u64 pathbase;
	int err;

	if (ceph_snap(inode) != CEPH_NOSNAP)
		return -EROFS;

	/* only support user.* xattrs, for now */
	if (strncmp(name, "user.", 5) != 0)
		return -EOPNOTSUPP;

	if (_ceph_match_vir_xattr(name) != NULL)
		return -EOPNOTSUPP;

	path = ceph_build_path(dentry, &pathlen, &pathbase, 0);
	if (IS_ERR(path))
		return PTR_ERR(path);
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_LRMXATTR,
				       pathbase, path, 0, name,
				       dentry, USE_AUTH_MDS);
	kfree(path);
	if (IS_ERR(req))
		return PTR_ERR(req);

	ceph_mdsc_lease_release(mdsc, inode, NULL, CEPH_LOCK_IXATTR);
	err = ceph_mdsc_do_request(mdsc, req);
	ceph_mdsc_put_request(req);
	return err;
}

