/*
 *  linux/fs/fat/inode.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *  VFAT extensions by Gordon Chaffee, merged with msdos fs by Henrik Storner
 *  Rewritten for the constant inumbers support by Al Viro
 *
 *  Fixes:
 *
 *	Max Cohan: Fixed invalid FSINFO offset when info_sector is 0
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/pagemap.h>
#include <linux/mpage.h>
#include <linux/buffer_head.h>
#include <linux/mount.h>
#include <linux/aio.h>
#include <linux/vfs.h>
#include <linux/parser.h>
#include <linux/uio.h>
#include <linux/writeback.h>
#include <linux/log2.h>
#include <linux/hash.h>
#include <linux/blkdev.h>
#include <asm/unaligned.h>
#include "fat.h"

#ifndef CONFIG_FAT_DEFAULT_IOCHARSET
/* if user don't select VFAT, this is undefined. */
#define CONFIG_FAT_DEFAULT_IOCHARSET	""
#endif

//FOR TEST, COUNT_AREA means cluster counter
#define COUNT_AREA_0 25600
#define COUNT_AREA_1 25600
#define COUNT_AREA_2 25600
#define COUNT_AREA_3 25600
#define COUNT_AREA_4 25600
#define COUNT_AREA_5 25600

static int fat_default_codepage = CONFIG_FAT_DEFAULT_CODEPAGE;
static char fat_default_iocharset[] = CONFIG_FAT_DEFAULT_IOCHARSET;
static int fat_block = -1;

unsigned long opel_time_ordering( void )
{
	static unsigned long order = 0;
	static u64 previous_sec = 0;

	if( previous_sec == get_seconds() ) //same secondes
	{
		order++;
	}
	else //if diffrent sec, dont need to ditinguish
	{
		order = 0;
	}

	previous_sec = get_seconds();

	return order;
}
EXPORT_SYMBOL_GPL( opel_time_ordering );

void opel_de_reupdate(struct super_block *sb, struct inode *inode){
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	unsigned int i_pos;
	struct buffer_head *bh[32];
	struct msdos_dir_entry *raw_entry;
	unsigned int inpage_start, inpage_end, inpage_cur;

    i_pos = MSDOS_I(inode)->i_pos;

	inpage_cur = i_pos >> sbi->dir_per_block_bits;
	inpage_start = inpage_cur - (inpage_cur % OPEL_BLOCK_IN_PAGE);
	inpage_end = inpage_start + OPEL_BLOCK_IN_PAGE -1 ;

	printk("[opel_fat] DE info , I_pos : %u \n", i_pos);
	printk("[opel_fat] Start Cur End : %u / %u / %u \n", inpage_start, inpage_cur, inpage_end);

	bh[0] = sb_bread(sb, inpage_cur);
	raw_entry = &((struct msdos_dir_entry *) (bh[0]->b_data))
		[i_pos & (sbi->dir_per_block - 1)];
	raw_entry->size = cpu_to_le32(inode->i_size);

	mark_buffer_dirty(bh[0]);
	sync_dirty_buffer(bh[0]);
	brelse(bh[0]);

    printk("[opel_fat] Update DE Complete, real file size %u\n", (unsigned int)inode->i_size);
}

//This function update cluster chain one more time (file release time)
void opel_clusterchain_reupdate(struct super_block *sb, struct inode *inode){
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct dentry *dentry_f;
	int area_num;
	unsigned int allocated_size;
	unsigned int used_size = (unsigned int)(inode->i_size);
	unsigned int unused_size;

	int block_num;
	unsigned int new_eof_cluster;

	struct buffer_head *bh[32]; //Block in page
	int i;
	unsigned int *data = (unsigned int*) kmalloc(512, GFP_KERNEL);

	unsigned int start_block_pos;
	unsigned int update_block_pos;
	unsigned int inpage_start_block_pos;
	unsigned int inpage_end_block_pos;
	unsigned int eof_block_pos;
	unsigned int eof_inpage_start_block_pos;
	unsigned int eof_inpage_end_block_pos ;

	unsigned int need_update_cluster_number;
	unsigned int ori_eof_cluster_number;

	unsigned int page_aligned_start;

	dentry_f =  list_entry(inode->i_dentry.first, struct dentry, d_u.d_alias);

    //First, Get some information
	opel_get_area_number(&area_num , inode);
	allocated_size = sbi->opel_pre_size[area_num] * 1024 * 1024;
	unused_size = allocated_size - used_size;

	page_aligned_start = sbi->fat_start - (sbi->fat_start % OPEL_BLOCK_IN_PAGE);;

	printk("[opel_fat] pre : %u, used : %u \n", allocated_size, used_size);

	need_update_cluster_number =  opel_floor(used_size, sbi->cluster_size) + (MSDOS_I(inode)->i_logstart);
	ori_eof_cluster_number = opel_floor(allocated_size, sbi->cluster_size) + (MSDOS_I(inode)->i_logstart) -1;

	printk("[opel_fat] need updated_cluster / ori eof number : %u / %u \n", need_update_cluster_number, ori_eof_cluster_number);

	if(need_update_cluster_number > sbi->opel_end_cluster[area_num])
		need_update_cluster_number = need_update_cluster_number - (sbi->opel_end_cluster[area_num] - sbi->opel_start_cluster[area_num]) -1;
	if(ori_eof_cluster_number  > sbi->opel_end_cluster[area_num])
		ori_eof_cluster_number = ori_eof_cluster_number - (sbi->opel_end_cluster[area_num] - sbi->opel_start_cluster[area_num]) -1 ;


	printk("[opel_fat] need updated_cluster / ori eof number : %u / %u \n", need_update_cluster_number, ori_eof_cluster_number);

	start_block_pos =   (unsigned int)sbi->fat_start  + opel_floor(MSDOS_I(inode)->i_logstart, OPEL_CLUSTER_IN_BLOCK);
	update_block_pos =  (unsigned int)sbi->fat_start  + opel_floor(need_update_cluster_number, OPEL_CLUSTER_IN_BLOCK);
	eof_block_pos =     (unsigned int)sbi->fat_start  + opel_floor(ori_eof_cluster_number, OPEL_CLUSTER_IN_BLOCK);

	inpage_start_block_pos = (unsigned int)update_block_pos - (update_block_pos % OPEL_BLOCK_IN_PAGE);
	inpage_end_block_pos = (unsigned int)inpage_start_block_pos + OPEL_BLOCK_IN_PAGE - 1;

	eof_inpage_start_block_pos = (unsigned int)eof_block_pos - (eof_block_pos % OPEL_BLOCK_IN_PAGE);
	eof_inpage_end_block_pos = (unsigned int)eof_inpage_start_block_pos + OPEL_BLOCK_IN_PAGE - 1;


	printk("[opel_fat] block start : %d , aligned start : %u \n", sbi->fat_start, page_aligned_start);
	printk("[opel_fat] start / update / eof block number : %u / %u / %u \n", start_block_pos, update_block_pos, eof_block_pos);
	printk("[opel_fat] inpage_start_block_pos / inpage_end_block_pos : %u / %u \n", inpage_start_block_pos,  inpage_end_block_pos);
	printk("[opel_fat] eof_inpage_start_block_pos / eof_inpage_end_block_pos : %u / %u \n", eof_inpage_start_block_pos,  eof_inpage_end_block_pos);
	printk("[opel_fat] pre size :%u , Real size : %u \n", allocated_size, (unsigned int)inode->i_size);

    // 1 Page update case
	if(inpage_start_block_pos == eof_inpage_start_block_pos){

		for(i=0; i<OPEL_BLOCK_IN_PAGE; i++){
			bh[i] = sb_bread(sb, inpage_start_block_pos + i);
		}

		block_num = eof_block_pos - eof_inpage_start_block_pos;
		memcpy(data,bh[block_num]->b_data, 512 );
		for(i=0; i< OPEL_CLUSTER_IN_BLOCK; i ++){
			if(data[i] == 0x0FFFFFFF){

				if(i==0)
					data[i] = data[i+1] - 1;
				else
					data[i] = data[i-1] + 1;

				break;
			}
		}

		if(i == OPEL_CLUSTER_IN_BLOCK)
			printk("[opel_fat] Error, can't find proper old eof \n");
		memcpy(bh[block_num]->b_data ,data, 512 );

		//Change new EoF
		block_num = update_block_pos - eof_inpage_start_block_pos;
		memcpy(data,bh[block_num]->b_data , 512 );
		new_eof_cluster = (MSDOS_I(inode)->i_logstart) + opel_ceil(used_size, sbi->cluster_size); //-1;

		if(new_eof_cluster > sbi->opel_end_cluster[area_num])
			new_eof_cluster = new_eof_cluster - (sbi->opel_end_cluster[area_num] - sbi->opel_start_cluster[area_num]) -1 ;

		printk("[opel_fat] new eof cluster num : %u \n", new_eof_cluster -1 );

		for(i=0; i<OPEL_CLUSTER_IN_BLOCK; i ++){
			if(data[i] == new_eof_cluster){
				data[i] = 0x0FFFFFFF;
				break;
			}
		}
		if(i == OPEL_CLUSTER_IN_BLOCK)
			printk("[opel_fat] Error, can't find proper new eof \n");

		memcpy(bh[block_num]->b_data ,data, 512 );

		for(i=0; i<OPEL_BLOCK_IN_PAGE; i++){
			mark_buffer_dirty(bh[i]);
		}
		fat_sync_bhs(bh, OPEL_BLOCK_IN_PAGE);

		for(i=0; i<OPEL_BLOCK_IN_PAGE; i++)
			brelse(bh[i]);

	}
	// 2 Page update case
	else{
		printk("[opel_fat] FAT update occur in two page \n");

		for(i=0; i<OPEL_BLOCK_IN_PAGE; i++){
			bh[i] = sb_bread(sb, eof_inpage_start_block_pos + i);
		}

		//Chage origianl EoF
		block_num = eof_block_pos - eof_inpage_start_block_pos;
		memcpy(data,bh[block_num]->b_data, 512 );
		for(i=0; i< OPEL_CLUSTER_IN_BLOCK; i ++){
			if(data[i] == 0x0FFFFFFF){

				if(i==0)
					data[i] = data[i+1] - 1;
				else
					data[i] = data[i-1] + 1;

				break;
			}
		}
		if(i == OPEL_CLUSTER_IN_BLOCK)
			printk("[opel_fat] Error, can't find proper old eof \n");
		memcpy(bh[block_num]->b_data ,data, 512 );

		ll_rw_block(WRITE, OPEL_BLOCK_IN_PAGE, bh);
		for(i=0; i<OPEL_BLOCK_IN_PAGE; i++)
			brelse(bh[i]);
		printk("[opel_fat] Origianl EoF page Write  \n");

		for(i=0; i<OPEL_BLOCK_IN_PAGE; i++){
			bh[i] = sb_bread(sb, inpage_start_block_pos + i);
		}
		//Change new EoF
		block_num = update_block_pos - inpage_start_block_pos;
		memcpy(data,bh[block_num]->b_data , 512 );
		new_eof_cluster = (MSDOS_I(inode)->i_logstart) + opel_ceil(used_size, sbi->cluster_size) ;
		for(i=0; i<OPEL_CLUSTER_IN_BLOCK; i ++){
			if(data[i] == new_eof_cluster){
				data[i] = 0x0FFFFFFF;
				break;
			}
		}
		if(i == OPEL_CLUSTER_IN_BLOCK)
			printk("[opel_fat] Error, can't find proper new eof \n");

		memcpy(bh[block_num]->b_data ,data, 512 );
		for(i=0; i<OPEL_BLOCK_IN_PAGE; i++){
			mark_buffer_dirty(bh[i]);
		}
		fat_sync_bhs(bh, OPEL_BLOCK_IN_PAGE);
		for(i=0; i<OPEL_BLOCK_IN_PAGE; i++)
			brelse(bh[i]);
	}

	kfree(data);
	sbi->opel_next_start_cluster[area_num] = new_eof_cluster;//+1;

}

/* FIXME: We can write the blocks as more big chunk. */
static int opel_fat_mirror_bhs(struct super_block *sb, struct buffer_head **bhs,
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

static int opel_pre_allocate( struct inode *inode, unsigned int start, unsigned int end, int pa_cluster_num, int area )
{
	struct super_block *sb = inode->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB( sb );
	struct buffer_head *bh[300];

	int i=0, j=0;
	int cluster = start + 1;
	int num_of_page = pa_cluster_num / OPEL_CLUSTER_IN_PAGE;
	int page_offset = pa_cluster_num % OPEL_CLUSTER_IN_PAGE;
	int p_cnt = 0, num_buf = 0, cnt=0;

	unsigned int *data;
	unsigned int fat_block_pos = fat_block + start / OPEL_CLUSTER_IN_BLOCK;

	//data채우기
	data = ( unsigned int * )kmalloc( (end-start+1) * sizeof(unsigned int)  , GFP_KERNEL ); //하나에 4byte

	for( cnt = cluster ; cnt <= (end+1) ; cnt++ )
	{
		data[i++] = cnt;
	}
	data[ end-start] = FAT_ENT_EOF;

	//First : Inode Update
	MSDOS_I(inode)->i_start = start;
	MSDOS_I(inode)->i_logstart = start;
	inode->i_blocks = pa_cluster_num << (sbi->cluster_bits - 9);

	//SB update// //할당한 만큼 free_cluster 빼주기
	sbi->opel_free_cluster[area] -= pa_cluster_num;
	sbi->free_clusters -= pa_cluster_num;


	sbi->opel_next_start_cluster[area] = end + 1;
	sbi->opel_prev_free_cluster[area] = end;

	for( i=0 ; i < (end-start+1)/128 ; i++ )
	{
			bh[num_buf] = sb_bread(sb, fat_block_pos); //block 읽어와서 bh가 가리키게 하고
			memcpy( bh[num_buf]->b_data , data + ( 128 * i ), 512 ); //block chain쓰기
			mark_buffer_dirty( bh[ num_buf ] );

			num_buf++;
			fat_block_pos++;
	}

	fat_sync_bhs(bh, num_buf);
	opel_fat_mirror_bhs(sb,bh,num_buf);

	for(i=0;i<num_buf;i++)
		brelse( bh[i] );

		kfree(data);

	return 0;
}

/* returns the length of a struct qstr, ignoring trailing dots */
static unsigned int __opel_vfat_striptail_len(unsigned int len, const char *name)
{
	    while (len && name[len - 1] == '.')
			        len--;
		    return len;
}

static unsigned int opel_vfat_striptail_len(const struct qstr *qstr)
{
	    return __opel_vfat_striptail_len(qstr->len, qstr->name);
}

static int opel_vfat_find( struct inode *dir, struct qstr *qname, struct fat_slot_info *sinfo )
{
	unsigned int len = opel_vfat_striptail_len( qname );
	if(len==0) return ENOENT;

	return fat_search_long( dir, qname->name, len, sinfo );
}

static int opel_vfat_unlink( struct inode *dir, struct dentry *dentry )
{
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = dir->i_sb;
	struct fat_slot_info sinfo;
	int err;

	mutex_lock(&MSDOS_SB(sb)->s_lock);

	err = opel_vfat_find(dir, &dentry->d_name, &sinfo);
	if (err)
		goto out;

	err = fat_remove_entries(dir, &sinfo);  /* and releases bh */
	if (err)
		goto out;
	clear_nlink(inode);
	inode->i_mtime = inode->i_atime = CURRENT_TIME_SEC;

	fat_detach(inode);
out:
	mutex_unlock(&MSDOS_SB(sb)->s_lock);

	return err;
}

int opel_fat_handle_cluster( struct inode *inode, int mode )
{
	struct super_block *sb = inode->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct dentry *dentry = NULL;
	struct PA_unit_t *punit = NULL;
	struct PA *pa = NULL;

	unsigned int num_pre_alloc;

	int err = 0, cluster = 0, area = 0, rs;
	int unit_num = 0;
	int cnt = 0;
	struct msdos_inode_info *ms_i;

	opel_get_area_number( &area, inode ); //각 파티션의 번호를
	num_pre_alloc = ( sbi->opel_pre_size[ area ] * 1024 ) / ( sbi->cluster_size / 1024 ); //할당 받을 클러스터 개수

	if( area == OPEL_BLACKBOX_ETC || sbi->fat_original_flag == OPEL_ORIGINAL_FAT_ON ) //ETC 영엮이랑 오리지널 프래그가 on이면 NORMAL_ALLOC할당으로 보낸다.
		goto NORMAL_ALLOC;
	dentry = list_entry( inode->i_dentry.first, struct dentry, d_u.d_alias );
	if( dentry == NULL  || strstr( dentry->d_name.name, "mp4" ) == NULL ) //동영상 파일이 아니면
		goto NORMAL_ALLOC;

	if( MSDOS_I( inode )->pre_count < num_pre_alloc)
	{
		MSDOS_I(inode)->pre_count++;
	}
	else
	{
		printk("[opel_fat] PA size exceeded \n");
		return -EIO;
	}

	if( MSDOS_I(inode)->pre_alloced == OPEL_PRE_ALLOC_ON ) //opel_pre_allocate함수 한번 타고 나오면 안들어간다.
		return 0;

	mutex_lock(&sbi->fat_lock);

	pa = sbi->parea_PA[ area ];
	punit = pa->pa_unit;
	unit_num = pa->pa_num;

	MSDOS_I(inode)->pre_alloced = OPEL_PRE_ALLOC_ON; //기존에는 inode->i_ino로 구별했었는데 변경함

	struct dentry *upper_dentry = NULL;
	upper_dentry = dentry->d_parent;

	do{
		if( cnt >= unit_num ){
			printk("[opel_fat] There is no PA Unit \n");
			MSDOS_I(inode)->pre_alloced = OPEL_ORIGINAL_FAT_OFF;
			opel_show_the_status_unit_flag( sb,area ); //유닛 상태 출력

			opel_vfat_unlink( upper_dentry->d_inode, dentry );
			err = -ENOSPC;
			break;
		}
		if( punit[ pa->cur_pa_cnt ].flag == OPEL_CLUSTER_FREE ){
			punit[ pa->cur_pa_cnt ].flag = OPEL_CLUSTER_USED; //수정
			rs = opel_pre_allocate( inode, punit[ pa->cur_pa_cnt ].start, punit[ pa->cur_pa_cnt ].end, pa->pa_cluster_num, area );
			if( !rs ){
				break;
			}
		}
		else;

		pa->cur_pa_cnt++;
		cnt++;
		if( pa->cur_pa_cnt >= unit_num ) pa->cur_pa_cnt = 0;

	}while( pa->cur_pa_cnt < unit_num );

	mutex_unlock(&sbi->fat_lock);

	return err;

NORMAL_ALLOC:
	//origin 기존 할당 방식

	err = fat_alloc_clusters( inode, &cluster, 1 );
	if (err)
		return err;
	/* FIXME: this cluster should be added after data of this
	 * cluster is writed */
	err = fat_chain_add(inode, cluster, 1);
	if (err)
	{
		printk("[opel_fat] fat_chain_add error !!!, fat_free_clusters\n");
		fat_free_clusters(inode, cluster);
	}
	return err;
}

static int fat_add_cluster(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB( sb );
	struct dentry *dentry = NULL;

	unsigned int val;
	unsigned int time;

	//origin
	int err, cluster;

	val = opel_fat_handle_cluster( inode, OPEL_FAT_ALLOC_CLUSTER );

    return val;

	/* FIXME: this cluster should be added after data of this
	 * cluster is writed */
}

void opel_check_page_align( unsigned int *cluster, unsigned int max_cluster, unsigned short fat_start )
{
	//align for 8kb page size - 2048 clusters in 1 page

	int adjust = fat_start % OPEL_BLOCK_IN_PAGE; //eight block in one page
	int offset = (*cluster + adjust * OPEL_CLUSTER_IN_BLOCK) % OPEL_CLUSTER_IN_PAGE; //1024 cluster in one page

	*cluster = *cluster + (OPEL_CLUSTER_IN_PAGE - offset);

	if(*cluster >= max_cluster)
		    *cluster = *cluster - OPEL_CLUSTER_IN_PAGE;
}

int opel_fat_just_init_super(struct super_block *sb)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);

	sbi->opel_free_valid = -1;

	sbi->opel_area_ratio[ OPEL_BLACKBOX_NORMAL		 ]    = 10;
	sbi->opel_area_ratio[ OPEL_BLACKBOX_EVENT  ]    = 10;
	sbi->opel_area_ratio[ OPEL_BLACKBOX_PARKING 		 ]    = 10;
	sbi->opel_area_ratio[ OPEL_BLACKBOX_MANUAL 		 ]    = 10;
	sbi->opel_area_ratio[ OPEL_BLACKBOX_CONFIG         ]    = 10;
	sbi->opel_area_ratio[ OPEL_BLACKBOX_ETC 			 ] 	  = 10;

	sbi->opel_pre_size[ OPEL_BLACKBOX_NORMAL ]    	  = 20;
	sbi->opel_pre_size[ OPEL_BLACKBOX_EVENT ]   = 20;
	sbi->opel_pre_size[ OPEL_BLACKBOX_PARKING ]		  = 20;
	sbi->opel_pre_size[ OPEL_BLACKBOX_MANUAL ]  		  = 20;

	sbi->opel_start_cluster[ OPEL_BLACKBOX_ETC          ] = FAT_START_ENT + 2;
	sbi->opel_start_cluster[ OPEL_BLACKBOX_NORMAL       ] = FAT_START_ENT + 2 + COUNT_AREA_0;
	sbi->opel_start_cluster[ OPEL_BLACKBOX_EVENT ] = FAT_START_ENT + 2 + COUNT_AREA_0 + ( COUNT_AREA_1 );  //10M + 400k : 400k는 여유 공간
	sbi->opel_start_cluster[ OPEL_BLACKBOX_PARKING      ] = FAT_START_ENT + 2 + COUNT_AREA_0 + ( COUNT_AREA_1 ) + COUNT_AREA_2;
	sbi->opel_start_cluster[ OPEL_BLACKBOX_MANUAL       ] = FAT_START_ENT + 2 + COUNT_AREA_0 + ( COUNT_AREA_1 ) + COUNT_AREA_2 + COUNT_AREA_3;
	sbi->opel_start_cluster[ OPEL_BLACKBOX_CONFIG        ] = FAT_START_ENT + 2 + COUNT_AREA_0 + ( COUNT_AREA_1 ) + COUNT_AREA_2 + COUNT_AREA_3 + COUNT_AREA_4;

	sbi->opel_end_cluster[  OPEL_BLACKBOX_ETC         ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_NORMAL       ] - 1;
	sbi->opel_end_cluster[  OPEL_BLACKBOX_NORMAL      ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_EVENT ] - 1;
	sbi->opel_end_cluster[  OPEL_BLACKBOX_EVENT] = sbi->opel_start_cluster[ OPEL_BLACKBOX_PARKING      ] - 1;
	sbi->opel_end_cluster[  OPEL_BLACKBOX_PARKING     ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_MANUAL       ] - 1;
	sbi->opel_end_cluster[  OPEL_BLACKBOX_MANUAL      ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_CONFIG        ] - 1;
	sbi->opel_end_cluster[  OPEL_BLACKBOX_CONFIG       ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_CONFIG ] + COUNT_AREA_5 - 1;

	sbi->opel_prev_free_cluster[ OPEL_BLACKBOX_ETC ]			 = sbi->opel_start_cluster[ OPEL_BLACKBOX_ETC ];
	sbi->opel_prev_free_cluster[ OPEL_BLACKBOX_NORMAL ]		 = sbi->opel_start_cluster[ OPEL_BLACKBOX_NORMAL ];
	sbi->opel_prev_free_cluster[ OPEL_BLACKBOX_EVENT ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_EVENT ];
	sbi->opel_prev_free_cluster[ OPEL_BLACKBOX_PARKING ]		 = sbi->opel_start_cluster[ OPEL_BLACKBOX_PARKING ];
	sbi->opel_prev_free_cluster[ OPEL_BLACKBOX_MANUAL ]		 = sbi->opel_start_cluster[ OPEL_BLACKBOX_MANUAL ];
	sbi->opel_prev_free_cluster[ OPEL_BLACKBOX_CONFIG ] 		 = sbi->opel_start_cluster[ OPEL_BLACKBOX_CONFIG ];

	sbi->opel_next_start_cluster[ OPEL_BLACKBOX_ETC ]			= -1;
	sbi->opel_next_start_cluster[ OPEL_BLACKBOX_NORMAL ]			= -1;
	sbi->opel_next_start_cluster[ OPEL_BLACKBOX_EVENT ]	= -1;
	sbi->opel_next_start_cluster[ OPEL_BLACKBOX_PARKING ]		= -1;
	sbi->opel_next_start_cluster[ OPEL_BLACKBOX_MANUAL ]			= -1;
	sbi->opel_next_start_cluster[ OPEL_BLACKBOX_CONFIG ] 			= -1;

	sbi->opel_free_cluster[ OPEL_BLACKBOX_ETC ]			 = 0;
	sbi->opel_free_cluster[ OPEL_BLACKBOX_NORMAL ]		 = 0;
	sbi->opel_free_cluster[ OPEL_BLACKBOX_EVENT ] = 0;
	sbi->opel_free_cluster[ OPEL_BLACKBOX_PARKING ]		 = 0;
	sbi->opel_free_cluster[ OPEL_BLACKBOX_MANUAL ]		 = 0;
	sbi->opel_free_cluster[ OPEL_BLACKBOX_CONFIG ] 		 = 0;

	sbi->fat_original_flag = OPEL_ORIGINAL_FAT_OFF;
	opel_fat_count_free_clusters_for_area( sb );

	return 0;
}
EXPORT_SYMBOL_GPL( opel_fat_just_init_super );


int opel_fat_update_super(struct super_block *sb){
	struct msdos_sb_info *sbi = MSDOS_SB(sb);

	// Init partitioning
	//Sequence : ETC / NORMAL / N_SHOCK / P_MOTION / P_SHOCK / MANUAL / BACKUP

	//Align for 8kb page (16 block)
	// 128 cluster in one block

	sbi->opel_free_valid = -1;
	sbi->opel_start_cluster[ OPEL_BLACKBOX_ETC ] = FAT_START_ENT + 2;
	opel_check_page_align( &sbi->opel_start_cluster[ OPEL_CONFIG_ETC ], sbi->max_cluster, sbi->fat_start );
	sbi->opel_start_cluster[ OPEL_BLACKBOX_NORMAL ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_ETC ] + ( sbi->max_cluster * sbi->opel_area_ratio[ OPEL_BLACKBOX_ETC ] ) / 100;
	opel_check_page_align( &sbi->opel_start_cluster[ OPEL_BLACKBOX_NORMAL ], sbi->max_cluster, sbi->fat_start );

	sbi->opel_start_cluster[ OPEL_BLACKBOX_EVENT ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_NORMAL ] + ( sbi->max_cluster * sbi->opel_area_ratio[ OPEL_BLACKBOX_NORMAL ] ) / 100;
	opel_check_page_align( &sbi->opel_start_cluster[ OPEL_BLACKBOX_EVENT ], sbi->max_cluster, sbi->fat_start );

	sbi->opel_start_cluster[ OPEL_BLACKBOX_PARKING ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_EVENT ] + ( sbi->max_cluster * sbi->opel_area_ratio[ OPEL_BLACKBOX_EVENT ] ) / 100;
	opel_check_page_align( &sbi->opel_start_cluster[ OPEL_BLACKBOX_PARKING ], sbi->max_cluster, sbi->fat_start );

	sbi->opel_start_cluster[ OPEL_BLACKBOX_MANUAL ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_PARKING ] + ( sbi->max_cluster * sbi->opel_area_ratio[ OPEL_BLACKBOX_PARKING ] ) / 100;
	opel_check_page_align( &sbi->opel_start_cluster[ OPEL_BLACKBOX_MANUAL ], sbi->max_cluster, sbi->fat_start );

	sbi->opel_start_cluster[ OPEL_BLACKBOX_CONFIG ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_MANUAL ] + ( sbi->max_cluster * sbi->opel_area_ratio[ OPEL_BLACKBOX_MANUAL ] ) / 100;
	opel_check_page_align( &sbi->opel_start_cluster[ OPEL_BLACKBOX_CONFIG ], sbi->max_cluster, sbi->fat_start );

	sbi->opel_end_cluster[ OPEL_BLACKBOX_ETC          ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_NORMAL ] - 1;
	sbi->opel_end_cluster[ OPEL_BLACKBOX_NORMAL       ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_EVENT ] - 1;
	sbi->opel_end_cluster[ OPEL_BLACKBOX_EVENT ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_PARKING ] - 1;
	sbi->opel_end_cluster[ OPEL_BLACKBOX_PARKING      ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_MANUAL ] - 1;
	sbi->opel_end_cluster[ OPEL_BLACKBOX_MANUAL       ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_CONFIG ] - 1;
	sbi->opel_end_cluster[ OPEL_BLACKBOX_CONFIG        ] = sbi->max_cluster - 1;

	sbi->opel_prev_free_cluster[ OPEL_BLACKBOX_ETC ]			 = sbi->opel_start_cluster[ OPEL_BLACKBOX_ETC ];
	sbi->opel_prev_free_cluster[ OPEL_BLACKBOX_NORMAL ]		 = sbi->opel_start_cluster[ OPEL_BLACKBOX_NORMAL ];
	sbi->opel_prev_free_cluster[ OPEL_BLACKBOX_EVENT ] = sbi->opel_start_cluster[ OPEL_BLACKBOX_EVENT ];
	sbi->opel_prev_free_cluster[ OPEL_BLACKBOX_PARKING ]		 = sbi->opel_start_cluster[ OPEL_BLACKBOX_PARKING ];
	sbi->opel_prev_free_cluster[ OPEL_BLACKBOX_MANUAL ]		 = sbi->opel_start_cluster[ OPEL_BLACKBOX_MANUAL ];
	sbi->opel_prev_free_cluster[ OPEL_BLACKBOX_CONFIG ] 		 = sbi->opel_start_cluster[ OPEL_BLACKBOX_CONFIG ];

	sbi->opel_next_start_cluster[ OPEL_BLACKBOX_ETC ]			= -1;
	sbi->opel_next_start_cluster[ OPEL_BLACKBOX_NORMAL ]			= -1;
	sbi->opel_next_start_cluster[ OPEL_BLACKBOX_EVENT ]	= -1;
	sbi->opel_next_start_cluster[ OPEL_BLACKBOX_PARKING ]		= -1;
	sbi->opel_next_start_cluster[ OPEL_BLACKBOX_MANUAL ]			= -1;
	sbi->opel_next_start_cluster[ OPEL_BLACKBOX_CONFIG ] 			= -1;

	sbi->opel_free_cluster[ OPEL_BLACKBOX_ETC ]			 = 0;
	sbi->opel_free_cluster[ OPEL_BLACKBOX_NORMAL ]		 = 0;
	sbi->opel_free_cluster[ OPEL_BLACKBOX_EVENT ] = 0;
	sbi->opel_free_cluster[ OPEL_BLACKBOX_PARKING ]		 = 0;
	sbi->opel_free_cluster[ OPEL_BLACKBOX_MANUAL ]		 = 0;
	sbi->opel_free_cluster[ OPEL_BLACKBOX_CONFIG ] 		 = 0;

	//초기화
	sbi->fat_original_flag = OPEL_ORIGINAL_FAT_OFF;
	opel_fat_count_free_clusters_for_area( sb );

	printk("[opel_fat] Complete cluster calculation \n");
	printk("[opel_fat]    1. [%2d%%] ETC		[%6d ~%6d] / Free %6d(%3d%%) / MB : %d  \n", sbi->opel_area_ratio[OPEL_BLACKBOX_ETC],  sbi->opel_start_cluster[OPEL_BLACKBOX_ETC], sbi->opel_end_cluster[OPEL_BLACKBOX_ETC],sbi->opel_free_cluster[OPEL_BLACKBOX_ETC], \
			(sbi->opel_free_cluster[OPEL_BLACKBOX_ETC] * 100) / (sbi->opel_end_cluster[OPEL_BLACKBOX_ETC] - sbi->opel_start_cluster[OPEL_BLACKBOX_ETC] + 1 ), sbi->opel_free_cluster[ OPEL_BLACKBOX_ETC ] * (sbi->cluster_size/1024) / 1024 );

	printk("[opel_fat]    2. [%2d%%] Normal	[%6d ~%6d] / Free %6d(%3d%%) / MB : %d\n", sbi->opel_area_ratio[OPEL_BLACKBOX_NORMAL],   sbi->opel_start_cluster[OPEL_BLACKBOX_NORMAL], sbi->opel_end_cluster[OPEL_BLACKBOX_NORMAL],sbi->opel_free_cluster[OPEL_BLACKBOX_NORMAL], \
			(sbi->opel_free_cluster[OPEL_BLACKBOX_NORMAL] * 100) / (sbi->opel_end_cluster[OPEL_BLACKBOX_NORMAL]-sbi->opel_start_cluster[OPEL_BLACKBOX_NORMAL] + 1 ) , sbi->opel_free_cluster[ OPEL_BLACKBOX_NORMAL ] * (sbi->cluster_size/1024) / 1024 );

	printk("[opel_fat]    3. [%2d%%] Event	[%6d ~%6d] / Free %6d(%3d%%) / MB : %d \n", sbi->opel_area_ratio[OPEL_BLACKBOX_EVENT],   sbi->opel_start_cluster[OPEL_BLACKBOX_EVENT], sbi->opel_end_cluster[OPEL_BLACKBOX_EVENT],sbi->opel_free_cluster[OPEL_BLACKBOX_EVENT], \
			(sbi->opel_free_cluster[OPEL_BLACKBOX_EVENT] * 100) / (sbi->opel_end_cluster[OPEL_BLACKBOX_EVENT]-sbi->opel_start_cluster[OPEL_BLACKBOX_EVENT] + 1 ) , sbi->opel_free_cluster[ OPEL_BLACKBOX_EVENT ] * (sbi->cluster_size/1024) / 1024 );

	printk("[opel_fat]    4. [%2d%%] Parking	[%6d ~%6d] / Free %6d(%3d%%) / MB : %d \n", sbi->opel_area_ratio[OPEL_BLACKBOX_PARKING],   sbi->opel_start_cluster[OPEL_BLACKBOX_PARKING], sbi->opel_end_cluster[OPEL_BLACKBOX_PARKING],sbi->opel_free_cluster[OPEL_BLACKBOX_PARKING], \
			(sbi->opel_free_cluster[OPEL_BLACKBOX_PARKING] * 100) / (sbi->opel_end_cluster[OPEL_BLACKBOX_PARKING]-sbi->opel_start_cluster[ OPEL_BLACKBOX_PARKING ] + 1)  , sbi->opel_free_cluster[ OPEL_BLACKBOX_PARKING ] * (sbi->cluster_size/1024) / 1024 );

	printk("[opel_fat]    5. [%2d%%] Manual	[%6d ~%6d] / Free %6d(%3d%%) / MB : %d \n", sbi->opel_area_ratio[OPEL_BLACKBOX_MANUAL],   sbi->opel_start_cluster[OPEL_BLACKBOX_MANUAL], sbi->opel_end_cluster[OPEL_BLACKBOX_MANUAL],sbi->opel_free_cluster[OPEL_BLACKBOX_MANUAL], \
			(sbi->opel_free_cluster[OPEL_BLACKBOX_MANUAL] * 100) / (sbi->opel_end_cluster[OPEL_BLACKBOX_MANUAL]-sbi->opel_start_cluster[OPEL_BLACKBOX_MANUAL] + 1)  , sbi->opel_free_cluster[ OPEL_BLACKBOX_MANUAL ] * (sbi->cluster_size/1024) / 1024 );

	printk("[opel_fat]    6. [%2d%%] Config	[%6d ~%6d] / Free %6d(%3d%%) / MB : %d \n", sbi->opel_area_ratio[OPEL_BLACKBOX_CONFIG],   sbi->opel_start_cluster[OPEL_BLACKBOX_CONFIG], sbi->opel_end_cluster[OPEL_BLACKBOX_CONFIG],sbi->opel_free_cluster[OPEL_BLACKBOX_CONFIG], \
			(sbi->opel_free_cluster[OPEL_BLACKBOX_CONFIG] * 100) / (sbi->opel_end_cluster[OPEL_BLACKBOX_CONFIG]-sbi->opel_start_cluster[OPEL_BLACKBOX_CONFIG] + 1)  , sbi->opel_free_cluster[ OPEL_BLACKBOX_CONFIG ] * (sbi->cluster_size/1024) / 1024 );

	printk("[opel_fat] Total SD_Size : %u[KB] \n", (sbi->max_cluster-2)  * (sbi->cluster_size / 1024) );

	if( sbi->opel_free_cluster[ OPEL_BLACKBOX_ETC ] == 0 || sbi->opel_free_cluster[ OPEL_BLACKBOX_NORMAL ] == 0 || sbi->opel_free_cluster[ OPEL_BLACKBOX_EVENT ] == 0 ||
			sbi->opel_free_cluster[ OPEL_BLACKBOX_PARKING ] == 0 || sbi->opel_free_cluster[ OPEL_BLACKBOX_MANUAL ] == 0 || sbi->opel_free_cluster[ OPEL_BLACKBOX_CONFIG ] == 0 )
	{
		printk("[opel_fat] Any part is full, Our policy turn into a original FAT \n");
		sbi->fat_original_flag = OPEL_ORIGINAL_FAT_ON;
	}

	return 0;
}
EXPORT_SYMBOL_GPL( opel_fat_update_super );

static inline int __fat_get_block(struct inode *inode, sector_t iblock,
				  unsigned long *max_blocks,
				  struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	unsigned long mapped_blocks;
	sector_t phys;
	int err, offset;

	err = fat_bmap(inode, iblock, &phys, &mapped_blocks, create);
	if (err)
		return err; if (phys) { map_bh(bh_result, sb, phys); *max_blocks = min(mapped_blocks, *max_blocks);
		return 0;
	}
	if (!create)
		return 0;

	if (iblock != MSDOS_I(inode)->mmu_private >> sb->s_blocksize_bits) {
		fat_fs_error(sb, "corrupted file size (i_pos %lld, %lld)",
			MSDOS_I(inode)->i_pos, MSDOS_I(inode)->mmu_private);
		return -EIO;
	}

	offset = (unsigned long)iblock & (sbi->sec_per_clus - 1);
	if (!offset) {
		/* TODO: multiple cluster allocation would be desirable. */

		err = fat_add_cluster(inode);
		if (err)
			return err;
	}
	/* available blocks on this cluster */
	mapped_blocks = sbi->sec_per_clus - offset;

	*max_blocks = min(mapped_blocks, *max_blocks);
	MSDOS_I(inode)->mmu_private += *max_blocks << sb->s_blocksize_bits;

	err = fat_bmap(inode, iblock, &phys, &mapped_blocks, create);
	if (err)
		return err;

	BUG_ON(!phys);
	BUG_ON(*max_blocks != mapped_blocks);
	set_buffer_new(bh_result);
	map_bh(bh_result, sb, phys);

	return 0;
}

static int fat_get_block(struct inode *inode, sector_t iblock,
			 struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	unsigned long max_blocks = bh_result->b_size >> inode->i_blkbits;
	int err;

	err = __fat_get_block(inode, iblock, &max_blocks, bh_result, create);
	if (err)
		return err;
	bh_result->b_size = max_blocks << sb->s_blocksize_bits;

	return 0;
}

static int fat_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, fat_get_block, wbc);
}

static int fat_writepages(struct address_space *mapping,
			  struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, fat_get_block);
}

static int fat_readpage(struct file *file, struct page *page)
{
	return mpage_readpage(page, fat_get_block);
}

static int fat_readpages(struct file *file, struct address_space *mapping,
			 struct list_head *pages, unsigned nr_pages)
{
	return mpage_readpages(mapping, pages, nr_pages, fat_get_block);
}

static void fat_write_failed(struct address_space *mapping, loff_t to)
{
	struct inode *inode = mapping->host;

	if (to > inode->i_size) {
		truncate_pagecache(inode, to, inode->i_size);
		fat_truncate_blocks(inode, inode->i_size);
	}
}

static int fat_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata)
{
	int err;

	*pagep = NULL;
	err = cont_write_begin(file, mapping, pos, len, flags,
				pagep, fsdata, fat_get_block,
				&MSDOS_I(mapping->host)->mmu_private);
	if (err < 0)
	{
		fat_write_failed(mapping, pos + len);
	}
	return err;
}

static int fat_write_end(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct page *pagep, void *fsdata)
{
	struct inode *inode = mapping->host;
	int err;
	err = generic_write_end(file, mapping, pos, len, copied, pagep, fsdata);
	if (err < len)
		fat_write_failed(mapping, pos + len);
	if (!(err < 0) && !(MSDOS_I(inode)->i_attrs & ATTR_ARCH)) {
		inode->i_mtime = inode->i_ctime = CURRENT_TIME_SEC;
		MSDOS_I(inode)->i_attrs |= ATTR_ARCH;
		mark_inode_dirty(inode);
	}
	return err;
}

static ssize_t fat_direct_IO(int rw, struct kiocb *iocb,
			     const struct iovec *iov,
			     loff_t offset, unsigned long nr_segs)
{
	struct file *file = iocb->ki_filp;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	ssize_t ret;

	if (rw == WRITE) {
		/*
		 * FIXME: blockdev_direct_IO() doesn't use ->write_begin(),
		 * so we need to update the ->mmu_private to block boundary.
		 *
		 * But we must fill the remaining area or hole by nul for
		 * updating ->mmu_private.
		 *
		 * Return 0, and fallback to normal buffered write.
		 */
		loff_t size = offset + iov_length(iov, nr_segs);
		if (MSDOS_I(inode)->mmu_private < size)
			return 0;
	}

	/*
	 * FAT need to use the DIO_LOCKING for avoiding the race
	 * condition of fat_get_block() and ->truncate().
	 */
	ret = blockdev_direct_IO(rw, iocb, inode, iov, offset, nr_segs,
				 fat_get_block);
	if (ret < 0 && (rw & WRITE))
		fat_write_failed(mapping, offset + iov_length(iov, nr_segs));

	return ret;
}

static sector_t _fat_bmap(struct address_space *mapping, sector_t block)
{
	sector_t blocknr;

	/* fat_get_cluster() assumes the requested blocknr isn't truncated. */
	down_read(&MSDOS_I(mapping->host)->truncate_lock);
	blocknr = generic_block_bmap(mapping, block, fat_get_block);
	up_read(&MSDOS_I(mapping->host)->truncate_lock);

	return blocknr;
}

static const struct address_space_operations fat_aops = {
	.readpage	= fat_readpage,
	.readpages	= fat_readpages,
	.writepage	= fat_writepage,
	.writepages	= fat_writepages,
	.write_begin	= fat_write_begin,
	.write_end	= fat_write_end,
	.direct_IO	= fat_direct_IO,
	.bmap		= _fat_bmap
};

/*
 * New FAT inode stuff. We do the following:
 *	a) i_ino is constant and has nothing with on-disk location.
 *	b) FAT manages its own cache of directory entries.
 *	c) *This* cache is indexed by on-disk location.
 *	d) inode has an associated directory entry, all right, but
 *		it may be unhashed.
 *	e) currently entries are stored within struct inode. That should
 *		change.
 *	f) we deal with races in the following way:
 *		1. readdir() and lookup() do FAT-dir-cache lookup.
 *		2. rename() unhashes the F-d-c entry and rehashes it in
 *			a new place.
 *		3. unlink() and rmdir() unhash F-d-c entry.
 *		4. fat_write_inode() checks whether the thing is unhashed.
 *			If it is we silently return. If it isn't we do bread(),
 *			check if the location is still valid and retry if it
 *			isn't. Otherwise we do changes.
 *		5. Spinlock is used to protect hash/unhash/location check/lookup
 *		6. fat_evict_inode() unhashes the F-d-c entry.
 *		7. lookup() and readdir() do igrab() if they find a F-d-c entry
 *			and consider negative result as cache miss.
 */

static void fat_hash_init(struct super_block *sb)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	int i;

	spin_lock_init(&sbi->inode_hash_lock);
	for (i = 0; i < FAT_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&sbi->inode_hashtable[i]);
}

static inline unsigned long fat_hash(loff_t i_pos)
{
	return hash_32(i_pos, FAT_HASH_BITS);
}

static void dir_hash_init(struct super_block *sb)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	int i;

	spin_lock_init(&sbi->dir_hash_lock);
	for (i = 0; i < FAT_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&sbi->dir_hashtable[i]);
}

void fat_attach(struct inode *inode, loff_t i_pos)
{
	struct msdos_sb_info *sbi = MSDOS_SB(inode->i_sb);

	if (inode->i_ino != MSDOS_ROOT_INO) {
		struct hlist_head *head =   sbi->inode_hashtable
					  + fat_hash(i_pos);

		spin_lock(&sbi->inode_hash_lock);
		MSDOS_I(inode)->i_pos = i_pos;
		hlist_add_head(&MSDOS_I(inode)->i_fat_hash, head);
		spin_unlock(&sbi->inode_hash_lock);
	}

	/* If NFS support is enabled, cache the mapping of start cluster
	 * to directory inode. This is used during reconnection of
	 * dentries to the filesystem root.
	 */
	if (S_ISDIR(inode->i_mode) && sbi->options.nfs) {
		struct hlist_head *d_head = sbi->dir_hashtable;
		d_head += fat_dir_hash(MSDOS_I(inode)->i_logstart);

		spin_lock(&sbi->dir_hash_lock);
		hlist_add_head(&MSDOS_I(inode)->i_dir_hash, d_head);
		spin_unlock(&sbi->dir_hash_lock);
	}
}
EXPORT_SYMBOL_GPL(fat_attach);

void fat_detach(struct inode *inode)
{
	struct msdos_sb_info *sbi = MSDOS_SB(inode->i_sb);
	spin_lock(&sbi->inode_hash_lock);
	MSDOS_I(inode)->i_pos = 0;
	hlist_del_init(&MSDOS_I(inode)->i_fat_hash);
	spin_unlock(&sbi->inode_hash_lock);

	if (S_ISDIR(inode->i_mode) && sbi->options.nfs) {
		spin_lock(&sbi->dir_hash_lock);
		hlist_del_init(&MSDOS_I(inode)->i_dir_hash);
		spin_unlock(&sbi->dir_hash_lock);
	}
}
EXPORT_SYMBOL_GPL(fat_detach);

struct inode *fat_iget(struct super_block *sb, loff_t i_pos)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct hlist_head *head = sbi->inode_hashtable + fat_hash(i_pos);
	struct msdos_inode_info *i;
	struct inode *inode = NULL;

	spin_lock(&sbi->inode_hash_lock);
	hlist_for_each_entry(i, head, i_fat_hash) {

		BUG_ON(i->vfs_inode.i_sb != sb);
		if (i->i_pos != i_pos)
			continue;
		inode = igrab(&i->vfs_inode);
		if (inode)
			break;
	}

	spin_unlock(&sbi->inode_hash_lock);

	return inode;
}

static int is_exec(unsigned char *extension)
{
	unsigned char *exe_extensions = "EXECOMBAT", *walk;

	for (walk = exe_extensions; *walk; walk += 3)
		if (!strncmp(extension, walk, 3))
			return 1;
	return 0;
}

static int fat_calc_dir_size(struct inode *inode)
{
	struct msdos_sb_info *sbi = MSDOS_SB(inode->i_sb);
	int ret, fclus, dclus;

	inode->i_size = 0;
	if (MSDOS_I(inode)->i_start == 0)
		return 0;

	ret = fat_get_cluster(inode, FAT_ENT_EOF, &fclus, &dclus);
	if (ret < 0)
		return ret;
	inode->i_size = (fclus + 1) << sbi->cluster_bits;

	return 0;
}

/* doesn't deal with root inode */
int fat_fill_inode(struct inode *inode, struct msdos_dir_entry *de)
{
	struct msdos_sb_info *sbi = MSDOS_SB(inode->i_sb);
	int error;

	MSDOS_I(inode)->i_pos = 0;
	inode->i_uid = sbi->options.fs_uid;
	inode->i_gid = sbi->options.fs_gid;
	inode->i_version++;
	inode->i_generation = get_seconds();

	if ((de->attr & ATTR_DIR) && !IS_FREE(de->name)) {
		inode->i_generation &= ~1;
		inode->i_mode = fat_make_mode(sbi, de->attr, S_IRWXUGO);
		inode->i_op = sbi->dir_ops;
		inode->i_fop = &fat_dir_operations;

		MSDOS_I(inode)->i_start = fat_get_start(sbi, de);
		MSDOS_I(inode)->i_logstart = MSDOS_I(inode)->i_start;
		error = fat_calc_dir_size(inode);
		if (error < 0)
			return error;
		MSDOS_I(inode)->mmu_private = inode->i_size;

		set_nlink(inode, fat_subdirs(inode));
	} else { /* not a directory */
		inode->i_generation |= 1;
		inode->i_mode = fat_make_mode(sbi, de->attr,
			((sbi->options.showexec && !is_exec(de->name + 8))
			 ? S_IRUGO|S_IWUGO : S_IRWXUGO));
		MSDOS_I(inode)->i_start = fat_get_start(sbi, de);

		MSDOS_I(inode)->i_logstart = MSDOS_I(inode)->i_start;
		inode->i_size = le32_to_cpu(de->size);
		inode->i_op = &fat_file_inode_operations;
		inode->i_fop = &fat_file_operations;
		inode->i_mapping->a_ops = &fat_aops;
		MSDOS_I(inode)->mmu_private = inode->i_size;
	}
	if (de->attr & ATTR_SYS) {
		if (sbi->options.sys_immutable)
			inode->i_flags |= S_IMMUTABLE;
	}
	fat_save_attrs(inode, de->attr);

	inode->i_blocks = ((inode->i_size + (sbi->cluster_size - 1))
			   & ~((loff_t)sbi->cluster_size - 1)) >> 9;

	fat_time_fat2unix(sbi, &inode->i_mtime, de->time, de->date, 0);
	if (sbi->options.isvfat) {
		fat_time_fat2unix(sbi, &inode->i_ctime, de->ctime,
				  de->cdate, de->ctime_cs);
		fat_time_fat2unix(sbi, &inode->i_atime, 0, de->adate, 0);
	} else
	{
		inode->i_ctime = inode->i_atime = inode->i_mtime;
	}

	return 0;
}

static inline void fat_lock_build_inode(struct msdos_sb_info *sbi)
{
	if (sbi->options.nfs == FAT_NFS_NOSTALE_RO)
		mutex_lock(&sbi->nfs_build_inode_lock);
}

static inline void fat_unlock_build_inode(struct msdos_sb_info *sbi)
{
	if (sbi->options.nfs == FAT_NFS_NOSTALE_RO)
		mutex_unlock(&sbi->nfs_build_inode_lock);
}

struct inode *fat_build_inode(struct super_block *sb,
			struct msdos_dir_entry *de, loff_t i_pos)
{
	struct inode *inode;
	int err;

	fat_lock_build_inode(MSDOS_SB(sb));
	inode = fat_iget(sb, i_pos);
	if (inode)
		goto out;
	inode = new_inode(sb);
	if (!inode) {
		inode = ERR_PTR(-ENOMEM);
		goto out;
	}
	inode->i_ino = iunique(sb, MSDOS_ROOT_INO);
	inode->i_version = 1;
	err = fat_fill_inode(inode, de);
	if (err) {
		iput(inode);
		inode = ERR_PTR(err);
		goto out;
	}
	fat_attach(inode, i_pos);
	insert_inode_hash(inode);

out:
	fat_unlock_build_inode(MSDOS_SB(sb));
	return inode;
}

EXPORT_SYMBOL_GPL(fat_build_inode);

static void fat_evict_inode(struct inode *inode)
{
	truncate_inode_pages(&inode->i_data, 0);
	if (!inode->i_nlink) {
		inode->i_size = 0;
		fat_truncate_blocks(inode, 0);
	}
	invalidate_inode_buffers(inode);
	clear_inode(inode);
	fat_cache_inval_inode(inode);
	fat_detach(inode);
}

static void fat_set_state(struct super_block *sb,
			unsigned int set, unsigned int force)
{
	struct buffer_head *bh;
	struct fat_boot_sector *b;
	struct msdos_sb_info *sbi = sb->s_fs_info;

	/* do not change any thing if mounted read only */
	if ((sb->s_flags & MS_RDONLY) && !force)
		return;

	/* do not change state if fs was dirty */
	if (sbi->dirty) {
		/* warn only on set (mount). */
		if (set)
			fat_msg(sb, KERN_WARNING, "Volume was not properly "
				"unmounted. Some data may be corrupt. "
				"Please run fsck.");
		return;
	}

	bh = sb_bread(sb, 0);
	if (bh == NULL) {
		fat_msg(sb, KERN_ERR, "unable to read boot sector "
			"to mark fs as dirty");
		return;
	}

	b = (struct fat_boot_sector *) bh->b_data;

	if (sbi->fat_bits == 32) {
		if (set)
			b->fat32.state |= FAT_STATE_DIRTY;
		else
			b->fat32.state &= ~FAT_STATE_DIRTY;
	} else /* fat 16 and 12 */ {
		if (set)
			b->fat16.state |= FAT_STATE_DIRTY;
		else
			b->fat16.state &= ~FAT_STATE_DIRTY;
	}

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
}

static void fat_put_super(struct super_block *sb)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);

	fat_set_state(sb, 0, 0);

	iput(sbi->fsinfo_inode);
	iput(sbi->fat_inode);

	unload_nls(sbi->nls_disk);
	unload_nls(sbi->nls_io);

	if (sbi->options.iocharset != fat_default_iocharset)
		kfree(sbi->options.iocharset);

	sb->s_fs_info = NULL;
	kfree(sbi);
}

static struct kmem_cache *fat_inode_cachep;

static struct inode *fat_alloc_inode(struct super_block *sb)
{
	struct msdos_inode_info *ei;
	ei = kmem_cache_alloc(fat_inode_cachep, GFP_NOFS);
	if (!ei)
		return NULL;

	ei->pre_alloced =0;
	ei->pre_count = 0;

	init_rwsem(&ei->truncate_lock);
	return &ei->vfs_inode;
}

static void fat_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	kmem_cache_free(fat_inode_cachep, MSDOS_I(inode));
}

static void fat_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, fat_i_callback);
}

static void init_once(void *foo)
{
	struct msdos_inode_info *ei = (struct msdos_inode_info *)foo;

	spin_lock_init(&ei->cache_lru_lock);
	ei->nr_caches = 0;
	ei->cache_valid_id = FAT_CACHE_VALID + 1;
	INIT_LIST_HEAD(&ei->cache_lru);
	INIT_HLIST_NODE(&ei->i_fat_hash);
	INIT_HLIST_NODE(&ei->i_dir_hash);
	inode_init_once(&ei->vfs_inode);
}

static int __init fat_init_inodecache(void)
{
	fat_inode_cachep = kmem_cache_create("fat_inode_cache",
					     sizeof(struct msdos_inode_info),
					     0, (SLAB_RECLAIM_ACCOUNT|
						SLAB_MEM_SPREAD),
					     init_once);
	if (fat_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void __exit fat_destroy_inodecache(void)
{
	/*
	 * Make sure all delayed rcu free inodes are flushed before we
	 * destroy cache.
	 */
	rcu_barrier();
	kmem_cache_destroy(fat_inode_cachep);
}

static int fat_remount(struct super_block *sb, int *flags, char *data)
{
	int new_rdonly;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	*flags |= MS_NODIRATIME | (sbi->options.isvfat ? 0 : MS_NOATIME);

	/* make sure we update state on remount. */
	new_rdonly = *flags & MS_RDONLY;
	if (new_rdonly != (sb->s_flags & MS_RDONLY)) {
		if (new_rdonly)
			fat_set_state(sb, 0, 0);
		else
			fat_set_state(sb, 1, 1);
	}
	return 0;
}

static int fat_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);

	/* If the count of free cluster is still unknown, counts it here. */
	if (sbi->free_clusters == -1 || !sbi->free_clus_valid) {
		int err = fat_count_free_clusters(dentry->d_sb);
		if (err)
			return err;
	}

	buf->f_type = dentry->d_sb->s_magic;
	buf->f_bsize = sbi->cluster_size;
	buf->f_blocks = sbi->max_cluster - FAT_START_ENT;
	buf->f_bfree = sbi->free_clusters;
	buf->f_bavail = sbi->free_clusters;
	buf->f_fsid.val[0] = (u32)id;
	buf->f_fsid.val[1] = (u32)(id >> 32);
	buf->f_namelen =
		(sbi->options.isvfat ? FAT_LFN_LEN : 12) * NLS_MAX_CHARSET_SIZE;

	return 0;
}

static int __fat_write_inode(struct inode *inode, int wait)
{
	struct super_block *sb = inode->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct buffer_head *bh;
	struct msdos_dir_entry *raw_entry;
	loff_t i_pos;
	sector_t blocknr;
	int err, offset;

  if (inode->i_ino == MSDOS_ROOT_INO)
		return 0;

retry:
	i_pos = fat_i_pos_read(sbi, inode);
	if (!i_pos)
		return 0;

	fat_get_blknr_offset(sbi, i_pos, &blocknr, &offset);
	bh = sb_bread(sb, blocknr);
	if (!bh) {
		fat_msg(sb, KERN_ERR, "unable to read inode block "
		       "for updating (i_pos %lld)", i_pos);
		return -EIO;
	}
	spin_lock(&sbi->inode_hash_lock);
	if (i_pos != MSDOS_I(inode)->i_pos) {
		spin_unlock(&sbi->inode_hash_lock);
		brelse(bh);
		goto retry;
	}

	raw_entry = &((struct msdos_dir_entry *) (bh->b_data))[offset];
	if (S_ISDIR(inode->i_mode))
		raw_entry->size = 0;
	else
	{
		raw_entry->size = cpu_to_le32(inode->i_size);
	}
	raw_entry->attr = fat_make_attrs(inode);
	fat_set_start(raw_entry, MSDOS_I(inode)->i_logstart);
	fat_time_unix2fat(sbi, &inode->i_mtime, &raw_entry->time,
			  &raw_entry->date, NULL);

	if (sbi->options.isvfat) {
		__le16 atime;
		fat_time_unix2fat(sbi, &inode->i_ctime, &raw_entry->ctime,
				  &raw_entry->cdate, &raw_entry->ctime_cs);
		fat_time_unix2fat(sbi, &inode->i_atime, &atime,
				  &raw_entry->adate, NULL);
	}

	spin_unlock(&sbi->inode_hash_lock);
	mark_buffer_dirty(bh);
	err = 0;
	if (wait)
		err = sync_dirty_buffer(bh);
	brelse(bh);
	return err;
}

static int fat_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	int err;

	if (inode->i_ino == MSDOS_FSINFO_INO) {
		struct super_block *sb = inode->i_sb;

		mutex_lock(&MSDOS_SB(sb)->s_lock);
		err = fat_clusters_flush(sb);
		mutex_unlock(&MSDOS_SB(sb)->s_lock);
	} else
		err = __fat_write_inode(inode, wbc->sync_mode == WB_SYNC_ALL);

	return err;
}

int fat_sync_inode(struct inode *inode)
{
	return __fat_write_inode(inode, 1);
}

EXPORT_SYMBOL_GPL(fat_sync_inode);

static int fat_show_options(struct seq_file *m, struct dentry *root);
static const struct super_operations fat_sops = {
	.alloc_inode	= fat_alloc_inode,
	.destroy_inode	= fat_destroy_inode,
	.write_inode	= fat_write_inode,
	.evict_inode	= fat_evict_inode,
	.put_super	= fat_put_super,
	.statfs		= fat_statfs,
	.remount_fs	= fat_remount,

	.show_options	= fat_show_options,
};

static int fat_show_options(struct seq_file *m, struct dentry *root)
{
	struct msdos_sb_info *sbi = MSDOS_SB(root->d_sb);
	struct fat_mount_options *opts = &sbi->options;
	int isvfat = opts->isvfat;

	if (!uid_eq(opts->fs_uid, GLOBAL_ROOT_UID))
		seq_printf(m, ",uid=%u",
				from_kuid_munged(&init_user_ns, opts->fs_uid));
	if (!gid_eq(opts->fs_gid, GLOBAL_ROOT_GID))
		seq_printf(m, ",gid=%u",
				from_kgid_munged(&init_user_ns, opts->fs_gid));
	seq_printf(m, ",fmask=%04o", opts->fs_fmask);
	seq_printf(m, ",dmask=%04o", opts->fs_dmask);
	if (opts->allow_utime)
		seq_printf(m, ",allow_utime=%04o", opts->allow_utime);
	if (sbi->nls_disk)
		/* strip "cp" prefix from displayed option */
		seq_printf(m, ",codepage=%s", &sbi->nls_disk->charset[2]);
	if (isvfat) {
		if (sbi->nls_io)
			seq_printf(m, ",iocharset=%s", sbi->nls_io->charset);

		switch (opts->shortname) {
		case VFAT_SFN_DISPLAY_WIN95 | VFAT_SFN_CREATE_WIN95:
			seq_puts(m, ",shortname=win95");
			break;
		case VFAT_SFN_DISPLAY_WINNT | VFAT_SFN_CREATE_WINNT:
			seq_puts(m, ",shortname=winnt");
			break;
		case VFAT_SFN_DISPLAY_WINNT | VFAT_SFN_CREATE_WIN95:
			seq_puts(m, ",shortname=mixed");
			break;
		case VFAT_SFN_DISPLAY_LOWER | VFAT_SFN_CREATE_WIN95:
			seq_puts(m, ",shortname=lower");
			break;
		default:
			seq_puts(m, ",shortname=unknown");
			break;
		}
	}
	if (opts->name_check != 'n')
		seq_printf(m, ",check=%c", opts->name_check);
	if (opts->usefree)
		seq_puts(m, ",usefree");
	if (opts->quiet)
		seq_puts(m, ",quiet");
	if (opts->showexec)
		seq_puts(m, ",showexec");
	if (opts->sys_immutable)
		seq_puts(m, ",sys_immutable");
	if (!isvfat) {
		if (opts->dotsOK)
			seq_puts(m, ",dotsOK=yes");
		if (opts->nocase)
			seq_puts(m, ",nocase");
	} else {
		if (opts->utf8)
			seq_puts(m, ",utf8");
		if (opts->unicode_xlate)
			seq_puts(m, ",uni_xlate");
		if (!opts->numtail)
			seq_puts(m, ",nonumtail");
		if (opts->rodir)
			seq_puts(m, ",rodir");
	}
	if (opts->flush)
		seq_puts(m, ",flush");
	if (opts->tz_set) {
		if (opts->time_offset)
			seq_printf(m, ",time_offset=%d", opts->time_offset);
		else
			seq_puts(m, ",tz=UTC");
	}
	if (opts->errors == FAT_ERRORS_CONT)
		seq_puts(m, ",errors=continue");
	else if (opts->errors == FAT_ERRORS_PANIC)
		seq_puts(m, ",errors=panic");
	else
		seq_puts(m, ",errors=remount-ro");
	if (opts->nfs == FAT_NFS_NOSTALE_RO)
		seq_puts(m, ",nfs=nostale_ro");
	else if (opts->nfs)
		seq_puts(m, ",nfs=stale_rw");
	if (opts->discard)
		seq_puts(m, ",discard");

	return 0;
}

enum {
	Opt_check_n, Opt_check_r, Opt_check_s, Opt_uid, Opt_gid,
	Opt_umask, Opt_dmask, Opt_fmask, Opt_allow_utime, Opt_codepage,
	Opt_usefree, Opt_nocase, Opt_quiet, Opt_showexec, Opt_debug,
	Opt_immutable, Opt_dots, Opt_nodots,
	Opt_charset, Opt_shortname_lower, Opt_shortname_win95,
	Opt_shortname_winnt, Opt_shortname_mixed, Opt_utf8_no, Opt_utf8_yes,
	Opt_uni_xl_no, Opt_uni_xl_yes, Opt_nonumtail_no, Opt_nonumtail_yes,
	Opt_obsolete, Opt_flush, Opt_tz_utc, Opt_rodir, Opt_err_cont,
	Opt_err_panic, Opt_err_ro, Opt_discard, Opt_nfs, Opt_time_offset,
	Opt_nfs_stale_rw, Opt_nfs_nostale_ro, Opt_err,
};

static const match_table_t fat_tokens = {
	{Opt_check_r, "check=relaxed"},
	{Opt_check_s, "check=strict"},
	{Opt_check_n, "check=normal"},
	{Opt_check_r, "check=r"},
	{Opt_check_s, "check=s"},
	{Opt_check_n, "check=n"},
	{Opt_uid, "uid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_umask, "umask=%o"},
	{Opt_dmask, "dmask=%o"},
	{Opt_fmask, "fmask=%o"},
	{Opt_allow_utime, "allow_utime=%o"},
	{Opt_codepage, "codepage=%u"},
	{Opt_usefree, "usefree"},
	{Opt_nocase, "nocase"},
	{Opt_quiet, "quiet"},
	{Opt_showexec, "showexec"},
	{Opt_debug, "debug"},
	{Opt_immutable, "sys_immutable"},
	{Opt_flush, "flush"},
	{Opt_tz_utc, "tz=UTC"},
	{Opt_time_offset, "time_offset=%d"},
	{Opt_err_cont, "errors=continue"},
	{Opt_err_panic, "errors=panic"},
	{Opt_err_ro, "errors=remount-ro"},
	{Opt_discard, "discard"},
	{Opt_nfs_stale_rw, "nfs"},
	{Opt_nfs_stale_rw, "nfs=stale_rw"},
	{Opt_nfs_nostale_ro, "nfs=nostale_ro"},
	{Opt_obsolete, "conv=binary"},
	{Opt_obsolete, "conv=text"},
	{Opt_obsolete, "conv=auto"},
	{Opt_obsolete, "conv=b"},
	{Opt_obsolete, "conv=t"},
	{Opt_obsolete, "conv=a"},
	{Opt_obsolete, "fat=%u"},
	{Opt_obsolete, "blocksize=%u"},
	{Opt_obsolete, "cvf_format=%20s"},
	{Opt_obsolete, "cvf_options=%100s"},
	{Opt_obsolete, "posix"},
	{Opt_err, NULL},
};
static const match_table_t msdos_tokens = {
	{Opt_nodots, "nodots"},
	{Opt_nodots, "dotsOK=no"},
	{Opt_dots, "dots"},
	{Opt_dots, "dotsOK=yes"},
	{Opt_err, NULL}
};
static const match_table_t vfat_tokens = {
	{Opt_charset, "iocharset=%s"},
	{Opt_shortname_lower, "shortname=lower"},
	{Opt_shortname_win95, "shortname=win95"},
	{Opt_shortname_winnt, "shortname=winnt"},
	{Opt_shortname_mixed, "shortname=mixed"},
	{Opt_utf8_no, "utf8=0"},		/* 0 or no or false */
	{Opt_utf8_no, "utf8=no"},
	{Opt_utf8_no, "utf8=false"},
	{Opt_utf8_yes, "utf8=1"},		/* empty or 1 or yes or true */
	{Opt_utf8_yes, "utf8=yes"},
	{Opt_utf8_yes, "utf8=true"},
	{Opt_utf8_yes, "utf8"},
	{Opt_uni_xl_no, "uni_xlate=0"},		/* 0 or no or false */
	{Opt_uni_xl_no, "uni_xlate=no"},
	{Opt_uni_xl_no, "uni_xlate=false"},
	{Opt_uni_xl_yes, "uni_xlate=1"},	/* empty or 1 or yes or true */
	{Opt_uni_xl_yes, "uni_xlate=yes"},
	{Opt_uni_xl_yes, "uni_xlate=true"},
	{Opt_uni_xl_yes, "uni_xlate"},
	{Opt_nonumtail_no, "nonumtail=0"},	/* 0 or no or false */
	{Opt_nonumtail_no, "nonumtail=no"},
	{Opt_nonumtail_no, "nonumtail=false"},
	{Opt_nonumtail_yes, "nonumtail=1"},	/* empty or 1 or yes or true */
	{Opt_nonumtail_yes, "nonumtail=yes"},
	{Opt_nonumtail_yes, "nonumtail=true"},
	{Opt_nonumtail_yes, "nonumtail"},
	{Opt_rodir, "rodir"},
	{Opt_err, NULL}
};

static int parse_options(struct super_block *sb, char *options, int is_vfat,
			 int silent, int *debug, struct fat_mount_options *opts)
{
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int option;
	char *iocharset;

	opts->isvfat = is_vfat;

	opts->fs_uid = current_uid();
	opts->fs_gid = current_gid();
	opts->fs_fmask = opts->fs_dmask = current_umask();
	opts->allow_utime = -1;
	opts->codepage = fat_default_codepage;
	opts->iocharset = fat_default_iocharset;
	if (is_vfat) {
		opts->shortname = VFAT_SFN_DISPLAY_WINNT|VFAT_SFN_CREATE_WIN95;
		opts->rodir = 0;
	} else {
		opts->shortname = 0;
		opts->rodir = 1;
	}
	opts->name_check = 'n';
	opts->quiet = opts->showexec = opts->sys_immutable = opts->dotsOK =  0;
	opts->utf8 = opts->unicode_xlate = 0;
	opts->numtail = 1;
	opts->usefree = opts->nocase = 0;
	opts->tz_set = 0;
	opts->nfs = 0;
	opts->errors = FAT_ERRORS_RO;
	*debug = 0;

	if (!options)
		goto out;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;
		if (!*p)
			continue;

		token = match_token(p, fat_tokens, args);
		if (token == Opt_err) {
			if (is_vfat)
				token = match_token(p, vfat_tokens, args);
			else
				token = match_token(p, msdos_tokens, args);
		}
		switch (token) {
		case Opt_check_s:
			opts->name_check = 's';
			break;
		case Opt_check_r:
			opts->name_check = 'r';
			break;
		case Opt_check_n:
			opts->name_check = 'n';
			break;
		case Opt_usefree:
			opts->usefree = 1;
			break;
		case Opt_nocase:
			if (!is_vfat)
				opts->nocase = 1;
			else {
				/* for backward compatibility */
				opts->shortname = VFAT_SFN_DISPLAY_WIN95
					| VFAT_SFN_CREATE_WIN95;
			}
			break;
		case Opt_quiet:
			opts->quiet = 1;
			break;
		case Opt_showexec:
			opts->showexec = 1;
			break;
		case Opt_debug:
			*debug = 1;
			break;
		case Opt_immutable:
			opts->sys_immutable = 1;
			break;
		case Opt_uid:
			if (match_int(&args[0], &option))
				return -EINVAL;
			opts->fs_uid = make_kuid(current_user_ns(), option);
			if (!uid_valid(opts->fs_uid))
				return -EINVAL;
			break;
		case Opt_gid:
			if (match_int(&args[0], &option))
				return -EINVAL;
			opts->fs_gid = make_kgid(current_user_ns(), option);
			if (!gid_valid(opts->fs_gid))
				return -EINVAL;
			break;
		case Opt_umask:
			if (match_octal(&args[0], &option))
				return -EINVAL;
			opts->fs_fmask = opts->fs_dmask = option;
			break;
		case Opt_dmask:
			if (match_octal(&args[0], &option))
				return -EINVAL;
			opts->fs_dmask = option;
			break;
		case Opt_fmask:
			if (match_octal(&args[0], &option))
				return -EINVAL;
			opts->fs_fmask = option;
			break;
		case Opt_allow_utime:
			if (match_octal(&args[0], &option))
				return -EINVAL;
			opts->allow_utime = option & (S_IWGRP | S_IWOTH);
			break;
		case Opt_codepage:
			if (match_int(&args[0], &option))
				return -EINVAL;
			opts->codepage = option;
			break;
		case Opt_flush:
			opts->flush = 1;
			break;
		case Opt_time_offset:
			if (match_int(&args[0], &option))
				return -EINVAL;
			if (option < -12 * 60 || option > 12 * 60)
				return -EINVAL;
			opts->tz_set = 1;
			opts->time_offset = option;
			break;
		case Opt_tz_utc:
			opts->tz_set = 1;
			opts->time_offset = 0;
			break;
		case Opt_err_cont:
			opts->errors = FAT_ERRORS_CONT;
			break;
		case Opt_err_panic:
			opts->errors = FAT_ERRORS_PANIC;
			break;
		case Opt_err_ro:
			opts->errors = FAT_ERRORS_RO;
			break;
		case Opt_nfs_stale_rw:
			opts->nfs = FAT_NFS_STALE_RW;
			break;
		case Opt_nfs_nostale_ro:
			opts->nfs = FAT_NFS_NOSTALE_RO;
			break;

		/* msdos specific */
		case Opt_dots:
			opts->dotsOK = 1;
			break;
		case Opt_nodots:
			opts->dotsOK = 0;
			break;

		/* vfat specific */
		case Opt_charset:
			if (opts->iocharset != fat_default_iocharset)
				kfree(opts->iocharset);
			iocharset = match_strdup(&args[0]);
			if (!iocharset)
				return -ENOMEM;
			opts->iocharset = iocharset;
			break;
		case Opt_shortname_lower:
			opts->shortname = VFAT_SFN_DISPLAY_LOWER
					| VFAT_SFN_CREATE_WIN95;
			break;
		case Opt_shortname_win95:
			opts->shortname = VFAT_SFN_DISPLAY_WIN95
					| VFAT_SFN_CREATE_WIN95;
			break;
		case Opt_shortname_winnt:
			opts->shortname = VFAT_SFN_DISPLAY_WINNT
					| VFAT_SFN_CREATE_WINNT;
			break;
		case Opt_shortname_mixed:
			opts->shortname = VFAT_SFN_DISPLAY_WINNT
					| VFAT_SFN_CREATE_WIN95;
			break;
		case Opt_utf8_no:		/* 0 or no or false */
			opts->utf8 = 0;
			break;
		case Opt_utf8_yes:		/* empty or 1 or yes or true */
			opts->utf8 = 1;
			break;
		case Opt_uni_xl_no:		/* 0 or no or false */
			opts->unicode_xlate = 0;
			break;
		case Opt_uni_xl_yes:		/* empty or 1 or yes or true */
			opts->unicode_xlate = 1;
			break;
		case Opt_nonumtail_no:		/* 0 or no or false */
			opts->numtail = 1;	/* negated option */
			break;
		case Opt_nonumtail_yes:		/* empty or 1 or yes or true */
			opts->numtail = 0;	/* negated option */
			break;
		case Opt_rodir:
			opts->rodir = 1;
			break;
		case Opt_discard:
			opts->discard = 1;
			break;

		/* obsolete mount options */
		case Opt_obsolete:
			fat_msg(sb, KERN_INFO, "\"%s\" option is obsolete, "
			       "not supported now", p);
			break;
		/* unknown option */
		default:
			if (!silent) {
				fat_msg(sb, KERN_ERR,
				       "Unrecognized mount option \"%s\" "
				       "or missing value", p);
			}
			return -EINVAL;
		}
	}

out:
	/* UTF-8 doesn't provide FAT semantics */
	if (!strcmp(opts->iocharset, "utf8")) {
		fat_msg(sb, KERN_WARNING, "utf8 is not a recommended IO charset"
		       " for FAT filesystems, filesystem will be "
		       "case sensitive!");
	}

	/* If user doesn't specify allow_utime, it's initialized from dmask. */
	if (opts->allow_utime == (unsigned short)-1)
		opts->allow_utime = ~opts->fs_dmask & (S_IWGRP | S_IWOTH);
	if (opts->unicode_xlate)
		opts->utf8 = 0;
	if (opts->nfs == FAT_NFS_NOSTALE_RO) {
		sb->s_flags |= MS_RDONLY;
		sb->s_export_op = &fat_export_ops_nostale;
	}

	return 0;
}

static int fat_read_root(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	int error;

	MSDOS_I(inode)->i_pos = MSDOS_ROOT_INO;
	inode->i_uid = sbi->options.fs_uid;
	inode->i_gid = sbi->options.fs_gid;
	inode->i_version++;
	inode->i_generation = 0;
	inode->i_mode = fat_make_mode(sbi, ATTR_DIR, S_IRWXUGO);
	inode->i_op = sbi->dir_ops;
	inode->i_fop = &fat_dir_operations;
	if (sbi->fat_bits == 32) {
		MSDOS_I(inode)->i_start = sbi->root_cluster;
		error = fat_calc_dir_size(inode);
		if (error < 0)
			return error;
	} else {
		MSDOS_I(inode)->i_start = 0;
		inode->i_size = sbi->dir_entries * sizeof(struct msdos_dir_entry);
	}
	inode->i_blocks = ((inode->i_size + (sbi->cluster_size - 1))
			   & ~((loff_t)sbi->cluster_size - 1)) >> 9;
	MSDOS_I(inode)->i_logstart = 0;
	MSDOS_I(inode)->mmu_private = inode->i_size;

	fat_save_attrs(inode, ATTR_DIR);
	inode->i_mtime.tv_sec = inode->i_atime.tv_sec = inode->i_ctime.tv_sec = 0;
	inode->i_mtime.tv_nsec = inode->i_atime.tv_nsec = inode->i_ctime.tv_nsec = 0;
	set_nlink(inode, fat_subdirs(inode)+2);

	return 0;
}

static unsigned long calc_fat_clusters(struct super_block *sb)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);

	/* Divide first to avoid overflow */
	if (sbi->fat_bits != 12) {
		unsigned long ent_per_sec = sb->s_blocksize * 8 / sbi->fat_bits;
		return ent_per_sec * sbi->fat_length;
	}

	return sbi->fat_length * sb->s_blocksize * 8 / sbi->fat_bits;
}

/*
 * Read the super block of an MS-DOS FS.
 */
int fat_fill_super(struct super_block *sb, void *data, int silent, int isvfat,
		   void (*setup)(struct super_block *))
{
	struct inode *root_inode = NULL, *fat_inode = NULL;
	struct inode *fsinfo_inode = NULL;
	struct buffer_head *bh;
	struct fat_boot_sector *b;
	struct msdos_sb_info *sbi;
	u16 logical_sector_size;
	u32 total_sectors, total_clusters, fat_clusters, rootdir_sectors;
	int debug;
	unsigned int media;
	long error;
	char buf[50];

	/*
	 * GFP_KERNEL is ok here, because while we do hold the
	 * supeblock lock, memory pressure can't call back into
	 * the filesystem, since we're only just about to mount
	 * it and have no inodes etc active!
	 */
	sbi = kzalloc(sizeof(struct msdos_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;

	sb->s_flags |= MS_NODIRATIME;
	sb->s_magic = MSDOS_SUPER_MAGIC;
	sb->s_op = &fat_sops;
	sb->s_export_op = &fat_export_ops;
	mutex_init(&sbi->nfs_build_inode_lock);
	ratelimit_state_init(&sbi->ratelimit, DEFAULT_RATELIMIT_INTERVAL,
			     DEFAULT_RATELIMIT_BURST);

	error = parse_options(sb, data, isvfat, silent, &debug, &sbi->options);
	if (error)
		goto out_fail;

	setup(sb); /* flavour-specific stuff that needs options */

	error = -EIO;
	sb_min_blocksize(sb, 512);

	printk( KERN_ALERT "[opel_fat] sb->blocksize : %u\n", sb->s_blocksize);

	bh = sb_bread(sb, 0);
	if (bh == NULL) {
		fat_msg(sb, KERN_ERR, "unable to read boot sector");
		goto out_fail;
	}

	b = (struct fat_boot_sector *) bh->b_data;
	if (!b->reserved) {
		if (!silent)
			fat_msg(sb, KERN_ERR, "bogus number of reserved sectors");
		brelse(bh);
		goto out_invalid;
	}
	if (!b->fats) {
		if (!silent)
			fat_msg(sb, KERN_ERR, "bogus number of FAT structure");
		brelse(bh);
		goto out_invalid;
	}

	/*
	 * Earlier we checked here that b->secs_track and b->head are nonzero,
	 * but it turns out valid FAT filesystems can have zero there.
	 */

	media = b->media;
	if (!fat_valid_media(media)) {
		if (!silent)
			fat_msg(sb, KERN_ERR, "invalid media value (0x%02x)",
			       media);
		brelse(bh);
		goto out_invalid;
	}
	logical_sector_size = get_unaligned_le16(&b->sector_size);
 	printk("[opel_fat] logical_sector_size : %u\n", logical_sector_size );

	if (!is_power_of_2(logical_sector_size)
	    || (logical_sector_size < 512)
	    || (logical_sector_size > 4096)) {
		if (!silent)
			fat_msg(sb, KERN_ERR, "bogus logical sector size %u",
			       logical_sector_size);
		brelse(bh);
		goto out_invalid;
	}
	sbi->sec_per_clus = b->sec_per_clus;

	printk("[opel_fat] sec_per_clus : %u \n", sbi->sec_per_clus );

	if (!is_power_of_2(sbi->sec_per_clus)) {
		if (!silent)
			fat_msg(sb, KERN_ERR, "bogus sectors per cluster %u",
			       sbi->sec_per_clus);
		brelse(bh);
		goto out_invalid;
	}

	if (logical_sector_size < sb->s_blocksize) {
		fat_msg(sb, KERN_ERR, "logical sector size too small for device"
		       " (logical sector size = %u)", logical_sector_size);
		brelse(bh);
		goto out_fail;
	}
	if (logical_sector_size > sb->s_blocksize) {
		brelse(bh);

		if (!sb_set_blocksize(sb, logical_sector_size)) {
			fat_msg(sb, KERN_ERR, "unable to set blocksize %u",
			       logical_sector_size);
			goto out_fail;
		}
		bh = sb_bread(sb, 0);
		if (bh == NULL) {
			fat_msg(sb, KERN_ERR, "unable to read boot sector"
			       " (logical sector size = %lu)",
			       sb->s_blocksize);
			goto out_fail;
		}
		b = (struct fat_boot_sector *) bh->b_data;
	}

	mutex_init(&sbi->s_lock);
	sbi->cluster_size = sb->s_blocksize * sbi->sec_per_clus;
	sbi->cluster_bits = ffs(sbi->cluster_size) - 1;
	sbi->fats = b->fats;
	sbi->fat_bits = 0;		/* Don't know yet */
	sbi->fat_start = le16_to_cpu(b->reserved);
	fat_block = sbi->fat_start;  //

	sbi->fat_length = le16_to_cpu(b->fat_length);
	sbi->root_cluster = 0;
	sbi->free_clusters = -1;	/* Don't know yet */
	sbi->free_clus_valid = 0;
	sbi->prev_free = FAT_START_ENT;
	sb->s_maxbytes = 0xffffffff;

	if (!sbi->fat_length && b->fat32.length) {
		struct fat_boot_fsinfo *fsinfo;
		struct buffer_head *fsinfo_bh;

		/* Must be FAT32 */
		sbi->fat_bits = 32;
		sbi->fat_length = le32_to_cpu(b->fat32.length);
		sbi->root_cluster = le32_to_cpu(b->fat32.root_cluster);

		/* MC - if info_sector is 0, don't multiply by 0 */
		sbi->fsinfo_sector = le16_to_cpu(b->fat32.info_sector);
		if (sbi->fsinfo_sector == 0)
			sbi->fsinfo_sector = 1;

		fsinfo_bh = sb_bread(sb, sbi->fsinfo_sector);
		if (fsinfo_bh == NULL) {
			fat_msg(sb, KERN_ERR, "bread failed, FSINFO block"
			       " (sector = %lu)", sbi->fsinfo_sector);
			brelse(bh);
			goto out_fail;
		}

		fsinfo = (struct fat_boot_fsinfo *)fsinfo_bh->b_data;
		if (!IS_FSINFO(fsinfo)) {
			fat_msg(sb, KERN_WARNING, "Invalid FSINFO signature: "
			       "0x%08x, 0x%08x (sector = %lu)",
			       le32_to_cpu(fsinfo->signature1),
			       le32_to_cpu(fsinfo->signature2),
			       sbi->fsinfo_sector);
		} else {
			if (sbi->options.usefree)
				sbi->free_clus_valid = 1;
			sbi->free_clusters = le32_to_cpu(fsinfo->free_clusters);
			sbi->prev_free = le32_to_cpu(fsinfo->next_cluster);
		}

		brelse(fsinfo_bh);
	}

	sbi->dir_per_block = sb->s_blocksize / sizeof(struct msdos_dir_entry);
	sbi->dir_per_block_bits = ffs(sbi->dir_per_block) - 1;

	printk("[opel_fat] sbi->dir_per_block : %d, sbi->dir_per_block_bits : %d \n", sbi->dir_per_block, sbi->dir_per_block_bits );
	printk("[opel_fat] sb->s_blocksize : %u \n", sb->s_blocksize );
	printk("[opel_fat] sbi->cluster_size = sb->s_blocksize * sbi->sec_per_clus : %u\n", sbi->cluster_size );
	printk("[opel_fat] sbi->fats (number of fat) : %u \n", sbi->fats );
	printk("[opel_fat] sbi->fat_length  : %u \n", sbi->fat_length );
	printk("[opel_fat] sbi->fat_start : %u \n", sbi->fat_start );

	sbi->dir_start = sbi->fat_start + sbi->fats * sbi->fat_length;
	sbi->dir_entries = get_unaligned_le16(&b->dir_entries);

	printk("[opel_fat] sbi->dir_start  : %u \n", sbi->dir_start );
	printk("[opel_fat] sbi->dir_entries  : %u \n", sbi->dir_entries );

	if (sbi->dir_entries & (sbi->dir_per_block - 1)) {
		if (!silent)
			fat_msg(sb, KERN_ERR, "bogus directory-entries per block"
			       " (%u)", sbi->dir_entries);
		brelse(bh);
		goto out_invalid;
	}

	rootdir_sectors = sbi->dir_entries
		* sizeof(struct msdos_dir_entry) / sb->s_blocksize;
	sbi->data_start = sbi->dir_start + rootdir_sectors;

	printk("[opel_fat] sbi->data_start  : %u \n", sbi->data_start );

	total_sectors = get_unaligned_le16(&b->sectors);
	if (total_sectors == 0)
		total_sectors = le32_to_cpu(b->total_sect);

	total_clusters = (total_sectors - sbi->data_start) / sbi->sec_per_clus;

	printk("[opel_fat] total_clusters = (total_sectors - sbi->data_start) / sbi->sec_per_clus  : %u \n", total_clusters );

	if (sbi->fat_bits != 32)
		sbi->fat_bits = (total_clusters > MAX_FAT12) ? 16 : 12;

	/* some OSes set FAT_STATE_DIRTY and clean it on unmount. */
	if (sbi->fat_bits == 32)
		sbi->dirty = b->fat32.state & FAT_STATE_DIRTY;
	else /* fat 16 or 12 */
		sbi->dirty = b->fat16.state & FAT_STATE_DIRTY;

	/* check that FAT table does not overflow */
	fat_clusters = calc_fat_clusters(sb);
	total_clusters = min(total_clusters, fat_clusters - FAT_START_ENT);

	printk("[opel_fat] fat_clusters = calc_fat_clusters(sb): %u \n", fat_clusters );
	printk("[opel_fat] total_clusters : min(total_clusters, fat_clusters - FAT_START_ENT) : %u \n", total_clusters );

	if (total_clusters > MAX_FAT(sb)) {
		if (!silent)
			fat_msg(sb, KERN_ERR, "count of clusters too big (%u)",
			       total_clusters);
		brelse(bh);
		goto out_invalid;
	}

	sbi->max_cluster = total_clusters + FAT_START_ENT;
	/* check the free_clusters, it's not necessarily correct */
	if (sbi->free_clusters != -1 && sbi->free_clusters > total_clusters)
		sbi->free_clusters = -1;
	/* check the prev_free, it's not necessarily correct */
	sbi->prev_free %= sbi->max_cluster;
	if (sbi->prev_free < FAT_START_ENT)
		sbi->prev_free = FAT_START_ENT;

	brelse(bh);

	/* set up enough so that it can read an inode */
	fat_hash_init(sb);
	dir_hash_init(sb);
	fat_ent_access_init(sb);

	/*
	 * The low byte of FAT's first entry must have same value with
	 * media-field.  But in real world, too many devices is
	 * writing wrong value.  So, removed that validity check.
	 *
	 * if (FAT_FIRST_ENT(sb, media) != first)
	 */

	error = -EINVAL;
	sprintf(buf, "cp%d", sbi->options.codepage);
	sbi->nls_disk = load_nls(buf);
	if (!sbi->nls_disk) {
		fat_msg(sb, KERN_ERR, "codepage %s not found", buf);
		goto out_fail;
	}

	/* FIXME: utf8 is using iocharset for upper/lower conversion */
	if (sbi->options.isvfat) {
		sbi->nls_io = load_nls(sbi->options.iocharset);
		if (!sbi->nls_io) {
			fat_msg(sb, KERN_ERR, "IO charset %s not found",
			       sbi->options.iocharset);
			goto out_fail;
		}
	}

	error = -ENOMEM;
	fat_inode = new_inode(sb);
	if (!fat_inode)
		goto out_fail;
	MSDOS_I(fat_inode)->i_pos = 0;
	sbi->fat_inode = fat_inode;

	fsinfo_inode = new_inode(sb);
	if (!fsinfo_inode)
		goto out_fail;
	fsinfo_inode->i_ino = MSDOS_FSINFO_INO;
	sbi->fsinfo_inode = fsinfo_inode;
	insert_inode_hash(fsinfo_inode);

	root_inode = new_inode(sb);
	if (!root_inode)
		goto out_fail;
	root_inode->i_ino = MSDOS_ROOT_INO;
	root_inode->i_version = 1;
	error = fat_read_root(root_inode);
	if (error < 0) {
		iput(root_inode);
		goto out_fail;
	}
	error = -ENOMEM;
	insert_inode_hash(root_inode);
	fat_attach(root_inode, 0);
	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		fat_msg(sb, KERN_ERR, "get root inode failed");
		goto out_fail;
	}

	if (sbi->options.discard) {
		struct request_queue *q = bdev_get_queue(sb->s_bdev);
		if (!blk_queue_discard(q))
			fat_msg(sb, KERN_WARNING,
					"mounting with \"discard\" option, but "
					"the device does not support discard");
	}

	fat_set_state(sb, 1, 0);

	sbi->count = 0;
	sbi->time = 0;
	sbi->i_ino = -1;
	sbi->file_name = NULL;

	return 0;

out_invalid:
	error = -EINVAL;
	if (!silent)
		fat_msg(sb, KERN_INFO, "Can't find a valid FAT filesystem");

out_fail:
	if (fsinfo_inode)
		iput(fsinfo_inode);
	if (fat_inode)
		iput(fat_inode);
	unload_nls(sbi->nls_io);
	unload_nls(sbi->nls_disk);
	if (sbi->options.iocharset != fat_default_iocharset)
		kfree(sbi->options.iocharset);
	sb->s_fs_info = NULL;
	kfree(sbi);
	return error;
}

EXPORT_SYMBOL_GPL(fat_fill_super);

/*
 * helper function for fat_flush_inodes.  This writes both the inode
 * and the file data blocks, waiting for in flight data blocks before
 * the start of the call.  It does not wait for any io started
 * during the call
 */
static int writeback_inode(struct inode *inode)
{

	int ret;

	/* if we used wait=1, sync_inode_metadata waits for the io for the
	* inode to finish.  So wait=0 is sent down to sync_inode_metadata
	* and filemap_fdatawrite is used for the data blocks
	*/
	ret = sync_inode_metadata(inode, 0);
	if (!ret)
		ret = filemap_fdatawrite(inode->i_mapping);
	return ret;
}

/*
 * write data and metadata corresponding to i1 and i2.  The io is
 * started but we do not wait for any of it to finish.
 *
 * filemap_flush is used for the block device, so if there is a dirty
 * page for a block already in flight, we will not wait and start the
 * io over again
 */
int fat_flush_inodes(struct super_block *sb, struct inode *i1, struct inode *i2)
{
	int ret = 0;
	if (!MSDOS_SB(sb)->options.flush)
		return 0;
	if (i1)
		ret = writeback_inode(i1);
	if (!ret && i2)
		ret = writeback_inode(i2);
	if (!ret) {
		struct address_space *mapping = sb->s_bdev->bd_inode->i_mapping;
		ret = filemap_flush(mapping);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(fat_flush_inodes);

static int __init init_fat_fs(void)
{
	int err;

	err = fat_cache_init();
	if (err)
		return err;

	err = fat_init_inodecache();
	if (err)
		goto failed;

	return 0;

failed:
	fat_cache_destroy();
	return err;
}

static void __exit exit_fat_fs(void)
{
	fat_cache_destroy();
	fat_destroy_inodecache();
}

module_init(init_fat_fs)
module_exit(exit_fat_fs)

MODULE_LICENSE("GPL");
