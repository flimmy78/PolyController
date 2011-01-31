/*
 * polyfsck - check a polyfs file system
 *
 * Copyright (C) 2000-2002 Transmeta Corporation
 * Copyright (C) 2011 Chris Boot
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * 1999/12/03: Linus Torvalds (cramfs tester and unarchive program)
 * 2000/06/03: Daniel Quinlan (CRC and length checking program)
 * 2000/06/04: Daniel Quinlan (merged programs, added options, support
 *                            for special files, preserve permissions and
 *                            ownership, cramfs superblock v2, bogus mode
 *                            test, pathname length test, etc.)
 * 2000/06/06: Daniel Quinlan (support for holes, pretty-printing,
 *                            symlink size test)
 * 2000/07/11: Daniel Quinlan (file length tests, start at offset 0 or 512,
 *                            fsck-compatible exit codes)
 * 2000/07/15: Daniel Quinlan (initial support for block devices)
 * 2002/01/10: Daniel Quinlan (additional checks, test more return codes,
 *                            use read if mmap fails, standardize messages)
 * 2002/02/22: Brad Bozarth   (add support for big-endian systems)
 * 2011/01/14: Chris Boot     convert to polyfs
 */

/* compile-time options */
#define INCLUDE_FS_TESTS	/* include polyfs checking and extraction */

#define _GNU_SOURCE
#include <sys/types.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#ifndef __APPLE__
#include <sys/sysmacros.h>
#endif
#include <utime.h>
#include <sys/ioctl.h>
#define _LINUX_STRING_H_
#include "polyfs/polyfs_fs.h"
#include <zlib.h>
#include <lzo/lzo1x.h>

#ifdef __APPLE__
#define MAP_ANONYMOUS MAP_ANON
#endif

#define BLKGETSIZE	_IO(0x12,96) /* return device size /512 (long *arg) */

/* Exit codes used by fsck-type programs */
#define FSCK_OK          0	/* No errors */
#define FSCK_NONDESTRUCT 1	/* File system errors corrected */
#define FSCK_REBOOT      2	/* System should be rebooted */
#define FSCK_UNCORRECTED 4	/* File system errors left uncorrected */
#define FSCK_ERROR       8	/* Operational error */
#define FSCK_USAGE       16	/* Usage or syntax error */
#define FSCK_LIBRARY     128	/* Shared library error */

#define PAD_SIZE 512

static const char *progname = "polyfsck";

static int fd;			/* ROM image file descriptor */
static char *filename;		/* ROM image filename */
struct polyfs_super super;	/* just find the polyfs superblock once */
static int opt_verbose = 0;	/* 1 = verbose (-v), 2+ = very verbose (-vv) */
#ifdef INCLUDE_FS_TESTS
static int opt_extract = 0;		/* extract polyfs (-x) */
static char *extract_dir = "/";	/* extraction directory (-x) */
static uid_t euid;			/* effective UID */

/* (polyfs_super + start) <= start_dir < end_dir <= start_data <= end_data */
static unsigned long start_dir = ~0UL;	/* start of first non-root inode */
static unsigned long end_dir = 0;	/* end of the directory structure */
static unsigned long start_data = ~0UL;	/* start of the data (256 MB = max) */
static unsigned long end_data = 0;	/* end of the data */

/* Guarantee access to at least 8kB at a time */
#define ROMBUFFER_BITS	13
#define ROMBUFFERSIZE	(1 << ROMBUFFER_BITS)
#define ROMBUFFERMASK	(ROMBUFFERSIZE-1)
static char read_buffer[ROMBUFFERSIZE * 2];
static unsigned long read_buffer_block = ~0UL;

/* Uncompressing data structures... */
static char outbuffer[POLYFS_BLOCK_SIZE * 2];
static z_stream stream;

/* Prototypes */
static void expand_fs(char *, struct polyfs_inode *);
#endif /* INCLUDE_FS_TESTS */

/* Input status of 0 to print help and exit without an error. */
static void usage(int status)
{
	FILE *stream = status ? stderr : stdout;

	fprintf(stream, "usage: %s [-hv] [-x dir] file\n"
			" -h         print this help\n"
			" -x dir     extract into dir\n"
			" -v         be more verbose\n"
			" file       file to test\n", progname);

	exit(status);
}

static void die(int status, int syserr, const char *fmt, ...)
{
	va_list arg_ptr;
	int save = errno;

	fflush(0);
	va_start(arg_ptr, fmt);
	fprintf(stderr, "%s: ", progname);
	vfprintf(stderr, fmt, arg_ptr);
	if (syserr) {
		fprintf(stderr, ": %s", strerror(save));
	}
	fprintf(stderr, "\n");
	va_end(arg_ptr);
	exit(status);
}

static void test_super(int *start, size_t *length) {
	struct stat st;

	/* find the physical size of the file or block device */
	if (stat(filename, &st) < 0) {
		die(FSCK_ERROR, 1, "stat failed: %s", filename);
	}
	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		die(FSCK_ERROR, 1, "open failed: %s", filename);
	}
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE, length) < 0) {
			die(FSCK_ERROR, 1, "ioctl failed: unable to determine device size: %s", filename);
		}
		*length = *length * 512;
	}
	else if (S_ISREG(st.st_mode)) {
		*length = st.st_size;
	}
	else {
		die(FSCK_ERROR, 0, "not a block device or file: %s", filename);
	}

	if (*length < sizeof(struct polyfs_super)) {
		die(FSCK_UNCORRECTED, 0, "filesystem smaller than a polyfs superblock!");
	}

	/* find superblock */
	if (read(fd, &super, sizeof(super)) != sizeof(super)) {
		die(FSCK_ERROR, 1, "read failed: %s", filename);
	}
	if (super.magic == POLYFS_32(POLYFS_MAGIC)) {
		*start = 0;
	}
	else if (*length >= (PAD_SIZE + sizeof(super))) {
		lseek(fd, PAD_SIZE, SEEK_SET);
		if (read(fd, &super, sizeof(super)) != sizeof(super)) {
			die(FSCK_ERROR, 1, "read failed: %s", filename);
		}
		if (super.magic == POLYFS_32(POLYFS_MAGIC)) {
			*start = PAD_SIZE;
		}
	}

	/* superblock tests */
	if (super.magic != POLYFS_32(POLYFS_MAGIC)) {
		die(FSCK_UNCORRECTED, 0, "superblock magic not found");
	}
#if __BYTE_ORDER == __BIG_ENDIAN
	super.size = POLYFS_32(super.size);
	super.flags = POLYFS_32(super.flags);
	super.future = POLYFS_32(super.future);
	super.fsid.crc = POLYFS_32(super.fsid.crc);
	super.fsid.edition = POLYFS_32(super.fsid.edition);
	super.fsid.blocks = POLYFS_32(super.fsid.blocks);
	super.fsid.files = POLYFS_32(super.fsid.files);
#endif /* __BYTE_ORDER == __BIG_ENDIAN */
	if (super.flags & ~POLYFS_SUPPORTED_FLAGS) {
		die(FSCK_ERROR, 0, "unsupported filesystem features");
	}
	if (super.size < POLYFS_BLOCK_SIZE) {
		die(FSCK_UNCORRECTED, 0, "superblock size (%d) too small", super.size);
	}
	if (super.flags & POLYFS_FLAG_FSID_VERSION_1) {
		if (super.fsid.files == 0) {
			die(FSCK_UNCORRECTED, 0, "zero file count");
		}
		if (*length < super.size) {
			die(FSCK_UNCORRECTED, 0, "file length too short, %lu is smaller than %lu",
					*length, super.size);
		}
		else if (*length > super.size) {
			fprintf(stderr, "warning: file extends past end of filesystem\n");
		}
	}
	else {
		die(FSCK_UNCORRECTED, 0, "invalid filesystem version");
	}
}

static void test_crc(int start)
{
	void *buf;
	uint32_t crc;

	if (!(super.flags & POLYFS_FLAG_FSID_VERSION_1)) {
#ifdef INCLUDE_FS_TESTS
		return;
#else /* not INCLUDE_FS_TESTS */
		die(FSCK_USAGE, 0, "unable to test CRC: invalid filesystem version");
#endif /* not INCLUDE_FS_TESTS */
	}

	crc = crc32(0L, Z_NULL, 0);

	buf = mmap(NULL, super.size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (buf == MAP_FAILED) {
		buf = mmap(NULL, super.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (buf != MAP_FAILED) {
			lseek(fd, 0, SEEK_SET);
			read(fd, buf, super.size);
		}
	}
	if (buf != MAP_FAILED) {
		((struct polyfs_super *) (buf+start))->fsid.crc = crc32(0L, Z_NULL, 0);
		crc = crc32(crc, buf+start, super.size-start);
		munmap(buf, super.size);
	}
	else {
		int retval;
		size_t length = 0;

		buf = malloc(4096);
		if (!buf) {
			die(FSCK_ERROR, 1, "malloc failed");
		}
		lseek(fd, start, SEEK_SET);
		for (;;) {
			retval = read(fd, buf, 4096);
			if (retval < 0) {
				die(FSCK_ERROR, 1, "read failed: %s", filename);
			}
			else if (retval == 0) {
				break;
			}
			if (length == 0) {
				((struct polyfs_super *) buf)->fsid.crc = crc32(0L, Z_NULL, 0);
			}
			length += retval;
			if (length > (super.size-start)) {
				crc = crc32(crc, buf, retval - (length - (super.size-start)));
				break;
			}
			crc = crc32(crc, buf, retval);
		}
		free(buf);
	}

	if (crc != super.fsid.crc) {
		die(FSCK_UNCORRECTED, 0, "crc error");
	}
}

#ifdef INCLUDE_FS_TESTS
static void print_node(char type, struct polyfs_inode *i, char *name)
{
	char info[11];

	if (S_ISCHR(i->mode) || (S_ISBLK(i->mode))) {
		/* major/minor numbers can be as high as 2^12 or 4096 */
		snprintf(info, 11, "%4d,%4d", major(i->size), minor(i->size));
	}
	else {
		/* size be as high as 2^24 or 16777216 */
		snprintf(info, 10, "%9d", i->size);
	}

	printf("%c %04o %s %5d:%-3d %s\n",
			type, i->mode & ~S_IFMT, info, i->uid, i->gid, name);
}

/*
 * Create a fake "blocked" access
 */
static void *romfs_read(unsigned long offset)
{
	unsigned int block = offset >> ROMBUFFER_BITS;
	if (block != read_buffer_block) {
		read_buffer_block = block;
		lseek(fd, block << ROMBUFFER_BITS, SEEK_SET);
		read(fd, read_buffer, ROMBUFFERSIZE * 2);
	}
	return read_buffer + (offset & ROMBUFFERMASK);
}

static struct polyfs_inode *polyfs_iget(struct polyfs_inode *i)
{
	struct polyfs_inode *inode = malloc(sizeof(struct polyfs_inode));

	if (!inode) {
		die(FSCK_ERROR, 1, "malloc failed");
	}
#if __BYTE_ORDER == __BIG_ENDIAN
	inode->mode = POLYFS_16(i->mode);
	inode->uid = POLYFS_16(i->uid);
	inode->size = POLYFS_24(i->size);
	inode->gid = i->gid;
	inode->namelen = POLYFS_GET_NAMELEN(i);
	inode->offset = POLYFS_GET_OFFSET(i);
#else /* not __BYTE_ORDER == __BIG_ENDIAN */
	*inode = *i;
#endif /* not __BYTE_ORDER == __BIG_ENDIAN */
	return inode;
}

static struct polyfs_inode *iget(unsigned int ino)
{
	return polyfs_iget(romfs_read(ino));
}

static void iput(struct polyfs_inode *inode)
{
	free(inode);
}

static int uncompress_block(void *src, int len)
{
	int err;

	if (super.flags & POLYFS_FLAG_LZO_COMPRESSION) {
		lzo_uint outlen = POLYFS_BLOCK_SIZE*2;

		if (len > POLYFS_BLOCK_SIZE + (POLYFS_BLOCK_SIZE / 16) + 64 + 3) {
			die(FSCK_UNCORRECTED, 0, "data block too large");
		}

		err = lzo1x_decompress_safe(src, len, (unsigned char *)outbuffer, &outlen, NULL);
		if (err != LZO_E_OK) {
			die(FSCK_UNCORRECTED, 0, "decompression error %d",
				err);
		}

		// take a CRC of the decompressed data
		uint32_t crc = crc32(0L, Z_NULL, 0);
		crc = crc32(crc, (Bytef *)outbuffer, outlen);

		// now try to do an overlapping decompression
		unsigned char *overlap = calloc(1, POLYFS_BLOCK_MAX_SIZE_WITH_OVERHEAD);
		if (!overlap) die(FSCK_UNCORRECTED, 0, "memory allocation failed");
		uint32_t offset = POLYFS_BLOCK_MAX_SIZE_WITH_OVERHEAD - len;
		memcpy(overlap + offset, src, len); // put the compressed data at the end of the buffer
		lzo_uint new_len = len < POLYFS_BLOCK_SIZE ? outlen : POLYFS_BLOCK_SIZE;
		err = lzo1x_decompress_safe(overlap + offset, len, overlap, &new_len, NULL);
		if (err != LZO_E_OK) {
			free(overlap);
			die(FSCK_UNCORRECTED, 0, "LZO overlap decompression failed: %d (1)", err);
		}
		uint32_t crc2 = crc32(0L, Z_NULL, 0);
		crc2 = crc32(crc2, overlap, new_len);
		if (new_len != outlen || crc != crc2) {
			free(overlap);
			die(FSCK_UNCORRECTED, 0, "LZO overlap decompression failed: %d (2)", err);
		}
		free(overlap);

		return outlen;
	}
	else if (super.flags & POLYFS_FLAG_ZLIB_COMPRESSION) {
		stream.next_in = src;
		stream.avail_in = len;
	
		stream.next_out = (unsigned char *) outbuffer;
		stream.avail_out = POLYFS_BLOCK_SIZE*2;
	
		inflateReset(&stream);
	
		if (len > POLYFS_BLOCK_SIZE*2) {
			die(FSCK_UNCORRECTED, 0, "data block too large");
		}
		err = inflate(&stream, Z_FINISH);
		if (err != Z_STREAM_END) {
			die(FSCK_UNCORRECTED, 0, "decompression error %p(%d): %s",
					src, len, zError(err));
		}
		return stream.total_out;
	}
	else {
		if (len > POLYFS_BLOCK_SIZE)
			die(FSCK_UNCORRECTED, 0, "data block too large");

		return len;
	}
}

static void do_uncompress(char *path, int fd, unsigned long offset, unsigned long size)
{
	unsigned long curr = offset + 4 * ((size + POLYFS_BLOCK_SIZE - 1) / POLYFS_BLOCK_SIZE);

	do {
		unsigned long out = POLYFS_BLOCK_SIZE;
		unsigned long next = POLYFS_32(*(uint32_t *) romfs_read(offset));

		if (next > end_data) {
			end_data = next;
		}

		offset += 4;
		if (curr == next) {
			if (opt_verbose > 1) {
				printf("  hole at %ld (%d)\n", curr, POLYFS_BLOCK_SIZE);
			}
			if (size < POLYFS_BLOCK_SIZE)
				out = size;
			memset(outbuffer, 0x00, out);
		}
		else {
			if (opt_verbose > 1) {
				printf("  uncompressing block at %ld to %ld (%ld)\n", curr, next, next - curr);
			}
			out = uncompress_block(romfs_read(curr), next - curr);
		}
		if (size >= POLYFS_BLOCK_SIZE) {
			if (out != POLYFS_BLOCK_SIZE) {
				die(FSCK_UNCORRECTED, 0, "non-block (%ld) bytes", out);
			}
		} else {
			if (out != size) {
				die(FSCK_UNCORRECTED, 0, "non-size (%ld vs %ld) bytes", out, size);
			}
		}
		size -= out;
		if (opt_extract) {
			if (write(fd, outbuffer, out) < 0) {
				die(FSCK_ERROR, 1, "write failed: %s", path);
			}
		}
		curr = next;
	} while (size);
}

static void change_file_status(char *path, struct polyfs_inode *i)
{
	struct utimbuf epoch = { 0, 0 };

	if (euid == 0) {
		if (lchown(path, i->uid, i->gid) < 0) {
			die(FSCK_ERROR, 1, "lchown failed: %s", path);
		}
		if (S_ISLNK(i->mode))
			return;
		if ((S_ISUID | S_ISGID) & i->mode) {
			if (chmod(path, i->mode) < 0) {
				die(FSCK_ERROR, 1, "chown failed: %s", path);
			}
		}
	}
	if (S_ISLNK(i->mode))
		return;
	if (utime(path, &epoch) < 0) {
		die(FSCK_ERROR, 1, "utime failed: %s", path);
	}
}

static void do_directory(char *path, struct polyfs_inode *i)
{
	int pathlen = strlen(path);
	int count = i->size;
	unsigned long offset = i->offset << 2;
	char *newpath = malloc(pathlen + 256);

	if (!newpath) {
		die(FSCK_ERROR, 1, "malloc failed");
	}
	if (offset == 0 && count != 0) {
		die(FSCK_UNCORRECTED, 0, "directory inode has zero offset and non-zero size: %s", path);
	}
	if (offset != 0 && offset < start_dir) {
		start_dir = offset;
	}
	/* TODO: Do we need to check end_dir for empty case? */
	memcpy(newpath, path, pathlen);
	if (pathlen > 1) {
		newpath[pathlen] = '/';
		pathlen++;
	}
	if (opt_verbose) {
		print_node('d', i, path);
	}
	if (opt_extract) {
		if (mkdir(path, i->mode) < 0) {
			die(FSCK_ERROR, 1, "mkdir failed: %s", path);
		}
		change_file_status(path, i);
	}
	while (count > 0) {
		struct polyfs_inode *child = iget(offset);
		int size;
		int newlen = child->namelen << 2;

		size = sizeof(struct polyfs_inode) + newlen;
		count -= size;

		offset += sizeof(struct polyfs_inode);

		memcpy(newpath + pathlen, romfs_read(offset), newlen);
		newpath[pathlen + newlen] = 0;
		if (newlen == 0) {
			die(FSCK_UNCORRECTED, 0, "filename length is zero");
		}
		if ((pathlen + newlen) - strlen(newpath) > 3) {
			die(FSCK_UNCORRECTED, 0, "bad filename length");
		}
		expand_fs(newpath, child);

		offset += newlen;

		if (offset <= start_dir) {
			die(FSCK_UNCORRECTED, 0, "bad inode offset");
		}
		if (offset > end_dir) {
			end_dir = offset;
		}
		iput(child); /* free(child) */
	}
	free(newpath);
}

static void do_file(char *path, struct polyfs_inode *i)
{
	unsigned long offset = i->offset << 2;
	int fd = 0;

	if (offset == 0 && i->size != 0) {
		die(FSCK_UNCORRECTED, 0, "file inode has zero offset and non-zero size");
	}
	if (i->size == 0 && offset != 0) {
		die(FSCK_UNCORRECTED, 0, "file inode has zero size and non-zero offset");
	}
	if (offset != 0 && offset < start_data) {
		start_data = offset;
	}
	if (opt_verbose) {
		print_node('f', i, path);
	}
	if (opt_extract) {
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, i->mode);
		if (fd < 0) {
			die(FSCK_ERROR, 1, "open failed: %s", path);
		}
	}
	if (i->size) {
		do_uncompress(path, fd, offset, i->size);
	}
	if (opt_extract) {
		close(fd);
		change_file_status(path, i);
	}
}

static void do_symlink(char *path, struct polyfs_inode *i)
{
	unsigned long offset = i->offset << 2;
	unsigned long curr = offset + 4;
	unsigned long next = POLYFS_32(*(uint32_t *) romfs_read(offset));
	unsigned long size;

	if (offset == 0) {
		die(FSCK_UNCORRECTED, 0, "symbolic link has zero offset");
	}
	if (i->size == 0) {
		die(FSCK_UNCORRECTED, 0, "symbolic link has zero size");
	}

	if (offset < start_data) {
		start_data = offset;
	}
	if (next > end_data) {
		end_data = next;
	}

	size = uncompress_block(romfs_read(curr), next - curr);
	if (size != i->size) {
		die(FSCK_UNCORRECTED, 0, "size error in symlink: %s", path);
	}
	outbuffer[size] = 0;
	if (opt_verbose) {
		char *str;

		asprintf(&str, "%s -> %s", path, outbuffer);
		print_node('l', i, str);
		if (opt_verbose > 1) {
			printf("  uncompressing block at %ld to %ld (%ld)\n", curr, next, next - curr);
		}
		free(str);
	}
	if (opt_extract) {
		if (symlink(outbuffer, path) < 0) {
			die(FSCK_ERROR, 1, "symlink failed: %s", path);
		}
		change_file_status(path, i);
	}
}

static void do_special_inode(char *path, struct polyfs_inode *i)
{
	dev_t devtype = 0;
	char type;

	if (i->offset) {	/* no need to shift offset */
		die(FSCK_UNCORRECTED, 0, "special file has non-zero offset: %s", path);
	}
	if (S_ISCHR(i->mode)) {
		devtype = i->size;
		type = 'c';
	}
	else if (S_ISBLK(i->mode)) {
		devtype = i->size;
		type = 'b';
	}
	else if (S_ISFIFO(i->mode)) {
		if (i->size != 0) {
			die(FSCK_UNCORRECTED, 0, "fifo has non-zero size: %s", path);
		}
		type = 'p';
	}
	else if (S_ISSOCK(i->mode)) {
		if (i->size != 0) {
			die(FSCK_UNCORRECTED, 0, "socket has non-zero size: %s", path);
		}
		type = 's';
	}
	else {
		die(FSCK_UNCORRECTED, 0, "bogus mode: %s (%o)", path, i->mode);
		return;		/* not reached */
	}

	if (opt_verbose) {
		print_node(type, i, path);
	}

	if (opt_extract) {
		if (mknod(path, i->mode, devtype) < 0) {
			die(FSCK_ERROR, 1, "mknod failed: %s", path);
		}
		change_file_status(path, i);
	}
}

static void expand_fs(char *path, struct polyfs_inode *inode)
{
	if (S_ISDIR(inode->mode)) {
		do_directory(path, inode);
	}
	else if (S_ISREG(inode->mode)) {
		do_file(path, inode);
	}
	else if (S_ISLNK(inode->mode)) {
		do_symlink(path, inode);
	}
	else {
		do_special_inode(path, inode);
	}
}

static void test_fs(int start)
{
	struct polyfs_inode *root;
	unsigned long root_offset;

	root = polyfs_iget(&super.root);
	root_offset = root->offset << 2;
	if (!S_ISDIR(root->mode))
		die(FSCK_UNCORRECTED, 0, "root inode is not directory");
	if (!(super.flags & POLYFS_FLAG_SHIFTED_ROOT_OFFSET) &&
			((root_offset != sizeof(struct polyfs_super)) &&
			 (root_offset != PAD_SIZE + sizeof(struct polyfs_super))))
	{
		die(FSCK_UNCORRECTED, 0, "bad root offset (%lu)", root_offset);
	}
	umask(0);
	euid = geteuid();
	stream.next_in = NULL;
	stream.avail_in = 0;
	inflateInit(&stream);
	expand_fs(extract_dir, root);
	inflateEnd(&stream);
	if (start_data != ~0UL) {
		if (start_data < (sizeof(struct polyfs_super) + start)) {
			die(FSCK_UNCORRECTED, 0, "directory data start (%ld) < sizeof(struct polyfs_super) + start (%ld)", start_data, sizeof(struct polyfs_super) + start);
		}
		if (end_dir != start_data) {
			die(FSCK_UNCORRECTED, 0, "directory data end (%ld) != file data start (%ld)", end_dir, start_data);
		}
	}
	if (super.flags & POLYFS_FLAG_FSID_VERSION_1) {
		if (end_data > super.size) {
			die(FSCK_UNCORRECTED, 0, "invalid file data offset");
		}
	}
	iput(root);		/* free(root) */
}
#endif /* INCLUDE_FS_TESTS */

int main(int argc, char **argv)
{
	int c;			/* for getopt */
	int start = 0;
	size_t length;

	if (argc)
		progname = argv[0];

	/* command line options */
	while ((c = getopt(argc, argv, "hx:v")) != EOF) {
		switch (c) {
			case 'h':
				usage(FSCK_OK);
			case 'x':
#ifdef INCLUDE_FS_TESTS
				opt_extract = 1;
				extract_dir = optarg;
				break;
#else /* not INCLUDE_FS_TESTS */
				die(FSCK_USAGE, 0, "compiled without -x support");
#endif /* not INCLUDE_FS_TESTS */
			case 'v':
				opt_verbose++;
				break;
		}
	}

	if ((argc - optind) != 1)
		usage(FSCK_USAGE);
	filename = argv[optind];

	test_super(&start, &length);
	test_crc(start);
#ifdef INCLUDE_FS_TESTS
	test_fs(start);
#endif /* INCLUDE_FS_TESTS */

	if (opt_verbose) {
		printf("%s: OK\n", filename);
	}

	exit(FSCK_OK);
}

