// vim: noet:ts=8:sts=8
#define FUSE_USE_VERSION 26
#define _GNU_SOURCE

#include <sys/mman.h>
#include <assert.h>
#include <endian.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vfat.h"

#define DEBUG_PRINT printf

// A kitchen sink for all important data about filesystem
struct vfat_data {
	const char	*dev;
	int		fs;
	struct fat_boot  fat_boot;
	/* XXX add your code here */
};

struct vfat_data vfat_info;
iconv_t iconv_utf16;

uid_t mount_uid;
gid_t mount_gid;
time_t mount_time;


static void
vfat_init(const char *dev)
{
	uint16_t rootDirSectors;
	uint32_t fatSz,totSec,dataSec,countofClusters;
	iconv_utf16 = iconv_open("utf-8", "utf-16"); // from utf-16 to utf-8
	// These are useful so that we can setup correct permissions in the mounted directories
	mount_uid = getuid();
	mount_gid = getgid();

	// Use mount time as mtime and ctime for the filesystem root entry (e.g. "/")
	mount_time = time(NULL);

	vfat_info.fs = open(dev, O_RDONLY);
	if (vfat_info.fs < 0)
		err(1, "open(%s)", dev);

	if(read(vfat_info.fs,&vfat_info.fat_boot,512) != 512){
		err(1,"read(%s)",dev);
	}
	printf("Bytes per sector:%d \n", vfat_info.fat_boot.bytes_per_sector);

	//FAT TYPE DETERMINATION:
	if(vfat_info.fat_boot.root_max_entries != 0){
		err(1,"error: should be 0\n");
	}
	rootDirSectors = ((vfat_info.fat_boot.root_max_entries * 32) + (vfat_info.fat_boot.bytes_per_sector - 1)) / vfat_info.fat_boot.bytes_per_sector;
	
	if(vfat_info.fat_boot.sectors_per_fat_small != 0){
		fatSz = vfat_info.fat_boot.sectors_per_fat_small;
	} else{
		fatSz = vfat_info.fat_boot.fat32.sectors_per_fat;
	}
	if(vfat_info.fat_boot.total_sectors_small != 0){
		totSec = vfat_info.fat_boot.total_sectors_small;
	} else {
		totSec = vfat_info.fat_boot.total_sectors;
	}
	
        dataSec = totSec - (vfat_info.fat_boot.reserved_sectors + (vfat_info.fat_boot.fat_count * fatSz) + rootDirSectors);
	countofClusters = dataSec / vfat_info.fat_boot.sectors_per_cluster;

	if(countofClusters < 4085) {
		err(1,"error: Volume is FAT12.\n");
	} else if(countofClusters < 65525) {
    		err(1,"error: Volume is FAT16.\n");
	} else {
    		printf("Volume is FAT32.\n");
	}
}

/* XXX add your code here */

unsigned char
chkSum (unsigned char *pFcbName)
	{
		short fcbNameLen;
		unsigned char sum;

		sum = 0;
		for (fcbNameLen=11; fcbNameLen!=0; fcbNameLen--) {
			// NOTE: The operation is an unsigned char rotate right
			sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *pFcbName++;
		}
		return (sum);
	}


static int
test_read(void) {
	unsigned char check_sum = NULL;
	int i, j, has_long_name = 0;
	uint32_t fatSz, first_data_sec;
	struct fat32_direntry short_entry;
	struct fat32_direntry_long long_entry;

        fatSz = vfat_info.fat_boot.fat32.sectors_per_fat;
	
	first_data_sec = vfat_info.fat_boot.reserved_sectors + fatSz * vfat_info.fat_boot.fat_count;
	
	if(lseek(vfat_info.fs, first_data_sec*vfat_info.fat_boot.bytes_per_sector, SEEK_CUR) == -1) {
		err(1, "lseek(%d)", first_data_sec*vfat_info.fat_boot.bytes_per_sector);
	}
	
	for(i = 0; i < vfat_info.fat_boot.sectors_per_cluster*vfat_info.fat_boot.bytes_per_sector; i+=32) {
		if(read(vfat_info.fs, &short_entry, 32) != 32){
			err(1, "read(short_dir)");
		}
		
		if((0x0F & short_entry.attr) == 0x0F) {
			has_long_name = 1;
			continue;			
		} else if(!has_long_name){
			if(short_entry.nameext[0] == 0xE5) {
				continue;
			} else if(short_entry.nameext[0] == 0x00) {
				break;
			} else if(short_entry.nameext[0] == 0x05) {
				short_entry.name1[0] = 0xE5;
			}
			
			DEBUG_PRINT("name of current dir: ");
			
			for(j = 0; j < 2; j++) {
				DEBUG_PRINT("%c", short_entry.nameext[j]);
			}
			DEBUG_PRINT("\n");
		} else {
			check_sum = chkSum(&(short_entry.nameext));
			
			while(true) {
				i-=64;
				if(lseek(vfat_info.fs, -64, SEEK_CUR) == -1) {
					err(1, "lseek(%d)", -64);
				}
			
				if(read(vfat_info.fs, &long_entry, 32) != 32){
					err(1, "read(long_dir)");
				}
			
				if(long_entry.csum != check_sum) {
					err(1, "");
				} else {
				
					DEBUG_PRINT("name of current dir: ");
					for(j = 0; j < 5 && short_entry.name1[j] != 0xFFFF; j++) {
						DEBUG_PRINT("%c", short_entry.name1[j]);
					}
					for(j = 0; j < 6  && short_entry.name2[j] != 0xFFFF; j++) {
						DEBUG_PRINT("%c", short_entry.name2[j]);
					}
					for(j = 0; j < 3  && short_entry.name3[j] != 0xFFFF; j++) {
						DEBUG_PRINT("%c", short_entry.name3[j]);
					}
					DEBUG_PRINT("\n");
					
					if((0xF0 & long_entry.seq) == 0x40) {
						break;
					}
				}
			}
		}
	}
	
	return 0;
}

static int
vfat_readdir(uint32_t first_cluster, fuse_fill_dir_t filler, void *fillerdata)
{
	struct stat st; // we can reuse same stat entry over and over again
	void *buf = NULL;
	struct vfat_direntry *e;
	char *name;

	memset(&st, 0, sizeof(st));
	st.st_uid = mount_uid;
	st.st_gid = mount_gid;
	st.st_nlink = 1;

	/* XXX add your code here */
}


// Used by vfat_search_entry()
struct vfat_search_data {
	const char	*name;
	int		found;
	struct stat	*st;
};


// You can use this in vfat_resolve as a filler function for vfat_readdir
static int
vfat_search_entry(void *data, const char *name, const struct stat *st, off_t offs)
{
	struct vfat_search_data *sd = data;

	if (strcmp(sd->name, name) != 0)
		return (0);

	sd->found = 1;
	*sd->st = *st;

	return (1);
}

// Recursively find correct file/directory node given the path
static int
vfat_resolve(const char *path, struct stat *st)
{
	struct vfat_search_data sd;

	/* XXX add your code here */
}

// Get file attributes
static int
vfat_fuse_getattr(const char *path, struct stat *st)
{
	/* XXX: This is example code, replace with your own implementation */
	DEBUG_PRINT("fuse getattr %s\n", path);
	// No such file
	if (strcmp(path, "/")==0) {
		st->st_dev = 0; // Ignored by FUSE
		st->st_ino = 0; // Ignored by FUSE unless overridden
		st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFDIR;
		st->st_nlink = 1;
		st->st_uid = mount_uid;
		st->st_gid = mount_gid;
		st->st_rdev = 0;
		st->st_size = 0;
		st->st_blksize = 0; // Ignored by FUSE
		st->st_blocks = 1;
		return 0;
	}
	if (strcmp(path, "/a.txt")==0 || strcmp(path, "/b.txt")==0) {
		st->st_dev = 0; // Ignored by FUSE
		st->st_ino = 0; // Ignored by FUSE unless overridden
		st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFREG;
		st->st_nlink = 1;
		st->st_uid = mount_uid;
		st->st_gid = mount_gid;
		st->st_rdev = 0;
		st->st_size = 10;
		st->st_blksize = 0; // Ignored by FUSE
		st->st_blocks = 1;
		return 0;
	}

	return -ENOENT;
}

static int
vfat_fuse_readdir(const char *path, void *buf,
		  fuse_fill_dir_t filler, off_t offs, struct fuse_file_info *fi)
{
	/* XXX: This is example code, replace with your own implementation */
	DEBUG_PRINT("fuse readdir %s\n", path);
	//assert(offs == 0);
	/* XXX add your code here */
	filler(buf, "a.txt", NULL, 0);
	filler(buf, "b.txt", NULL, 0);
	return 0;
}

static int
vfat_fuse_read(const char *path, char *buf, size_t size, off_t offs,
	       struct fuse_file_info *fi)
{
	/* XXX: This is example code, replace with your own implementation */
	DEBUG_PRINT("fuse read %s\n", path);
	assert(size > 1);
	buf[0] = 'X';
	buf[1] = 'Y';
	/* XXX add your code here */
	return 2; // number of bytes read from the file
		  // must be size unless EOF reached, negative for an error 
}

////////////// No need to modify anything below this point
static int
vfat_opt_args(void *data, const char *arg, int key, struct fuse_args *oargs)
{
	if (key == FUSE_OPT_KEY_NONOPT && !vfat_info.dev) {
		vfat_info.dev = strdup(arg);
		return (0);
	}
	return (1);
}

static struct fuse_operations vfat_available_ops = {
	.getattr = vfat_fuse_getattr,
	.readdir = vfat_fuse_readdir,
	.read = vfat_fuse_read,
};

int
main(int argc, char **argv)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	fuse_opt_parse(&args, NULL, NULL, vfat_opt_args);

	if (!vfat_info.dev)
		errx(1, "missing file system parameter");

	vfat_init(vfat_info.dev);
	test_read();
	return 0;//(fuse_main(args.argc, args.argv, &vfat_available_ops, NULL));
}
