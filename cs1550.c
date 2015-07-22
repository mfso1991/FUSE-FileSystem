/*
	FUSE: Filesystem in Userspace
	Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.
*/

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

//size of a disk block
#define	BLOCK_SIZE 512
#define BITMAP_LAST (5*BLOCK_SIZE - 1)
//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

/**
 *  Bitmap will contain 5 * BLOCK_SIZE bytes.
 *  Each bit of each byte represents a slot in disk with BLOCK_SIZE.
 */
struct cs1550_bitmap
{
    unsigned char bitmap[5*BLOCK_SIZE];
};

//The attribute packed means to not align these things
struct cs1550_directory_entry
{
	int nFiles;	//How many files are in this directory.
				//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nStartBlock;				//where the first block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
} ;

typedef struct cs1550_root_directory cs1550_root_directory;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct cs1550_root_directory
{
	int nDirectories;	//How many subdirectories are in the root
						//Needs to be less than MAX_DIRS_IN_ROOT
	struct cs1550_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
} ;


typedef struct cs1550_directory_entry cs1550_directory_entry;

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE)

struct cs1550_disk_block
{
	//All of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct cs1550_disk_block cs1550_disk_block;


/**
 *  Potentially, the allocated space for any one of the dname, fname, or fext contains junks.
 *  If directory, file, or extension is not specified, it might cause problem.
 */
static void parsing(const char* path, char* dname, char* fname, char* fext)
{
    memset(dname, 0, MAX_FILENAME + 1);
    memset(fname, 0, MAX_FILENAME + 1);
    memset(fext, 0, MAX_EXTENSION + 1);
    sscanf(path, "/%[^/]/%[^.].%s", dname, fname, fext);
}

/**
 *  The root directory occupies the first block of the .disk file.
 */
static long disk_offset_of_dir(FILE* fd, char* dname)
{
    cs1550_root_directory root;
    if(fread(&root, 1, BLOCK_SIZE, fd) == BLOCK_SIZE)
    {
        int n_dirs = root.nDirectories, i = 0;
        printf("%d directories on disk.\n", n_dirs);
        for(; i < n_dirs; i++)
            if(strcmp(dname, root.directories[i].dname) == 0)
               return root.directories[i].nStartBlock;
    }
    return 0;
}

static long disk_offset_of_file_ext(cs1550_directory_entry* subdir, char* fname, char* fext, int* index)
{
    int n_files = subdir->nFiles;
    for(; *index < n_files; (*index)++)
        if(strcmp((subdir->files[*index]).fname, fname) == 0 && strcmp((subdir->files[*index]).fext, fext) == 0)
            return (subdir->files[*index]).nStartBlock;
    return 0;
}

/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not. 
 *
 * man -s 2 stat will show the fields of a stat structure
 */
static int cs1550_getattr(const char *path, struct stat *stbuf)
{
    int ret = -ENOENT;
	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0)     //check to see if it is the root directory.
    {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
        ret = 0;
	} 
    else 
    {
        char dname[MAX_FILENAME + 1];   
        char fname[MAX_FILENAME + 1];   
        char fext[MAX_EXTENSION + 1]; 
        parsing(path, dname, fname, fext);
        
    /** invalid case 1 : directory not specified. (file extension and file name does not have to be specified in some cases.) 
        invalid case 2 : directory || file || extension too long.   */
        if(dname[0] && (dname[MAX_FILENAME] == '\0') && (fname[MAX_FILENAME] == '\0') && (fext[MAX_EXTENSION] == '\0')) 
        {
            FILE* fd = fopen(".disk", "rb");
            if(fd) /** NULL is returned if failed to open. **/  
            {
                long dir_pos = disk_offset_of_dir(fd, dname);
                if(dir_pos)    /** 0 is returned if directory not found. **/  
                {
                    if(fname[0] == '\0')   /** only directory name specified. **/
                    {
                		stbuf->st_mode = S_IFDIR | 0755;
                        stbuf->st_nlink = 2;
                        ret = 0;
                    }
                    else
                        if(fseek(fd, BLOCK_SIZE * dir_pos, SEEK_SET) == 0)  
                        {
                            cs1550_directory_entry subdir;
                            if(fread(&subdir, 1, BLOCK_SIZE, fd) == BLOCK_SIZE)
                            {
                                int index = 0;
                                long file_ext_pos = disk_offset_of_file_ext(&subdir, fname, fext, &index);
                                if(file_ext_pos)    /** 0 is returned if file.ext not found. **/  
                                {
                                    stbuf->st_mode = S_IFREG | 0666; 
                                    stbuf->st_nlink = 1;
                                    stbuf->st_size = subdir.files[index].fsize;
                                    ret = 0;
                                }
                            }
                        }
                }   
                else printf("~~~directory not found.\n");
                fclose(fd);
            }
        }
    }
	return ret;
}

/* 
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	//Since we're building with -Wall (all warnings reported) we need
	//to "use" every parameter, so let's just cast them to void to
	//satisfy the compiler
	(void) offset;
	(void) fi;
  
    int ret = -ENOENT;
    filler(buf, ".", NULL,0);
	filler(buf, "..", NULL, 0);
	
    if(strcmp(path, "/") != 0)     /** TRUE if sub-directory **/
    {
        char dname[MAX_FILENAME + 1];   
        char fname[MAX_FILENAME + 1];   
        char fext[MAX_EXTENSION + 1]; 
        parsing(path, dname, fname, fext);
        if(dname[0] && dname[MAX_FILENAME] == '\0' && fname[MAX_FILENAME] == '\0' && fext[MAX_EXTENSION] == '\0')
        {
            FILE* fd = fopen(".disk", "rb");
            if(fd)
            {
                long dir_pos = disk_offset_of_dir(fd, dname);
                if(dir_pos)    /** 0 is returned if directory not found. **/  
                    if(fseek(fd, BLOCK_SIZE * dir_pos, SEEK_SET) == 0)  
                    {
                         cs1550_directory_entry subdir;
                         if(fread(&subdir, 1, BLOCK_SIZE, fd) == BLOCK_SIZE)
                         {
                            int n_files = subdir.nFiles, i = 0;
                            for(; i < n_files; i++)
                            {
                                char file_ext[MAX_FILENAME + MAX_EXTENSION + 2];
                                strcpy(file_ext, subdir.files[i].fname);
                                if(subdir.files[i].fext[0])
                                {
                                    strcat(file_ext, ".");
                                    strcat(file_ext, subdir.files[i].fext);
                                }
                                filler(buf, file_ext, NULL, 0);
                            }
                            ret = 0;
                         }
                    }
            }
            fclose(fd);
        }
    }
    else    /** ROOT **/
    {
        cs1550_root_directory root;
        FILE* fd = fopen(".disk", "rb");
        if(fd && (fread(&root, 1, BLOCK_SIZE, fd) == BLOCK_SIZE))
        {
            int n_dirs = root.nDirectories, i = 0;
            for(; i < n_dirs; i++)
                filler(buf, root.directories[i].dname, NULL, 0);
            ret = 0;
        }
        fclose(fd);
    }
	return ret;
}

static long find_free_block(FILE* fd, unsigned char* bitmap)
{
    int ret = 0, i = 1;
    for(; i < BITMAP_LAST; i++)
        if(bitmap[i] < 255) //meaning there is an empty block.
        {
            unsigned char mask = bitmap[i] + 1;
            unsigned char count = 128;
            for(ret = 7; (count ^ mask) != 0; count >>= 1, ret--);
            ret += (i*8);
            bitmap[i] = bitmap[i] | mask;
            printf("location found.\n");
            break;
        }
    if(fwrite(bitmap, 1, 5*BLOCK_SIZE, fd) == BITMAP_LAST + 1)
        printf("bitmap updated.\n");
    return ret;
}

static int disk_add_dir(FILE* fd, char* dname)
{
    int ret = -1;
    if(fseek(fd, -5*BLOCK_SIZE, SEEK_END) == 0)
    {
        struct cs1550_bitmap _bitmap;
        if(fread(&_bitmap, 1, 5*BLOCK_SIZE, fd) == (BITMAP_LAST+1))
        {
            long _nStartBlock = find_free_block(fd, _bitmap.bitmap);
            if(_nStartBlock)  /** 0 is returned if can't find one. **/
            {
                printf("bitmap returned. The starting block is %ld\n", _nStartBlock);
                if(fseek(fd, 0, SEEK_SET) == 0)
                {
                    cs1550_root_directory root;
                    if(fread(&root, 1, BLOCK_SIZE, fd) == BLOCK_SIZE)
                    {
                        stpcpy(root.directories[root.nDirectories].dname, dname);
                        root.directories[root.nDirectories].nStartBlock = _nStartBlock;
                        root.nDirectories += 1;
                        printf("now %d directories on disk.\n", root.nDirectories);
                        if(fseek(fd, 0, SEEK_SET) == 0)
                            if(fwrite(&root, 1, BLOCK_SIZE, fd) == BLOCK_SIZE)
                            {
                                printf("root updated.\n");
                                cs1550_directory_entry new_dir;
                                if(fseek(fd, _nStartBlock*BLOCK_SIZE, SEEK_SET) == 0)
                                    if(fwrite(&new_dir, 1, BLOCK_SIZE, fd) == BLOCK_SIZE)
                                    {
                                        printf("new dir written.\n");
                                        ret = 0;
                                    }
                            }
                    }
                }
            }
        }
    }
    return ret;
} 

/* 
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int cs1550_mkdir(const char *path, mode_t mode)
{
	(void) path;
	(void) mode;
    
    int ret = 0;
    char dname[MAX_FILENAME + 1];   
    char fname[MAX_FILENAME + 1];   
    char fext[MAX_EXTENSION + 1]; 
	parsing(path, dname, fname, fext);
    if(dname[MAX_FILENAME])
    {
        printf("too long!\n");
        ret = -ENAMETOOLONG;
    }
    else if(fname[0])   /** if contains second level directory**/
    {
        printf("second level!\n");
        ret = -EPERM;
    }
    else if(dname[0] && (dname[MAX_FILENAME] == '\0') && (fname[0] == '\0'))
    {
        printf("valid dir_name\n");
        FILE* fd = fopen(".disk", "rb+");
        if(fd)
        {
            if(disk_offset_of_dir(fd, dname))   /** directory already existed. **/
            {
                printf("already existed\n");
                ret = -EEXIST;
            }
            else    /** return non-zero value if failed. **/
                ret = disk_add_dir(fd, dname); 
        }
        fclose(fd);
    }

    return ret;
}

/* 
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path)
{
	(void) path;
    return 0;
}

/* 
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 */
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev)
{
	(void) mode;
	(void) dev;
	return 0;
}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path)
{
    (void) path;

    return 0;
}

/* 
 * Read size bytes from file into buf starting from offset
 *
 */
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//read in data
	//set size and return, or error

	size = 0;

	return size;
}

/* 
 * Write size bytes from buf into file starting from offset
 *
 */
static int cs1550_write(const char *path, const char *buf, size_t size, 
			  off_t offset, struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//write data
	//set size (should be same as input) and return, or error

	return size;
}

/******************************************************************************
 *
 *  DO NOT MODIFY ANYTHING BELOW THIS LINE
 *
 *****************************************************************************/

/*
 * truncate is called when a new file is created (with a 0 size) or when an
 * existing file is made shorter. We're not handling deleting files or 
 * truncating existing ones, so all we need to do here is to initialize
 * the appropriate directory entry.
 *
 */
static int cs1550_truncate(const char *path, off_t size)
{
	(void) path;
	(void) size;

    return 0;
}


/* 
 * Called when we open a file
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;
    /*
        //if we can't find the desired file, return an error
        return -ENOENT;
    */

    //It's not really necessary for this project to anything in open

    /* We're not going to worry about permissions for this project, but 
	   if we were and we don't have them to the file we should return an error

        return -EACCES;
    */

    return 0; //success!
}

/*
 * Called when close is called on a file descriptor, but because it might
 * have been dup'ed, this isn't a guarantee we won't ever need the file 
 * again. For us, return success simply to avoid the unimplemented error
 * in the debug log.
 */
static int cs1550_flush (const char *path , struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;

	return 0; //success!
}


//register our new functions as the implementations of the syscalls
static struct fuse_operations hello_oper = {
    .getattr	= cs1550_getattr,
    .readdir	= cs1550_readdir,
    .mkdir	= cs1550_mkdir,
	.rmdir = cs1550_rmdir,
    .read	= cs1550_read,
    .write	= cs1550_write,
	.mknod	= cs1550_mknod,
	.unlink = cs1550_unlink,
	.truncate = cs1550_truncate,
	.flush = cs1550_flush,
	.open	= cs1550_open,
};

//Don't change this.
int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &hello_oper, NULL);
}
