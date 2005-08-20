/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * io.c
 *
 * Buffer cache handling
 *
 * Copyright (C) 2002, 2004 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <linux/fs.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/highmem.h>

#include <cluster/masklog.h>

#include "ocfs2.h"

#include "alloc.h"
#include "inode.h"
#include "journal.h"
#include "uptodate.h"

#include "buffer_head_io.h"

void ocfs2_end_buffer_io_sync(struct buffer_head *bh,
			      int uptodate)
{
	if (!uptodate)
		mlog_errno(-EIO);

	if (uptodate)
		set_buffer_uptodate(bh);
	else
		clear_buffer_uptodate(bh);
	unlock_buffer(bh);
}

int ocfs2_write_blocks(ocfs2_super *osb, struct buffer_head *bhs[],
		       int nr, struct inode *inode)
{
	int status = 0;
	int i;
	struct super_block *sb;
	struct buffer_head *bh;

	mlog_entry("(bh[0]->b_blocknr = %llu, nr=%d, inode=%p)\n",
		   (unsigned long long)bhs[0]->b_blocknr, nr, inode);

	if (osb == NULL || osb->sb == NULL || bhs == NULL) {
		status = -EINVAL;
		mlog_errno(status);
		goto bail;
	}

	sb = osb->sb;

	if (inode)
		down(&OCFS2_I(inode)->ip_io_sem);
	for (i = 0 ; i < nr ; i++) {
		bh = bhs[i];
		if (bh == NULL) {
			if (inode)
				up(&OCFS2_I(inode)->ip_io_sem);
			status = -EIO;
			mlog_errno(status);
			goto bail;
		}

		if (unlikely(bh->b_blocknr < OCFS2_SUPER_BLOCK_BLKNO)) {
			BUG();
			status = -EIO;
			mlog_errno(status);
			goto bail;
		}

		if (unlikely(buffer_jbd(bh))) {
			/* What are you thinking?! */
			mlog(ML_ERROR, "trying to write a jbd managed bh "
				       "(blocknr = %llu), nr=%d\n",
			     (unsigned long long)bh->b_blocknr, nr);
			BUG();
		}

		lock_buffer(bh);

		set_buffer_uptodate(bh);

		/* remove from dirty list before I/O. */
		clear_buffer_dirty(bh);

		bh->b_end_io = ocfs2_end_buffer_io_sync;
		submit_bh(WRITE, bh);
	}

	for (i = (nr - 1) ; i >= 0; i--) {
		bh = bhs[i];

		wait_on_buffer(bh);

		if (!buffer_uptodate(bh)) {
			/* Status won't be cleared from here on out,
			 * so we can safely record this and loop back
			 * to cleanup the other buffers. Don't need to
			 * remove the clustered uptodate information
			 * for this bh as it's not marked locally
			 * uptodate. */
			status = -EIO;
			brelse(bh);
			bhs[i] = NULL;
			continue;
		}

		if (inode)
			ocfs2_set_buffer_uptodate(inode, bh);
	}
	if (inode)
		up(&OCFS2_I(inode)->ip_io_sem);

bail:

	mlog_exit(status);
	return status;
}

int ocfs2_read_blocks(ocfs2_super *osb, u64 block, int nr,
		      struct buffer_head *bhs[], int flags,
		      struct inode *inode)
{
	int status = 0;
	struct super_block *sb;
	int i, ignore_cache = 0;
	struct buffer_head *bh;

	mlog_entry("(block=(%"MLFu64"), nr=(%d), flags=%d, inode=%p)\n",
		   block, nr, flags, inode);

	if (osb == NULL || osb->sb == NULL || bhs == NULL) {
		status = -EINVAL;
		mlog_errno(status);
		goto bail;
	}

	if (nr < 0) {
		mlog(ML_ERROR, "asked to read %d blocks!\n", nr);
		status = -EINVAL;
		mlog_errno(status);
		goto bail;
	}

	if (nr == 0) {
		mlog(ML_BH_IO, "No buffers will be read!\n");
		status = 0;
		goto bail;
	}

	sb = osb->sb;

	if (flags & OCFS2_BH_CACHED && !inode)
		flags &= ~OCFS2_BH_CACHED;

	if (inode)
		down(&OCFS2_I(inode)->ip_io_sem);
	for (i = 0 ; i < nr ; i++) {
		if (bhs[i] == NULL) {
			bhs[i] = sb_getblk(sb, block++);
			if (bhs[i] == NULL) {
				if (inode)
					up(&OCFS2_I(inode)->ip_io_sem);
				status = -EIO;
				mlog_errno(status);
				goto bail;
			}
		}
		bh = bhs[i];
		ignore_cache = 0;

		if (flags & OCFS2_BH_CACHED &&
		    !ocfs2_buffer_uptodate(inode, bh)) {
			mlog(ML_UPTODATE,
			     "bh (%llu), inode %"MLFu64" not uptodate\n",
			     (unsigned long long)bh->b_blocknr,
			     OCFS2_I(inode)->ip_blkno);
			ignore_cache = 1;
		}

		/* XXX: Can we ever get this and *not* have the cached
		 * flag set? */
		if (buffer_jbd(bh)) {
			if (!(flags & OCFS2_BH_CACHED) || ignore_cache)
				mlog(ML_BH_IO, "trying to sync read a jbd "
					       "managed bh (blocknr = %llu)\n",
				     (unsigned long long)bh->b_blocknr);
			continue;
		}

		if (!(flags & OCFS2_BH_CACHED) || ignore_cache) {
			if (buffer_dirty(bh)) {
				/* This should probably be a BUG, or
				 * at least return an error. */
				mlog(ML_BH_IO, "asking me to sync read a dirty "
					       "buffer! (blocknr = %llu)\n",
				     (unsigned long long)bh->b_blocknr);
				continue;
			}

			lock_buffer(bh);
			if (buffer_jbd(bh)) {
#ifdef CATCH_BH_JBD_RACES
				mlog(ML_ERROR, "block %llu had the JBD bit set "
					       "while I was in lock_buffer!",
				     (unsigned long long)bh->b_blocknr);
				BUG();
#else
				unlock_buffer(bh);
				continue;
#endif
			}
			clear_buffer_uptodate(bh);
			bh->b_end_io = ocfs2_end_buffer_io_sync;
			if (flags & OCFS2_BH_READAHEAD)
				submit_bh(READA, bh);
			else
				submit_bh(READ, bh);
			continue;
		}
	}

	status = 0;

	for (i = (nr - 1); i >= 0; i--) {
		bh = bhs[i];

		/* We know this can't have changed as we hold the
		 * inode sem. Avoid doing any work on the bh if the
		 * journal has it. */
		if (!buffer_jbd(bh))
			wait_on_buffer(bh);

		if (!buffer_uptodate(bh)) {
			/* Status won't be cleared from here on out,
			 * so we can safely record this and loop back
			 * to cleanup the other buffers. Don't need to
			 * remove the clustered uptodate information
			 * for this bh as it's not marked locally
			 * uptodate. */
			status = -EIO;
			brelse(bh);
			bhs[i] = NULL;
			continue;
		}

		if (inode)
			ocfs2_set_buffer_uptodate(inode, bh);
	}
	if (inode)
		up(&OCFS2_I(inode)->ip_io_sem);

	mlog(ML_BH_IO, "block=(%"MLFu64"), nr=(%d), cached=%s\n", block, nr,
	     (!(flags & OCFS2_BH_CACHED) || ignore_cache) ? "no" : "yes");

bail:

	mlog_exit(status);
	return status;
}
