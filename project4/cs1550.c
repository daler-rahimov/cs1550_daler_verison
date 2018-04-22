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

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

//The attribute packed means to not align these things
struct cs1550_directory_entry {
	int nFiles;	//How many files are in this directory.
				//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory {
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nStartBlock;				//where the first block is on disk
	}__attribute__((packed)) files[MAX_FILES_IN_DIR];//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE
			- MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory)
			- sizeof(int)];
};

typedef struct cs1550_root_directory cs1550_root_directory;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct cs1550_root_directory {
	int nDirectories;	//How many subdirectories are in the root
						//Needs to be less than MAX_DIRS_IN_ROOT
	struct cs1550_directory {
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	}__attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory)
			- sizeof(int)];
};

typedef struct cs1550_directory_entry cs1550_directory_entry;

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE - sizeof(long))

struct cs1550_disk_block {
	//The next disk block, if needed. This is the next pointer in the linked 
	//allocation list
	long nNextBlock;

	//And all the rest of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct cs1550_disk_block cs1550_disk_block;
static int getFileSize(const char *path);
/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not. 
 *
 * man -s 2 stat will show the fields of a stat structure
 */
static int cs1550_getattr(const char *path, struct stat *stbuf) {
	int res = 0;
	memset(stbuf, 0, sizeof(struct stat));

	//is path the root dir?
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else {
		int numOfSlashs = 0;
		if (path[0] == '/') {	//if first char is '/'
			int i = 0;
			while (path[i] != '\0') {
				if (path[i] == '/') {
					numOfSlashs++;
				}
				i++;
			}
		}

		//Check if name is subdirectory
		if (numOfSlashs == 2) {
			//Might want to return a structure with these fields
			stbuf->st_mode = S_IFDIR | 0755;
			stbuf->st_nlink = 2;
			res = 0; //no error
		}

		//Check if name is a regular file
		else if (numOfSlashs == 3) {
			//regular file, probably want to be read and write
			int fileSize = getFileSize(path);
			stbuf->st_mode = S_IFREG | 0666;
			stbuf->st_nlink = 1; //file links
			stbuf->st_size = fileSize; //file size - make sure you replace with real size!
			res = 0; // no error
		} else {
			//Else return that path doesn't exist
			res = -ENOENT;
		}
	}
	return res;
}
/*
 * Gets the size of the file from a given path to the file
 */
static int getFileSize(const char *path) {
	int sizeOfFile = -1;
	char directory[MAX_FILENAME + 1];
	char filename[MAX_FILENAME + 1];
	char extension[MAX_EXTENSION + 1];
	cs1550_root_directory root_directory;
	cs1550_directory_entry directory_entry;
	FILE *fp;
	fp = fopen(".disk", "rb");
	if (!fp) {
		printf("Unable to open file! --getFileSize");
		return 1;
	}
	fread(&root_directory, sizeof(cs1550_root_directory), 1, fp);

	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);
	int i;
	for (i = 0; i < root_directory.nDirectories; i++) {
		if (strcmp(root_directory.directories[i].dname, directory)) { //if directory found
			fseek(fp, root_directory.directories[i].nStartBlock, SEEK_SET);
			fread(&directory_entry, sizeof(cs1550_directory_entry), 1, fp);
			break;
		}
	}
	for (i = 0; i < directory_entry.nFiles; i++) {
		if (strcmp(directory_entry.files[i].fname, filename)) {
			sizeOfFile = directory_entry.files[i].fsize;
			break;
		}
	}
	return sizeOfFile;
}

/* 
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi) {
	//Since we're building with -Wall (all warnings reported) we need
	//to "use" every parameter, so let's just cast them to void to
	//satisfy the compiler
	(void) offset;
	(void) fi;

	//This line assumes we have no subdirectories, need to change
	if (strcmp(path, "/") != 0)
		return -ENOENT;

	//the filler function allows us to add entries to the listing
	//read the fuse.h file for a description (in the ../include dir)
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	/*
	 //add the user stuff (subdirs or files)
	 //the +1 skips the leading '/' on the filenames
	 filler(buf, newpath + 1, NULL, 0);
	 */
	return 0;
}

/* 
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int cs1550_mkdir(const char *path, mode_t mode) {
	(void) path;
	(void) mode;
	int numOfDirs = 0;

	char directory_name[MAX_FILENAME + 1];
	char filename[MAX_FILENAME + 1];
	char extension[MAX_EXTENSION + 1];
	sscanf(path, "/%[^/]/%[^.].%s", directory_name, filename, extension);

	// read the root directory
	cs1550_root_directory root_directory;
	FILE *fp;
	fp = fopen(".disk", "rb");
	if (!fp) {
		printf("Unable to open file! --getFileSize");
		return 1;
	}
	fread(&root_directory, sizeof(cs1550_root_directory), 1, fp);

	numOfDirs = root_directory.nDirectories;
	if (numOfDirs >= MAX_DIRS_IN_ROOT) {
		printf("No more directories can be created! --cs1550_mkdir");
		return 1;
	} else if (numOfDirs == 0) {	// no directories has been created yet
		strcpy(root_directory.directories[0].dname, directory_name);
		root_directory.directories[0].nStartBlock =BLOCK_SIZE;
		root_directory.nDirectories++;
	} else {	// there are some directories in the root
		strcpy(root_directory.directories[numOfDirs].dname, directory_name);
		root_directory.directories[numOfDirs].nStartBlock =BLOCK_SIZE * (numOfDirs+1);
		root_directory.nDirectories++;
	}

	fp = fopen(".disk", "wb");
	if (!fp) {
		printf("Unable to open file! --cs1550_mkdir");
		return 1;
	}

	fwrite(&root_directory, sizeof(cs1550_root_directory), 1, fp);
	return 0;
}

/* 
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path) {
	(void) path;
	return 0;
}

/* 
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 */
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev) {
	(void) mode;
	(void) dev;
	(void) path;
	return 0;
}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path) {
	(void) path;

	return 0;
}

/* 
 * Read size bytes from file into buf starting from offset
 *
 */
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi) {
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
		off_t offset, struct fuse_file_info *fi) {
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
static int cs1550_truncate(const char *path, off_t size) {
	(void) path;
	(void) size;

	return 0;
}

/* 
 * Called when we open a file
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi) {
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
static int cs1550_flush(const char *path, struct fuse_file_info *fi) {
	(void) path;
	(void) fi;

	return 0; //success!
}

//register our new functions as the implementations of the syscalls
static struct fuse_operations hello_oper = { .getattr = cs1550_getattr,
		.readdir = cs1550_readdir, .mkdir = cs1550_mkdir, .rmdir = cs1550_rmdir,
		.read = cs1550_read, .write = cs1550_write, .mknod = cs1550_mknod,
		.unlink = cs1550_unlink, .truncate = cs1550_truncate, .flush =
				cs1550_flush, .open = cs1550_open, };

//Don't change this.
int main(int argc, char *argv[]) {
	return fuse_main(argc, argv, &hello_oper, NULL);
}
