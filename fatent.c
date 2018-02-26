/*
 * Copyright (C) 2004, OGAWA Hirofumi
 * Released under GPL v2.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/msdos_fs.h>
#include <linux/blkdev.h>
#include "fat.h"

struct fatent_operations {
	void (*ent_blocknr)(struct super_block *, int, int *, sector_t *);
	void (*ent_set_ptr)(struct fat_entry *, int);
	int (*ent_bread)(struct super_block *, struct fat_entry *,
			 int, sector_t);
	int (*ent_get)(struct fat_entry *);
	void (*ent_put)(struct fat_entry *, int);
	int (*ent_next)(struct fat_entry *);
};

static DEFINE_SPINLOCK(fat12_entry_lock);

static void fat12_ent_blocknr(struct super_block *sb, int entry,
			      int *offset, sector_t *blocknr)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	int bytes = entry + (entry >> 1);
	WARN_ON(entry < FAT_START_ENT || sbi->max_cluster <= entry);
	*offset = bytes & (sb->s_blocksize - 1);
	*blocknr = sbi->fat_start + (bytes >> sb->s_blocksize_bits);
}

static void fat_ent_blocknr(struct super_block *sb, int entry,
			    int *offset, sector_t *blocknr)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	int bytes = (entry << sbi->fatent_shift);
	WARN_ON(entry < FAT_START_ENT || sbi->max_cluster <= entry);
	*offset = bytes & (sb->s_blocksize - 1);
	*blocknr = sbi->fat_start + (bytes >> sb->s_blocksize_bits);
}

static void fat12_ent_set_ptr(struct fat_entry *fatent, int offset)
{
	struct buffer_head **bhs = fatent->bhs;
	if (fatent->nr_bhs == 1) {
		WARN_ON(offset >= (bhs[0]->b_size - 1));
		fatent->u.ent12_p[0] = bhs[0]->b_data + offset;
		fatent->u.ent12_p[1] = bhs[0]->b_data + (offset + 1);
	} else {
		WARN_ON(offset != (bhs[0]->b_size - 1));
		fatent->u.ent12_p[0] = bhs[0]->b_data + offset;
		fatent->u.ent12_p[1] = bhs[1]->b_data;
	}
}

static void fat16_ent_set_ptr(struct fat_entry *fatent, int offset)
{
	WARN_ON(offset & (2 - 1));
	fatent->u.ent16_p = (__le16 *)(fatent->bhs[0]->b_data + offset);
}

static void fat32_ent_set_ptr(struct fat_entry *fatent, int offset)
{
	WARN_ON(offset & (4 - 1));
	fatent->u.ent32_p = (__le32 *)(fatent->bhs[0]->b_data + offset);
}

static int fat12_ent_bread(struct super_block *sb, struct fat_entry *fatent,
			   int offset, sector_t blocknr)
{
	struct buffer_head **bhs = fatent->bhs;

	WARN_ON(blocknr < MSDOS_SB(sb)->fat_start);
	fatent->fat_inode = MSDOS_SB(sb)->fat_inode;

	bhs[0] = sb_bread(sb, blocknr);
	if (!bhs[0])
		goto err;

	if ((offset + 1) < sb->s_blocksize)
		fatent->nr_bhs = 1;
	else {
		/* This entry is block boundary, it needs the next block */
		blocknr++;
		bhs[1] = sb_bread(sb, blocknr);
		if (!bhs[1])
			goto err_brelse;
		fatent->nr_bhs = 2;
	}
	fat12_ent_set_ptr(fatent, offset);
	return 0;

err_brelse:
	brelse(bhs[0]);
err:
	fat_msg(sb, KERN_ERR, "FAT read failed (blocknr %llu)", (llu)blocknr);
	return -EIO;
}

static int fat_ent_bread(struct super_block *sb, struct fat_entry *fatent,
			 int offset, sector_t blocknr)
{
	struct fatent_operations *ops = MSDOS_SB(sb)->fatent_ops;

	WARN_ON(blocknr < MSDOS_SB(sb)->fat_start);
	fatent->fat_inode = MSDOS_SB(sb)->fat_inode;
	fatent->bhs[0] = sb_bread(sb, blocknr);
	if (!fatent->bhs[0]) {
		fat_msg(sb, KERN_ERR, "FAT read failed (blocknr %llu)",
		       (llu)blocknr);
		return -EIO;
	}
	fatent->nr_bhs = 1;
	ops->ent_set_ptr(fatent, offset);
	return 0;
}

static int fat12_ent_get(struct fat_entry *fatent)
{
	u8 **ent12_p = fatent->u.ent12_p;
	int next;

	spin_lock(&fat12_entry_lock);
	if (fatent->entry & 1)
		next = (*ent12_p[0] >> 4) | (*ent12_p[1] << 4);
	else
		next = (*ent12_p[1] << 8) | *ent12_p[0];
	spin_unlock(&fat12_entry_lock);

	next &= 0x0fff;
	if (next >= BAD_FAT12)
		next = FAT_ENT_EOF;
	return next;
}

static int fat16_ent_get(struct fat_entry *fatent)
{
	int next = le16_to_cpu(*fatent->u.ent16_p);
	WARN_ON((unsigned long)fatent->u.ent16_p & (2 - 1));
	if (next >= BAD_FAT16)
		next = FAT_ENT_EOF;
	return next;
}

static int fat32_ent_get(struct fat_entry *fatent)
{
	int next = le32_to_cpu(*fatent->u.ent32_p) & 0x0fffffff;

	WARN_ON((unsigned long)fatent->u.ent32_p & (4 - 1));
	if (next >= BAD_FAT32)
	{
		next = FAT_ENT_EOF;
	}
	return next;
}

static void fat12_ent_put(struct fat_entry *fatent, int new)
{
	u8 **ent12_p = fatent->u.ent12_p;

	if (new == FAT_ENT_EOF)
		new = EOF_FAT12;

	spin_lock(&fat12_entry_lock);
	if (fatent->entry & 1) {
		*ent12_p[0] = (new << 4) | (*ent12_p[0] & 0x0f);
		*ent12_p[1] = new >> 4;
	} else {
		*ent12_p[0] = new & 0xff;
		*ent12_p[1] = (*ent12_p[1] & 0xf0) | (new >> 8);
	}
	spin_unlock(&fat12_entry_lock);

	mark_buffer_dirty_inode(fatent->bhs[0], fatent->fat_inode);
	if (fatent->nr_bhs == 2)
		mark_buffer_dirty_inode(fatent->bhs[1], fatent->fat_inode);
}

static void fat16_ent_put(struct fat_entry *fatent, int new)
{
	if (new == FAT_ENT_EOF)
		new = EOF_FAT16;

	*fatent->u.ent16_p = cpu_to_le16(new);
	mark_buffer_dirty_inode(fatent->bhs[0], fatent->fat_inode);
}

static void fat32_ent_put(struct fat_entry *fatent, int new)
{
    WARN_ON(new & 0xf0000000);
	new |= le32_to_cpu(*fatent->u.ent32_p) & ~0x0fffffff;
	*fatent->u.ent32_p = cpu_to_le32(new);
	mark_buffer_dirty_inode(fatent->bhs[0], fatent->fat_inode);
}

static int fat12_ent_next(struct fat_entry *fatent)
{
	u8 **ent12_p = fatent->u.ent12_p;
	struct buffer_head **bhs = fatent->bhs;
	u8 *nextp = ent12_p[1] + 1 + (fatent->entry & 1);

	fatent->entry++;
	if (fatent->nr_bhs == 1) {
		WARN_ON(ent12_p[0] > (u8 *)(bhs[0]->b_data +
							(bhs[0]->b_size - 2)));
		WARN_ON(ent12_p[1] > (u8 *)(bhs[0]->b_data +
							(bhs[0]->b_size - 1)));
		if (nextp < (u8 *)(bhs[0]->b_data + (bhs[0]->b_size - 1))) {
			ent12_p[0] = nextp - 1;
			ent12_p[1] = nextp;
			return 1;
		}
	} else {
		WARN_ON(ent12_p[0] != (u8 *)(bhs[0]->b_data +
							(bhs[0]->b_size - 1)));
		WARN_ON(ent12_p[1] != (u8 *)bhs[1]->b_data);
		ent12_p[0] = nextp - 1;
		ent12_p[1] = nextp;
		brelse(bhs[0]);
		bhs[0] = bhs[1];
		fatent->nr_bhs = 1;
		return 1;
	}
	ent12_p[0] = NULL;
	ent12_p[1] = NULL;
	return 0;
}

static int fat16_ent_next(struct fat_entry *fatent)
{
	const struct buffer_head *bh = fatent->bhs[0];
	fatent->entry++;
	if (fatent->u.ent16_p < (__le16 *)(bh->b_data + (bh->b_size - 2))) {
		fatent->u.ent16_p++;
		return 1;
	}
	fatent->u.ent16_p = NULL;
	return 0;
}

static int fat32_ent_next(struct fat_entry *fatent)
{
	const struct buffer_head *bh = fatent->bhs[0];
	fatent->entry++;
	if (fatent->u.ent32_p < (__le32 *)(bh->b_data + (bh->b_size - 4))) {
		fatent->u.ent32_p++;
		return 1;
	}
	fatent->u.ent32_p = NULL;
	return 0;
}

static struct fatent_operations fat12_ops = {
	.ent_blocknr	= fat12_ent_blocknr,
	.ent_set_ptr	= fat12_ent_set_ptr,
	.ent_bread	= fat12_ent_bread,
	.ent_get	= fat12_ent_get,
	.ent_put	= fat12_ent_put,
	.ent_next	= fat12_ent_next,
};

static struct fatent_operations fat16_ops = {
	.ent_blocknr	= fat_ent_blocknr,
	.ent_set_ptr	= fat16_ent_set_ptr,
	.ent_bread	= fat_ent_bread,
	.ent_get	= fat16_ent_get,
	.ent_put	= fat16_ent_put,
	.ent_next	= fat16_ent_next,
};

static struct fatent_operations fat32_ops = {
	.ent_blocknr	= fat_ent_blocknr,
	.ent_set_ptr	= fat32_ent_set_ptr,
	.ent_bread	= fat_ent_bread,
	.ent_get	= fat32_ent_get,
	.ent_put	= fat32_ent_put,
	.ent_next	= fat32_ent_next,
};

static inline void lock_fat(struct msdos_sb_info *sbi)
{
	mutex_lock(&sbi->fat_lock);
}

static inline void unlock_fat(struct msdos_sb_info *sbi)
{
	mutex_unlock(&sbi->fat_lock);
}

void fat_ent_access_init(struct super_block *sb)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);

	mutex_init(&sbi->fat_lock);

	switch (sbi->fat_bits) {
	case 32:
		sbi->fatent_shift = 2;
		sbi->fatent_ops = &fat32_ops;
		break;
	case 16:
		sbi->fatent_shift = 1;
		sbi->fatent_ops = &fat16_ops;
		break;
	case 12:
		sbi->fatent_shift = -1;
		sbi->fatent_ops = &fat12_ops;
		break;
	}
}

static void mark_fsinfo_dirty(struct super_block *sb)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);

	if (sb->s_flags & MS_RDONLY || sbi->fat_bits != 32)
		return;

	__mark_inode_dirty(sbi->fsinfo_inode, I_DIRTY_SYNC);
}

static inline int fat_ent_update_ptr(struct super_block *sb,
				     struct fat_entry *fatent,
				     int offset, sector_t blocknr)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct fatent_operations *ops = sbi->fatent_ops;
	struct buffer_head **bhs = fatent->bhs;

	/* Is this fatent's blocks including this entry? */
	if (!fatent->nr_bhs || bhs[0]->b_blocknr != blocknr)
		return 0;
	if (sbi->fat_bits == 12) {
		if ((offset + 1) < sb->s_blocksize) {
			/* This entry is on bhs[0]. */
			if (fatent->nr_bhs == 2) {
				brelse(bhs[1]);
				fatent->nr_bhs = 1;
			}
		} else {
			/* This entry needs the next block. */
			if (fatent->nr_bhs != 2)
				return 0;
			if (bhs[1]->b_blocknr != (blocknr + 1))
				return 0;
		}
	}
	ops->ent_set_ptr(fatent, offset);
	return 1;
}

int fat_ent_read(struct inode *inode, struct fat_entry *fatent, int entry)
{
	struct super_block *sb = inode->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB(inode->i_sb);
	struct fatent_operations *ops = sbi->fatent_ops;
	int err, offset;
	sector_t blocknr;

	if (entry < FAT_START_ENT || sbi->max_cluster <= entry) {
		fatent_brelse(fatent);
		fat_fs_error(sb, "invalid access to FAT (entry 0x%08x)", entry);
		return -EIO;
	}

	fatent_set_entry(fatent, entry);
	ops->ent_blocknr(sb, entry, &offset, &blocknr);

	if (!fat_ent_update_ptr(sb, fatent, offset, blocknr)) {
		fatent_brelse(fatent);
		err = ops->ent_bread(sb, fatent, offset, blocknr);
		if (err)
			return err;
	}
	return ops->ent_get(fatent);
}

/* FIXME: We can write the blocks as more big chunk. */
static int fat_mirror_bhs(struct super_block *sb, struct buffer_head **bhs,
			  int nr_bhs)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct buffer_head *c_bh;
	int err, n, copy;

	err = 0;
	for (copy = 1; copy < sbi->fats; copy++) {
		sector_t backup_fat = sbi->fat_length * copy;

		for (n = 0; n < nr_bhs; n++) {
			c_bh = sb_getblk(sb, backup_fat + bhs[n]->b_blocknr);
			if (!c_bh) {
				err = -ENOMEM;
				goto error;
			}
			memcpy(c_bh->b_data, bhs[n]->b_data, sb->s_blocksize);
			set_buffer_uptodate(c_bh);
			mark_buffer_dirty_inode(c_bh, sbi->fat_inode);
			if (sb->s_flags & MS_SYNCHRONOUS)
				err = sync_dirty_buffer(c_bh);
			brelse(c_bh);
			if (err)
				goto error;
		}
	}
error:
	return err;
}

int fat_ent_write(struct inode *inode, struct fat_entry *fatent,
		  int new, int wait)
{
	struct super_block *sb = inode->i_sb;
	struct fatent_operations *ops = MSDOS_SB(sb)->fatent_ops;
	int err;

	ops->ent_put(fatent, new);
	if (wait) {
		err = fat_sync_bhs(fatent->bhs, fatent->nr_bhs);
		if (err)
			return err;
	}
	return fat_mirror_bhs(sb, fatent->bhs, fatent->nr_bhs);
}

static inline int fat_ent_next(struct msdos_sb_info *sbi,
			       struct fat_entry *fatent)
{
	if (sbi->fatent_ops->ent_next(fatent)) {
		if (fatent->entry < sbi->max_cluster)
			return 1;
	}
	return 0;
}

static inline int fat_ent_read_block(struct super_block *sb,
				     struct fat_entry *fatent)
{
	struct fatent_operations *ops = MSDOS_SB(sb)->fatent_ops;
	sector_t blocknr;
	int offset;

	fatent_brelse(fatent);
	ops->ent_blocknr(sb, fatent->entry, &offset, &blocknr);
	return ops->ent_bread(sb, fatent, offset, blocknr);
}

static void fat_collect_bhs(struct buffer_head **bhs, int *nr_bhs,
			    struct fat_entry *fatent)
{
	int n, i;

	for (n = 0; n < fatent->nr_bhs; n++) {
		for (i = 0; i < *nr_bhs; i++) {
			if (fatent->bhs[n] == bhs[i])
				break;
		}
		if (i == *nr_bhs) {
			get_bh(fatent->bhs[n]);
			bhs[i] = fatent->bhs[n];
			(*nr_bhs)++;
		}
	}
}

int opel_fat_alloc_cluster( struct inode *inode, int *cluster, int mode )
{
	struct super_block *sb = inode->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct fatent_operations *ops = sbi->fatent_ops;
	struct fat_entry fatent, prev_ent;
	struct buffer_head *bhs[MAX_BUF_PER_PAGE];
	int i, count, err, nr_bhs, idx_clus;

	if(mode == -1){
		printk("[opel_fat] Not 'etc' type file need one cluster allocation ! \n");
		return -1;
	}

	lock_fat(sbi);

	err = nr_bhs = idx_clus = 0;
	count = FAT_START_ENT;
	fatent_init(&prev_ent);
	fatent_init(&fatent);

	fatent_set_entry(&fatent, 0);

	while (count < sbi->max_cluster) {
		if (fatent.entry >= sbi->max_cluster)
			fatent.entry = FAT_START_ENT;
		else if(fatent.entry <  FAT_START_ENT)
			fatent.entry = FAT_START_ENT;

		fatent_set_entry(&fatent, fatent.entry);
		err = fat_ent_read_block(sb, &fatent);

		if (err)
			goto out;

		/* Find the free entries in a block */

		do {
			if (ops->ent_get(&fatent) == FAT_ENT_FREE) {
				int entry = fatent.entry;

				/* make the cluster chain */
				ops->ent_put(&fatent, FAT_ENT_EOF);
				if (prev_ent.nr_bhs)
					ops->ent_put(&prev_ent, entry);

				fat_collect_bhs(bhs, &nr_bhs, &fatent);

				sbi->prev_free = entry;
				if (sbi->free_clusters != -1){
					sbi->free_clusters--;
				}

				cluster[idx_clus] = entry;
				idx_clus++;

                goto out;

			}

			count++;
			if (count == sbi->max_cluster)
				break;
		} while (fat_ent_next(sbi, &fatent));
	}

out:
	unlock_fat(sbi);
	mark_fsinfo_dirty( sb );
	fatent_brelse(&fatent);
	if (!err) {
		if (inode_needs_sync(inode))
			err = fat_sync_bhs(bhs, nr_bhs);
		if (!err)
			err = fat_mirror_bhs(sb, bhs, nr_bhs);
	}
	for (i = 0; i < nr_bhs; i++)
		brelse(bhs[i]);

	if (err && idx_clus)
		fat_free_clusters(inode, cluster[0]);

	return err;
}
//FAT 영엮 할당
int fat_alloc_clusters(struct inode *inode, int *cluster, int nr_cluster)
{
	struct super_block *sb = inode->i_sb;

	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct fatent_operations *ops = sbi->fatent_ops;
	struct fat_entry fatent, prev_ent;
	struct buffer_head *bhs[MAX_BUF_PER_PAGE];
	int i, count, err, nr_bhs, idx_clus;

	int area = 0; //proper work area number for inode
	unsigned long flags;

	BUG_ON(nr_cluster > (MAX_BUF_PER_PAGE / 2));	/* fixed limit */

	lock_fat(sbi);
	if (sbi->free_clusters != -1 && sbi->free_clus_valid &&
	    sbi->free_clusters < nr_cluster) {
		printk("[opel_fat] No space storage / current free %u / nr_cluster %d \n", sbi->free_clusters, nr_cluster );
		unlock_fat(sbi);
		return -ENOSPC;
	}

	if( sbi->fat_original_flag == OPEL_ORIGINAL_FAT_OFF )
	{
		opel_get_area_number( &area, inode );

		//Free space check for each area
		if( sbi->opel_free_cluster[ area ] < nr_cluster )
		{
			printk("[opel_fat] No space storage / bx current free %u / nr_cluster %d / area : %d  \n", sbi->opel_free_cluster[ area ], nr_cluster, area );
			unlock_fat(sbi);
			return -ENOSPC;
		}
	}

    err = nr_bhs = idx_clus = 0;
	count = FAT_START_ENT;
	fatent_init(&prev_ent);
	fatent_init(&fatent);

	if( sbi->fat_original_flag == OPEL_ORIGINAL_FAT_ON )
		fatent_set_entry(&fatent, sbi->prev_free + 1);
	else
		fatent_set_entry(&fatent, sbi->opel_prev_free_cluster[area] + 0 );

	while (count < sbi->max_cluster) {
		if (fatent.entry >= sbi->max_cluster)
			fatent.entry = FAT_START_ENT;
		fatent_set_entry(&fatent, fatent.entry);
		err = fat_ent_read_block(sb, &fatent);
		if (err)
			goto out;

		/* Find the free entries in a block */
		do {

			if( sbi->fat_original_flag == OPEL_ORIGINAL_FAT_ON ) //기존 디폴트로 동작해라
			{
				if (ops->ent_get(&fatent) == FAT_ENT_FREE )
				{
					int entry = fatent.entry;

					/* make the cluster chain */
					ops->ent_put(&fatent, FAT_ENT_EOF);
					if (prev_ent.nr_bhs)
						ops->ent_put(&prev_ent, entry);


					fat_collect_bhs(bhs, &nr_bhs, &fatent);

					sbi->prev_free = entry;

					//update for each area data
					if (sbi->free_clusters != -1)
					{
						sbi->free_clusters--;
					}

					cluster[idx_clus] = entry;
					idx_clus++;
					if (idx_clus == nr_cluster)
						goto out;

					/*
					 * fat_collect_bhs() gets ref-count of bhs,
					 * so we can still use the prev_ent.
					 */
					prev_ent = fatent;
				}
			}
			else //OPEL로 동작
			{
				if (ops->ent_get(&fatent) == FAT_ENT_FREE && sbi->opel_start_cluster[ area ] <= fatent.entry && sbi->opel_end_cluster[ area ] >= fatent.entry )
				{
					int entry = fatent.entry;

					/* make the cluster chain */
					ops->ent_put(&fatent, FAT_ENT_EOF);
					if (prev_ent.nr_bhs)
						ops->ent_put(&prev_ent, entry);

					fat_collect_bhs(bhs, &nr_bhs, &fatent);

					sbi->prev_free = entry;
					sbi->opel_prev_free_cluster[ area ] = entry;

					//update for each area data
					if (sbi->free_clusters != -1)
					{
						sbi->free_clusters--;
						sbi->opel_free_cluster[ area ]--;
					}

					cluster[idx_clus] = entry;
					idx_clus++;
					if (idx_clus == nr_cluster)
						goto out;

					/*
					 * fat_collect_bhs() gets ref-count of bhs,
					 * so we can still use the prev_ent.
					 */
					prev_ent = fatent;
				}
			}
			count++;
			if (count == sbi->max_cluster)
				break;
		} while (fat_ent_next(sbi, &fatent));
	}

	/* Couldn't allocate the free entries */
	sbi->free_clusters = 0;
	sbi->free_clus_valid = 1;
	err = -ENOSPC;

out:

	unlock_fat(sbi);
	mark_fsinfo_dirty(sb);
	fatent_brelse(&fatent);
	if (!err) {
		if (inode_needs_sync(inode))
			err = fat_sync_bhs(bhs, nr_bhs);
		if (!err)
			err = fat_mirror_bhs(sb, bhs, nr_bhs);
	}
	for (i = 0; i < nr_bhs; i++)
	{
		brelse(bhs[i]);
	}

	if (err && idx_clus)
	{
		fat_free_clusters(inode, cluster[0]);
	}

	return err;
}

void opel_show_the_status_unit_flag( struct super_block *sb, int area )
{
	struct msdos_sb_info *sbi = MSDOS_SB( sb );
	struct PA *pa = sbi->parea_PA[ area ];
	struct PA_unit_t *punit = pa->pa_unit;

	int unit_num = pa->pa_num;
	int i = 0, j = 0;

}
EXPORT_SYMBOL_GPL( opel_show_the_status_unit_flag );

static int get_area_number_for_free_func( struct super_block *sb, int entry )
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);

	if( sbi->opel_start_cluster[ OPEL_BLACKBOX_ETC ] <= entry && entry <= sbi->opel_end_cluster[ OPEL_BLACKBOX_ETC ] )
		return OPEL_BLACKBOX_ETC;
	else if( sbi->opel_start_cluster[ OPEL_BLACKBOX_NORMAL ] <= entry && entry <= sbi->opel_end_cluster[ OPEL_BLACKBOX_NORMAL ] )
		return OPEL_BLACKBOX_NORMAL;
	else if( sbi->opel_start_cluster[ OPEL_BLACKBOX_EVENT ] <= entry && entry <= sbi->opel_end_cluster[ OPEL_BLACKBOX_EVENT ] )
		return OPEL_BLACKBOX_EVENT;
	else if( sbi->opel_start_cluster[ OPEL_BLACKBOX_PARKING ] <= entry && entry <= sbi->opel_end_cluster[ OPEL_BLACKBOX_PARKING ] )
		return OPEL_BLACKBOX_PARKING;
	else if( sbi->opel_start_cluster[ OPEL_BLACKBOX_MANUAL ] <= entry && entry <= sbi->opel_end_cluster[ OPEL_BLACKBOX_MANUAL ] )
		return OPEL_BLACKBOX_MANUAL;
	else if( sbi->opel_start_cluster[ OPEL_BLACKBOX_CONFIG ] <= entry && entry <= sbi->opel_end_cluster[ OPEL_BLACKBOX_CONFIG ] )
		return OPEL_BLACKBOX_CONFIG;
	else
	{
		return OPEL_BLACKBOX_ETC;
	}
}

//파일을 삭제할 때, 할당된 클러스터를 해제한다.
int fat_free_clusters(struct inode *inode, int cluster)
{
	struct super_block *sb = inode->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct fatent_operations *ops = sbi->fatent_ops;
	struct fat_entry fatent;
	struct buffer_head *bhs[MAX_BUF_PER_PAGE];
	struct dentry *dentry = NULL;

	struct PA_unit_t *punit = NULL;
	struct PA *pa = NULL;
	int p_cnt=0;
	int i, err, nr_bhs;
	int first_cl = cluster, dirty_fsinfo = 0;
	int previous_cluster, temp_cluster= 0;

	int area=0;
	static int cnt = 0;

	nr_bhs = 0;
	fatent_init(&fatent);
	lock_fat(sbi);

	//FOR copy, >
	MSDOS_I( inode )->pre_alloced = 0;
	MSDOS_I( inode )->pre_count = 0;

	do {
		cluster = fat_ent_read(inode, &fatent, cluster);
		if (cluster < 0) {
			err = cluster;
			goto error;
		} else if (cluster == FAT_ENT_FREE) {
			fat_fs_error(sb, "%s: deleting FAT entry beyond EOF",
				     __func__);
			err = -EIO;
			goto error;
		}
		else
		{
			if( cluster != FAT_ENT_EOF ){
				if( cluster != temp_cluster + 1 )
					cnt++;

				temp_cluster = cluster;
			}
			else{ //EOF
				cnt = cnt - 1;
				temp_cluster = 0;
				cnt = 0;
			}
		}

		if (sbi->options.discard) {
			/*
			 * Issue discard for the sectors we no longer
			 * care about, batching contiguous clusters
			 * into one request
			 */
			if (cluster != fatent.entry + 1) {
				int nr_clus = fatent.entry - first_cl + 1;

				sb_issue_discard(sb,
					fat_clus_to_blknr(sbi, first_cl),
					nr_clus * sbi->sec_per_clus,
					GFP_NOFS, 0);

				first_cl = cluster;
			}
		}

		ops->ent_put(&fatent, FAT_ENT_FREE);
		if (sbi->free_clusters != -1) {
			sbi->free_clusters++; //해제 하라고 들어왔으니깐, free_clsuters가 하나 증가

			if( sbi->fat_original_flag == OPEL_ORIGINAL_FAT_OFF )  //PA, Partitioning
			{
				area = get_area_number_for_free_func( sb, fatent.entry );
				(sbi->opel_free_cluster[ area ])++;

				if( area != OPEL_BLACKBOX_ETC ) //pa는 미리할당 관련된 코드
				{
					/////////////PA_manage
					pa = sbi->parea_PA[ area ];
					punit = pa->pa_unit;

					//몇 번째 unit인지
					p_cnt = (cluster - punit[ 0 ].start) / pa->pa_cluster_num;
					punit[ p_cnt ].flag = OPEL_CLUSTER_FREE;
				}
			}
			dirty_fsinfo = 1;
		}

		if (nr_bhs + fatent.nr_bhs > MAX_BUF_PER_PAGE) {
			if (sb->s_flags & MS_SYNCHRONOUS) {

				err = fat_sync_bhs(bhs, nr_bhs);
				if (err)
				{
					goto error;
				}
			}
			err = fat_mirror_bhs(sb, bhs, nr_bhs);
			if (err)
			{
				goto error;
			}
			for (i = 0; i < nr_bhs; i++)
				brelse(bhs[i]);
			nr_bhs = 0;
		}
		fat_collect_bhs(bhs, &nr_bhs, &fatent);
	} while (cluster != FAT_ENT_EOF);

	//unit 상태 확인
	if (sb->s_flags & MS_SYNCHRONOUS) {
		err = fat_sync_bhs(bhs, nr_bhs);
		if (err)
		{
			goto error;
		}
	}
	err = fat_mirror_bhs(sb, bhs, nr_bhs);
error:

	fatent_brelse(&fatent);
	for (i = 0; i < nr_bhs; i++)
		brelse(bhs[i]);
	unlock_fat(sbi);
	if (dirty_fsinfo)
		mark_fsinfo_dirty(sb);

	return err;
}
EXPORT_SYMBOL_GPL(fat_free_clusters);

/* 128kb is the whole sectors for FAT12 and FAT16 */
#define FAT_READA_SIZE		(128 * 1024)

static void fat_ent_reada(struct super_block *sb, struct fat_entry *fatent,
			  unsigned long reada_blocks)
{
	struct fatent_operations *ops = MSDOS_SB(sb)->fatent_ops;
	sector_t blocknr;
	int i, offset;

	ops->ent_blocknr(sb, fatent->entry, &offset, &blocknr);

	for (i = 0; i < reada_blocks; i++)
		sb_breadahead(sb, blocknr + i);
}

//각 파티션의 번호를 전달
void opel_get_area_number( int *area, struct inode *inode )
{
	struct dentry *dentry = NULL;
	struct dentry *upper_dentry = NULL;
	int temp_area = -1;

    dentry = list_entry( inode->i_dentry.first, struct dentry, d_u.d_alias );
	if( S_ISDIR( inode->i_mode) || dentry->d_parent == NULL)
	{
		temp_area = OPEL_BLACKBOX_ETC;
	}
	else
	{
		upper_dentry = dentry->d_parent;
		while( upper_dentry != NULL )
		{
			if( upper_dentry->d_name.name == NULL )
				break;

			//etc는 따로 디렉터리 설정으로 하지 않고 말그대로 기타로 하겟음 ( ex ) 현재 보드상엣 /media/boot 부분에 boot.ini랑 디바이스 파일이랑 , zImage올라가있는데
			//이 쪽 파티션이 vfat로 되어 있어서 이런 역할로 etc를 나눠 주겟음
			else if(strcmp(upper_dentry->d_name.name, OPEL_NORMAL_DIRECTORY ) == 0 )
				temp_area = OPEL_BLACKBOX_NORMAL;

			else if(strcmp(upper_dentry->d_name.name, OPEL_EVENT_DIRECTORY ) == 0 )
				temp_area = OPEL_BLACKBOX_EVENT;

			else if(strcmp(upper_dentry->d_name.name, OPEL_PARKING_DIRECTORY ) == 0 )
				temp_area = OPEL_BLACKBOX_PARKING;

			else if(strcmp(upper_dentry->d_name.name, OPEL_MANUAL_DIRECTORY ) == 0 )
				temp_area = OPEL_BLACKBOX_MANUAL;

			else if(strcmp(upper_dentry->d_name.name, OPEL_CONFIG_DIRECTORY ) == 0 )
				temp_area = OPEL_BLACKBOX_CONFIG;
			else //test
				temp_area = OPEL_BLACKBOX_ETC;


			if( temp_area != -1 )
				break;
			else if( !strcmp("/", upper_dentry->d_name.name ) )
				break;
			else
				upper_dentry =upper_dentry->d_parent;
		}
	}

	if( 1 <= temp_area && temp_area <= 4 ) //normal, evnet, parking, manual인데 avi가 아니먄 etc에서 할당
	{
		if( strstr( dentry->d_name.name, "mp4" ) == NULL )
		{
			*area = OPEL_BLACKBOX_ETC;
			return;
		}
	}
	if( temp_area == -1 )
		*area = OPEL_BLACKBOX_ETC;
	else
		*area =temp_area;
}
EXPORT_SYMBOL_GPL( opel_get_area_number );

int opel_fat_count_free_clusters_for_area(struct super_block *sb)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct fatent_operations *ops = sbi->fatent_ops;
	struct fat_entry fatent;
	unsigned long reada_blocks, reada_mask, cur_block;
	int free;

	reada_blocks = FAT_READA_SIZE >> sb->s_blocksize_bits;
	reada_mask = reada_blocks - 1;
	cur_block = 0;

	free = 0;
	fatent_init(&fatent);
	fatent_set_entry(&fatent, FAT_START_ENT);

	if( sbi->opel_free_valid != -1 )
		goto area_out;

	while (fatent.entry < sbi->max_cluster) {
		/* readahead of fat blocks */
		if ((cur_block & reada_mask) == 0) {
			unsigned long rest = sbi->fat_length - cur_block;
			fat_ent_reada(sb, &fatent, min(reada_blocks, rest));
		}
		cur_block++;

		fat_ent_read_block(sb, &fatent);

		do {
			if (ops->ent_get(&fatent) == FAT_ENT_FREE)
			{
				free++;

				if( fatent.entry < sbi->opel_start_cluster[ OPEL_BLACKBOX_ETC ] );

				else if( fatent.entry <= sbi->opel_end_cluster[ OPEL_BLACKBOX_ETC ] )
					(sbi->opel_free_cluster[ OPEL_BLACKBOX_ETC ])++;

				else if( fatent.entry <= sbi->opel_end_cluster[ OPEL_BLACKBOX_NORMAL ] )
				{
					(sbi->opel_free_cluster[ OPEL_BLACKBOX_NORMAL ])++;
				}

				else if( fatent.entry <= sbi->opel_end_cluster[ OPEL_BLACKBOX_EVENT ] )
					(sbi->opel_free_cluster[ OPEL_BLACKBOX_EVENT ])++;

				else if( fatent.entry <= sbi->opel_end_cluster[ OPEL_BLACKBOX_PARKING ] )
					(sbi->opel_free_cluster[ OPEL_BLACKBOX_PARKING ])++;

				else if( fatent.entry <= sbi->opel_end_cluster[ OPEL_BLACKBOX_MANUAL ] )
					(sbi->opel_free_cluster[ OPEL_BLACKBOX_MANUAL ])++;

				else if( fatent.entry <= sbi->opel_end_cluster[ OPEL_BLACKBOX_CONFIG ] )
					(sbi->opel_free_cluster[ OPEL_BLACKBOX_CONFIG ])++;

			}
		} while (fat_ent_next(sbi, &fatent));
	}

	sbi->opel_free_valid = 1;
	sbi->free_clusters = free;
	sbi->free_clus_valid = 1;
	mark_fsinfo_dirty(sb);
	fatent_brelse(&fatent);

area_out:
	return 0;
}
EXPORT_SYMBOL_GPL( opel_fat_count_free_clusters_for_area );

static int __opel_decide_each_pa_status( unsigned int start, unsigned int mid, unsigned int end, unsigned int pa_cnt, struct PA_unit_t *punit, struct super_block *sb )
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct fatent_operations *ops = sbi->fatent_ops;
	struct fat_entry fatent;
	unsigned long reada_blocks, reada_mask, cur_block;
	int err = 0, free;
	int cluster_cur = 0;
	int cnt = 0;

	if( !( (end & mid) ^ FAT_ENT_EOF) || !( (end & start) ^ FAT_ENT_EOF) ) //중간 혹은 처음과 끝이 EOF면 연결
	{
		punit[ pa_cnt ].flag = OPEL_CLUSTER_MIDEOF;
	}
	else if( end == FAT_ENT_EOF && mid != FAT_ENT_EOF )
	{
		if( start == FAT_ENT_FREE )
		{
			punit[ pa_cnt ].flag = OPEL_CLUSTER_GARBAGE;
		}
		else if( start != FAT_ENT_FREE )
		{
			//used 정상파일(1,2)
			punit[ pa_cnt ].flag = OPEL_CLUSTER_USED;
		}
	}

	reada_blocks = FAT_READA_SIZE >> sb->s_blocksize_bits;
	reada_mask = reada_blocks - 1;
	cur_block = 0;

	free = 0;
	fatent_init(&fatent);
	fatent_set_entry(&fatent, punit[ pa_cnt ].start );
	while (fatent.entry <= punit[ pa_cnt ].end ) {

		/* readahead of fat blocks */
		if ((cur_block & reada_mask) == 0) {
			unsigned long rest = sbi->fat_length - cur_block;
			fat_ent_reada(sb, &fatent, min(reada_blocks, rest));
		}
		cur_block++;

		err = fat_ent_read_block(sb, &fatent);
		if (err)
			goto out;

		do {

			if( punit[ pa_cnt ].flag == OPEL_CLUSTER_MIDEOF )
			{
				if( ops->ent_get( &fatent ) == FAT_ENT_EOF && fatent.entry != punit[pa_cnt].end )
				{
					ops->ent_put( &fatent, fatent.entry + 1 );
				}
			}
			else if( punit[ pa_cnt ].flag == OPEL_CLUSTER_GARBAGE )
			{
				if( punit[ pa_cnt ].start <= fatent.entry && fatent.entry <= punit[ pa_cnt ].end )
				{
					if( ops->ent_get( &fatent ) != FAT_ENT_FREE )
					{
						ops->ent_put( &fatent, FAT_ENT_FREE );
						printk("[opel_fat] %u ", ops->ent_get( &fatent ) );
					}
				}
			}
			else; //used

			cnt++;
		} while (fat_ent_next(sbi, &fatent));
	}

	mark_fsinfo_dirty(sb);
	fatent_brelse(&fatent);
out:
	return err;
}

int opel_decide_each_pa_status( struct super_block *sb, struct PA_unit_t *punit, int area  )
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct fatent_operations *ops = sbi->fatent_ops;
	struct fat_entry fatent;
	unsigned long reada_blocks, reada_mask, cur_block;
	int err = 0, free;
	int cluster_cur = 0;


	int total_pa_cnt=0, i=0;
	int before_cl = FAT_START_ENT;
	unsigned int cluster_num = FAT_START_ENT;
	unsigned int pa_cnt = 0, num_pre_alloc = ( sbi->opel_pre_size[ area ] * 1024 ) / ( sbi->cluster_size / 1024 );
	unsigned int start = 0, mid = 0, end = 0;

	lock_fat(sbi);

	reada_blocks = FAT_READA_SIZE >> sb->s_blocksize_bits;
	reada_mask = reada_blocks - 1;
	cur_block = 0;

	free = 0;
	fatent_init(&fatent);
	fatent_set_entry(&fatent, FAT_START_ENT);

	while (fatent.entry < sbi->max_cluster) {

		/* readahead of fat blocks */
		if ((cur_block & reada_mask) == 0) {
			unsigned long rest = sbi->fat_length - cur_block;
			fat_ent_reada(sb, &fatent, min(reada_blocks, rest));
		}
		cur_block++;

		err = fat_ent_read_block(sb, &fatent);
		if (err)
			goto out;

		do {
			if( punit[ pa_cnt ].start <= fatent.entry && fatent.entry <= punit[ pa_cnt ].end )
			{
				if( fatent.entry == punit[ pa_cnt ].start )
				{
					start = ops->ent_get( &fatent );
				}

				if( ops->ent_get( &fatent ) == FAT_ENT_EOF )
				{
					printk("[opel_fat] eof entry : %d \n", fatent.entry );

					if( fatent.entry == punit[ pa_cnt ].end ) //끝 EOF
						end = ops->ent_get( &fatent );

					else if( punit[ pa_cnt ].start < fatent.entry && fatent.entry < punit[ pa_cnt ].end ) //중간 EOF
					{
						mid = ops->ent_get( &fatent );
					}
				}
			}

			if( fatent.entry == punit[ pa_cnt ].end )
			{
				__opel_decide_each_pa_status( start, mid, end, pa_cnt, punit, sb );

				if( punit[ pa_cnt ].flag == OPEL_CLUSTER_GARBAGE )  punit[ pa_cnt ].flag = OPEL_CLUSTER_FREE;
				else if( punit[ pa_cnt ].flag == OPEL_CLUSTER_MIDEOF ) punit[ pa_cnt ].flag = OPEL_CLUSTER_USED;
				else;

				pa_cnt++;
				start = mid = end = FAT_ENT_FREE;
			}
		} while (fat_ent_next(sbi, &fatent));
	}
	mark_fsinfo_dirty(sb);
	fatent_brelse(&fatent);
out:
	unlock_fat(sbi);
	return err;
}
EXPORT_SYMBOL_GPL( opel_decide_each_pa_status );

int fat_count_free_clusters(struct super_block *sb)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct fatent_operations *ops = sbi->fatent_ops;
	struct fat_entry fatent;
	unsigned long reada_blocks, reada_mask, cur_block;
	int err = 0, free;

	lock_fat(sbi);
	if (sbi->free_clusters != -1 && sbi->free_clus_valid)
		goto out;

	reada_blocks = FAT_READA_SIZE >> sb->s_blocksize_bits;
	reada_mask = reada_blocks - 1;
	cur_block = 0;

	free = 0;
	fatent_init(&fatent);
	fatent_set_entry(&fatent, FAT_START_ENT);
	while (fatent.entry < sbi->max_cluster) {
		/* readahead of fat blocks */
		if ((cur_block & reada_mask) == 0) {
			unsigned long rest = sbi->fat_length - cur_block;
			fat_ent_reada(sb, &fatent, min(reada_blocks, rest));
		}
		cur_block++;

		err = fat_ent_read_block(sb, &fatent);
		if (err)
			goto out;

		do {
			if (ops->ent_get(&fatent) == FAT_ENT_FREE)
			{
				free++;
			}
		} while (fat_ent_next(sbi, &fatent));
	}
	sbi->free_clusters = free;
	sbi->free_clus_valid = 1;
	mark_fsinfo_dirty(sb);
	fatent_brelse(&fatent);
out:
	unlock_fat(sbi);
	return err;
}
