#include <linux/module.h>
#include <linux/init.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/pagemap.h>
#include <linux/mpage.h>
#include <linux/buffer_head.h>
#include <linux/exportfs.h>
#include <linux/mount.h>
#include <linux/vfs.h>
#include <linux/parser.h>
#include <linux/uio.h>
#include <linux/writeback.h>
#include <linux/log2.h>
#include <linux/hash.h>
#include <asm/unaligned.h>
#include "fat.h"

const char *config_data = "Blackbox Configuration\r\n\r\nPartitioning Size[Percentage]\r\n\tOPEL_CONFIG_NORMAL       =20\r\n\tOPEL_CONFIG_EVENT    =15\r\n\tOPEL_CONFIG_PARKING   =15\r\n\tOPEL_CONFIG_MANUAL       =15\r\n\tOPEL_CONFIG_IMAGE        =15\r\n\tOPEL_CONFIG_ETC          =20\r\n\r\nPreallocation Setting[MB]\r\n\tOPEL_CONFIG_NORMAL      =88\r\n\tOPEL_CONFIG_EVENT    =56\r\n\tOPEL_CONFIG_PARKING   =40\r\n\tOPEL_CONFIG_MANUAL      =24\r\n\t\0";

unsigned char msdos_name[ MSDOS_NAME ] = "BXFS_CON";

static int opel_read_config_data(struct super_block *sb, char *data)
{
	/*
	   Read Data from config file.
	   Need to change more smart
	 */
	struct msdos_sb_info *sbi = MSDOS_SB(sb);

	int num_of_pre = 4;
	int num_of_par = 6;
	int num_of_total = num_of_pre + num_of_par;

	int i;
	int block_size = 512;
	int count = 0;

	int read_val[20] = {0, };

	for(i=0; i<block_size; i++)
	{
		if(data[i] == '='){
			if(data[i+3] >= '0' && data[i+3] <= '9'){//Case1 : 3 digit number
				read_val[count] = (((int)(data[i+1])-48) * 100) +  (((int)(data[i+2])-48) * 10) +  (((int)(data[i+3])-48)) ;
				i+=3;
			}
			else if(data[i+2] >= '0' && data[i+2] <= '9'){ //Case 2 : 2 digit number
				read_val[count] = (((int)(data[i+1])-48) * 10) +  (((int)(data[i+2])-48)) ;
				i+=2;
			}
			else if(data[i+1] >= '0' && data[i+1] <= '9'){ //Case 3 : 1 digit number
				read_val[count] = (((int)(data[i+1])-48)) ;
				i++;
			}
			else{
				printk("[opel_fat] Invalid config file data! \n");
				return -1;
			}

			count++;

			if(count >= num_of_total){
				printk("[opel_fat] Complete data parsing \n");
				break;
			}
		}
	}
	if(count < num_of_total){
		printk("[opel_fat] Some config date are dropped! \n");
		return -1;
	}

	sbi->opel_area_ratio[ OPEL_BLACKBOX_NORMAL		 ]    = read_val[0];
	sbi->opel_area_ratio[ OPEL_BLACKBOX_EVENT  ]    = read_val[1];
	sbi->opel_area_ratio[ OPEL_BLACKBOX_PARKING 		 ]    = read_val[2];
	sbi->opel_area_ratio[ OPEL_BLACKBOX_MANUAL 		 ]    = read_val[3];
	sbi->opel_area_ratio[ OPEL_BLACKBOX_CONFIG         ]    = read_val[4];
	sbi->opel_area_ratio[ OPEL_BLACKBOX_ETC 			 ] 	  = read_val[5];

	printk("[opel_fat] %u \n", sbi->opel_area_ratio[ OPEL_BLACKBOX_NORMAL ] );
	printk("[opel_fat] %u \n", sbi->opel_area_ratio[ OPEL_BLACKBOX_EVENT ] );
	printk("[opel_fat] %u \n", sbi->opel_area_ratio[ OPEL_BLACKBOX_PARKING ] );
	printk("[opel_fat] %u \n", sbi->opel_area_ratio[ OPEL_BLACKBOX_MANUAL ] );
	printk("[opel_fat] %u \n", sbi->opel_area_ratio[ OPEL_BLACKBOX_CONFIG ] );
	printk("[opel_fat] %u \n\n", sbi->opel_area_ratio[ OPEL_BLACKBOX_ETC ] );

	sbi->opel_pre_size[ OPEL_BLACKBOX_NORMAL ]    	  = read_val[6]; //normal
	sbi->opel_pre_size[ OPEL_BLACKBOX_EVENT ]   = read_val[7]; //event
	sbi->opel_pre_size[ OPEL_BLACKBOX_PARKING ]		  = read_val[8]; //parking
	sbi->opel_pre_size[ OPEL_BLACKBOX_MANUAL ]  		  = read_val[9]; //manual
	sbi->opel_pre_size[ OPEL_BLACKBOX_ETC ] = sbi->opel_pre_size[ OPEL_BLACKBOX_CONFIG ] = sbi->opel_pre_size[ OPEL_BLACKBOX_MANUAL ]; //ETC/HANDWORK

	printk("[opel_fat] %u \n", sbi->opel_pre_size[ OPEL_BLACKBOX_NORMAL ] );
    printk("[opel_fat] %u \n", sbi->opel_pre_size[ OPEL_BLACKBOX_EVENT ] );
    printk("[opel_fat] %u \n", sbi->opel_pre_size[ OPEL_BLACKBOX_PARKING ] );
	printk("[opel_fat] %u \n",	sbi->opel_pre_size[ OPEL_BLACKBOX_MANUAL ] );

	return 0;
}

static int _opel_fat_chain_add(struct inode *inode, int new_dclus, int nr_cluster)
{

	struct super_block *sb = inode->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	int ret, new_fclus, last;

	/*
	 * We must locate the last cluster of the file to add this new
	 * one (new_dclus) to the end of the link list (the FAT).
	 */
	last = new_fclus = 0;

	/* add new one to the last of the cluster chain */

	MSDOS_I(inode)->i_start = new_dclus;
	MSDOS_I(inode)->i_logstart = new_dclus;

	/*
	 * Since generic_write_sync() synchronizes regular files later,
	 * we sync here only directories.
	 */

	//sync wirte for fat scan
	ret = fat_sync_inode(inode);
	if (ret)
		return ret;

	inode->i_blocks = nr_cluster << (sbi->cluster_bits - 9);

	return 0;
}

int opel_build_config_file( struct inode *dir, struct dentry *dentry, int mode )
{
	struct msdos_dir_entry de;
	struct fat_slot_info sinfo;
	struct fat_slot_info *_sinfo = &sinfo;
	struct inode *inode = NULL;

	struct super_block *sb = dir->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB( sb );

	int cluster_num = 0;
	int err;
	int cluster = sbi->fat_start;

	//First, Create empty file
	memcpy( de.name, msdos_name, MSDOS_NAME ); //msdos_dir_entry의 name에 msdos_name배열 안에 이름을

	de.attr = 0 ? ATTR_DIR : ATTR_ARCH;
	de.lcase = 0;
	de.cdate = de.adate = 0;
	de.ctime = 0;
	de.ctime_cs = 0;
	de.time = 0;
	de.date = 0;
	de.start = cpu_to_le16(cluster_num);
	de.starthi = cpu_to_le16(cluster_num) >> 16;  //high 16bit니깐 이게 맞는거 같은데 원래 괄호안에 >> 16이 있었음
	de.size = 512; //length of data

	err = fat_add_entries( dir, &de, 1, _sinfo );
	inode = fat_build_inode( sb, sinfo.de, sinfo.i_pos );

	brelse( sinfo.bh );

	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out;
	}
	printk("[opel_fat] Complete Create Config File \n");

	// Second Cluster allocation
	if (!err){
		err = fat_flush_inodes(sb, dir, inode);
		//alloc speific area
		opel_fat_alloc_cluster(inode, &cluster, OPEL_CONFIG_ETC);
		_opel_fat_chain_add( inode, cluster, 1 );
	}
	else
		printk("[opel_fat] Error or Skipping \n");

out:
	return err;
}

static int opel_vfat_find_form( struct msdos_sb_info *sbi, struct inode *dir, unsigned char *name, sector_t *blknr )
{
	struct fat_slot_info sinfo;
	int err = opel_fat_scan(dir, name, &sinfo);
	if (err)
		    return -ENOENT;
	else
		printk("[opel_fat] detect \n");

	sector_t clus = sinfo.de->start;

	brelse(sinfo.bh);
	printk("[opel_fat] sinfo's start info : %u \n", clus );

 	*blknr = ( (clus - FAT_START_ENT) * sbi->sec_per_clus ) + sbi->data_start;
	return 0;
}

/*
 	Update for pre-allocation config file setting
 */
int opel_fat_config_init(struct super_block *sb)
{
	struct fat_slot_info sinfo;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	struct dentry *root_de = sb->s_root;
	struct inode *root = root_de->d_inode;

	struct buffer_head *bh;
	unsigned int block_pos;
	int err;

	sector_t blknr;

	if( !opel_fat_scan( root, msdos_name, &sinfo ) )
	{
		brelse( sinfo.bh );
		goto exist;
	}

	//First - Create file
	err = opel_build_config_file( root, root_de, 0 );

    //Second - Data Input
	if( opel_vfat_find_form( sbi, root, msdos_name, &blknr ) == -ENOENT )
		printk("[opel_fat] opel_vfat_find_form error \n");

	bh = sb_bread( sb, blknr );
	if( !bh )
	{
		brelse(bh);
	}

	strcpy( bh->b_data, config_data );

	mark_buffer_dirty(bh);
	err = sync_dirty_buffer(bh);
	brelse(bh);

	if (fat_scan(root, msdos_name, &sinfo))
	{
		return -1;
	}

exist:
	if( opel_vfat_find_form( sbi, root, msdos_name, &blknr ) == -ENOENT )
		printk("[opel_fat] opel_vfat_find_form error \n");

	bh = sb_bread( sb, blknr );
	opel_read_config_data( sb, bh->b_data );
	brelse( bh );

	return 0;
}
EXPORT_SYMBOL_GPL( opel_fat_config_init );
