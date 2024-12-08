/*
    hdfmonkey: A Swiss Army Knife for manipulating HDF disk images
    Copyright (C) 2010 Matt Westcott

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* The following changes required for FatFs (see FatFs.r15.patch):
   - ffconfig.h
	- FF_USE_MKFS 1
	- FF_CODE_PAGE 850 (Latin 1)
	- FF_USE_LFN 1
	- FF_VOLUMES 2
   - ff.h
	- add 'TCHAR label[11];        // Volume label (11 chars max) ' to MKFS_PARM
   - ff.c
	- initialise MKFS_PARM.label in f_mkfs ', "           " };'
	- after memcpy(buf + BS_VolLab32, ... add 'if (opt->label[0]) memcpy(buf+BS_VolLab32, opt->label, 11);'
	- after memcpy(buf + BS_VolLab, ... add the following:
		- if (fsty == FS_FAT12) memcpy(buf+BS_FilSysType+3, "12", 2);
		- else memcpy(buf+BS_FilSysType+3, "16", 2);
		- if (opt->label[0]) memcpy(buf+BS_VolLab, opt->label, 11);
 */

#include <config.h>

#define DIR FATDIR
#include "ff.h"
#undef DIR

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include "image_file.h"
#include "diskio.h"

#include "ffconf.h"

static const char DRV_0[] = "0:";
static const char DRV_1[] = "1:";

#define BUFFER_SIZE 2048
#define CLONE_BUFFER_SIZE 1048576

/* Print an error message for an error returned from the FAT driver */
static void fat_perror(char *custom_message, FRESULT result) {
	char *error_message;
	switch (result) {
		case FR_OK:
			error_message = "No error"; /* Not an error. Obviously. */
			break;
		case FR_DISK_ERR:
			error_message = "Low-level disk error";
			break;
		case FR_INT_ERR:
			error_message = "Internal error";
			break;
		case FR_NOT_READY:
			error_message = "Drive not ready";
			break;
		case FR_NO_FILE:
			error_message = "File not found";
			break;
		case FR_NO_PATH:
			error_message = "Path not found";
			break;
		case FR_INVALID_NAME:
			error_message = "File / directory name is invalid";
			break;
		case FR_DENIED:
			error_message = "Access denied";
			break;
		case FR_EXIST:
			error_message = "File / directory already exists";
			break;
		case FR_INVALID_OBJECT:
			error_message = "Invalid object";
			break;
		case FR_WRITE_PROTECTED:
			error_message = "Drive is write-protected";
			break;
		case FR_INVALID_DRIVE:
			error_message = "Invalid drive number";
			break;
		case FR_NOT_ENABLED:
			error_message = "Work area not initialised";
			break;
		case FR_NO_FILESYSTEM:
			error_message = "No FAT filesystem found";
			break;
		case FR_MKFS_ABORTED:
			error_message = "Disk is unsuitable for formatting";
			break;
		case FR_TIMEOUT:
			error_message = "Timeout";
			break;
		case FR_LOCKED:
			error_message = "File locked";
			break;
		case FR_NOT_ENOUGH_CORE:
			error_message = "Not enough memory";
			break;
		case FR_TOO_MANY_OPEN_FILES:
			error_message = "Too many open files";
			break;
		case FR_INVALID_PARAMETER:
			error_message = "Invalid parameter";
			break;
		default:
			error_message = "Unknown error code";
	}
	printf("%s: %s\n", custom_message, error_message);
}

/* compatibility wrapper for newer FatFs */
static FRESULT f_mkfs_compat(
        BYTE drv,                       /* Logical drive number */
        BYTE partition,         /* Partitioning rule 0:FDISK, 1:SFD */
        WORD allocsize,         /* Allocation unit size [bytes] */
        char *volume_label,
        BYTE fmt
)
{
	FRESULT result;
	BYTE work[FF_MAX_SS];	/* Working buffer */
	const char *path = 0 == drv ? DRV_0 : DRV_1; /* Logical drive number */
	// "format should generate two FATs (required by ESXDOS)" see 8ee674b
	MKFS_PARM opt = { .n_fat = 2, .au_size = allocsize }; /* Format options */
	// NOTE: ff.c has to be patched to change "FAT Signature" in VBR
	// "Use 'FAT12'/'FAT16' as the type descriptor,
	//     not just 'FAT' - ESXDOS checks it to distinguish from FAT32"
	// see a9af42a

	// "allow specifying a volume name on format/create" see 1b470de
	memset(opt.label, ' ', 11);
	if (volume_label) {
		for (int i=0; i<11 && *volume_label; i++) {
			opt.label[i] = *volume_label++;
		}
	}
	// Newer f_mkfs has different params and no longer allows direct
	// FAT type choice. The only option remaining is the ability to
	// force FAT32 (provided volume is big enough)
	// thus, we must re-map FS_FATxx to FM_FATxx
	switch (fmt) {
		case FS_FAT32:
			opt.fmt = FM_FAT32;
			break;
		case FS_FAT12:
		case FS_FAT16:
			opt.fmt = FM_FAT;
			break;
		default:
			opt.fmt = FM_FAT|FM_FAT32;
			break;
	}
	// not used, but for the sake of completeness...
	if (1==partition) {
		opt.fmt |= FM_SFD;
	}

	return f_mkfs(path, &opt, work, sizeof(work));
}

/* Open the file at pathname as an HDF or raw disk image, populating the passed
volume container and opening it as disk 0 for the FAT driver */
static int open_image(char *pathname, volume_container *vol, FATFS *fatfs, int writeable) {
	int res;
	
	if (image_file_is_hdf(pathname)) {
		/* HDF image file found */;
		res = hdf_image_open(vol, pathname, writeable);
	} else {
		/* Raw image file found */
		res = raw_image_open(vol, pathname, writeable);
	}
	if (res) return -1;
	
	disk_map(0, vol);
	
	if (fatfs != NULL) {
		if (f_mount(fatfs, DRV_0, 0) != FR_OK) {
			printf("mount failed\n");
			return -1;
		}
	}
	
	return 0;
}

static int filename_is_hdf(char *filename) {
	size_t len;
	
	len = strlen(filename);
	return (
		(filename[len-3] == 'h' || filename[len-3] == 'H')
		&& (filename[len-2] == 'd' || filename[len-2] == 'D')
		&& (filename[len-1] == 'f' || filename[len-1] == 'F')
	);
}

static int fat_path_is_dir(TCHAR *filename) {
	/* Test whether the given filename is a directory in the FAT filesystem. */
	/* Do this the quick-and-dirty way, by f_opendir-ing and checking for errors */
	FATDIR dir;
	FRESULT result;
	
	result = f_opendir(&dir, filename);
	if (result == FR_OK) {
		return 1;
	} else if (result == FR_NO_PATH) {
		return 0;
	} else {
		fat_perror("Error opening file", result);
		return -1;
	}
}

static char *concat_filename(char *path, char *filename) {
	char *out;
	size_t path_len, filename_len;
	
	path_len = strlen(path);
	filename_len = strlen(filename);
	
	out = malloc(path_len + 1 + filename_len + 1);
	if (!out) return NULL;
	strcpy(out, path);
	strcpy(out + path_len, "/");
	strcpy(out + path_len + 1, filename);
	strcpy(out + path_len + 1 + filename_len, "\0");
	
	return out;
}

static void strip_trailing_slash(char *path) {
	if (*path == '\0') return;
	while (*path != '\0') path++;
	path--;
	if (*path == '\\' || *path == '/') *path = '\0';
}

static int is_directory(char *path) {
	struct stat fileinfo;
	
	if (stat(path, &fileinfo) != 0) {
		return 0;
	}
	return (fileinfo.st_mode & S_IFDIR);
}

static int cmd_clone(int argc, char *argv[]) {
	char *source_filename;
	char *destination_filename;
	volume_container source_vol, destination_vol;
	char buffer[CLONE_BUFFER_SIZE];
	size_t total_size, transfer_size;
	off_t position;
	
	if (argc >= 3) {
		source_filename = argv[2];
	} else {
		printf("No source image filename supplied\n");
		return -1;
	}
	
	if (argc >= 4) {
		destination_filename = argv[3];
	} else {
		printf("No destination image filename supplied\n");
		return -1;
	}
	
	if (open_image(source_filename, &source_vol, NULL, 0) == -1) {
		return -1;
	}
	
	if (filename_is_hdf(destination_filename)) {
		if (hdf_image_create(&destination_vol, destination_filename, source_vol.sector_count) == -1) {
			source_vol.close(&source_vol);
			return -1;
		}
	} else {
		if (raw_image_create(&destination_vol, destination_filename, source_vol.sector_count) == -1) {
			source_vol.close(&source_vol);
			return -1;
		}
	}
	
	position = 0;
	total_size = source_vol.bytes_per_sector * source_vol.sector_count;
	
	while (position < total_size) {
		transfer_size = total_size - position;
		if (transfer_size > CLONE_BUFFER_SIZE) transfer_size = CLONE_BUFFER_SIZE;
		if (source_vol.read(&source_vol, position, buffer, transfer_size) < 0) {
			source_vol.close(&source_vol);
			destination_vol.close(&destination_vol);
			return -1;
		}
		if (destination_vol.write(&destination_vol, position, buffer, transfer_size) < 0) {
			source_vol.close(&source_vol);
			destination_vol.close(&destination_vol);
			return -1;
		}
		position += transfer_size;
	}
	
	source_vol.close(&source_vol);
	destination_vol.close(&destination_vol);
	return 0;
}

static int cmd_get(int argc, char *argv[]) {
	char *image_filename;
	char *source_filename;
	
	volume_container vol;
	FATFS fatfs;
	FRESULT result;
	FIL input_file;
	FILE *output_stream;
	
	char buffer[BUFFER_SIZE];
	UINT bytes_read;
	
	if (argc >= 3) {
		image_filename = argv[2];
	} else {
		printf("No image filename supplied\n");
		return -1;
	}

	if (argc >= 4) {
		source_filename = argv[3];
	} else {
		printf("No source filename supplied\n");
		return -1;
	}
	
	if (argc >= 5) {
		output_stream = fopen(argv[4], "wb");
		if (!output_stream) {
			perror("Could not open file for writing");
			return -1;
		}
	} else {
		output_stream = stdout;
	}
	
	if (open_image(image_filename, &vol, &fatfs, 0) == -1) {
		return -1;
	}
	
	result = f_open(&input_file, source_filename, FA_READ | FA_OPEN_EXISTING);
	if (result != FR_OK) {
		fat_perror("Error opening file", result);
		return -1;
	}
	
	do {
		result = f_read(&input_file, buffer, BUFFER_SIZE, &bytes_read);
		if (result != FR_OK) {
			fat_perror("Error reading file", result);
			f_close(&input_file);
			return -1;
		}
		fwrite(buffer, 1, bytes_read, output_stream);
	} while (bytes_read == BUFFER_SIZE);
	
	f_close(&input_file);
	if (output_stream != stdout) {
		fclose(output_stream);
	}
	
	return 0;
}

static int put_file(char *source_filename, char *dest_filename) {
	FILE *input_file;
	FIL output_file;
	FRESULT result;
	
	char buffer[BUFFER_SIZE];
	size_t bytes_read;
	UINT bytes_written;
	
	DIR *dir;
	struct dirent *dir_entry;
	
	char *source_child_filename;
	char *dest_child_filename;
	
	if (is_directory(source_filename)) {
		if (!fat_path_is_dir(dest_filename)) {
			result = f_mkdir(dest_filename);
			if (result != FR_OK) {
				fat_perror("Directory creation failed", result);
				return -1;
			}
		}
		dir = opendir(source_filename);
		if (!dir) {
			perror("Error opening directory");
			return -1;
		}
		
		while ((dir_entry = readdir(dir))) {
			if ( strcmp(dir_entry->d_name, ".") != 0 && strcmp(dir_entry->d_name, "..") != 0 ) {
				source_child_filename = concat_filename(source_filename, dir_entry->d_name);
				dest_child_filename = concat_filename(dest_filename, dir_entry->d_name);
				put_file(source_child_filename, dest_child_filename);
				free(source_child_filename);
				free(dest_child_filename);
			}
		}
		
		closedir(dir);
	} else {
		input_file = fopen(source_filename, "rb");
		if (!input_file) {
			perror("Could not open file for reading");
			return -1;
		}
		
		result = f_open(&output_file, dest_filename, FA_WRITE | FA_CREATE_ALWAYS);
		if (result != FR_OK) {
			fat_perror("Error opening file for writing", result);
			return -1;
		}
		
		do {
			bytes_read = fread(buffer, 1, BUFFER_SIZE, input_file);
			if (ferror(input_file)) {
				perror("Error reading file");
				return -1;
			}
			if (bytes_read != 0) {
				result = f_write(&output_file, buffer, bytes_read, &bytes_written);
				if (result != FR_OK) {
					fat_perror("Error writing file", result);
					f_close(&output_file);
					return -1;
				}
			}
		} while (bytes_read == BUFFER_SIZE);
		
		fclose(input_file);
		f_close(&output_file);
	}

	return 0;
}

static int cmd_put(int argc, char *argv[]) {
	char *image_filename;
	char *source_filename;
	char *dest_path;
	char *dest_filename;
	
	volume_container vol;
	FATFS fatfs;
	int copying_to_dir;
	int i;
	
	if (argc < 4) {
		printf("Usage: hdfmonkey put <image_file> <source_files> <destination_file_or_dir>\n");
		return -1;
	}
	
	image_filename = argv[2];
	if (open_image(image_filename, &vol, &fatfs, 1) == -1) {
		return -1;
	}
	
	dest_path = argv[argc-1];
	strip_trailing_slash(dest_path);
	copying_to_dir = fat_path_is_dir(dest_path);
	if (copying_to_dir == -1) {
		return -1;
	}
	
	if (!copying_to_dir) {
		if (argc > 5) {
			printf("Destination must be an existing directory when copying multiple files\n");
			return -1;
		}
		
		source_filename = argv[3];
		
		if ( put_file(source_filename, dest_path) == -1 ) {
			return -1;
		}
		
		return 0;
	} else {
		for (i = 3; i < (argc-1); i++) {
			dest_filename = concat_filename(dest_path, basename(argv[i]));
			if (!dest_filename) {
				printf("Out of memory\n");
				return -1;
			}
			put_file(argv[i], dest_filename);
			free(dest_filename);
		}
		return 0;
	}
}

static int cmd_ls(int argc, char *argv[]) {
	char *image_filename;
	volume_container vol_container;
	
	FATFS fatfs;
	FATDIR dir;
	FRESULT result;
	FILINFO file_info;
	
	TCHAR* dirname;
	
	if (argc < 3) {
		printf("No image filename supplied\n");
		return -1;
	}
	
	image_filename = argv[2];
	
	if (open_image(image_filename, &vol_container, &fatfs, 0) == -1) {
		return -1;
	}
	
	if (argc > 3) {
		/* explicit path specified */
		dirname = argv[3];
	} else {
		/* no path specified - use root */
		dirname = "";
	}
	
	if ((result = f_opendir(&dir, dirname)) != FR_OK) {
		fat_perror("Error opening dir", result);
		return -1;
	}
	
	while(1) {
		if ((result = f_readdir(&dir, &file_info)) != FR_OK) {
			fat_perror("Error reading dir", result);
			return -1;
		}
		if (file_info.fname[0] == '\0') break;
		
		/* indicate whether file is a dir or a regular file */
		if (file_info.fattrib & AM_DIR) {
			printf("[DIR]\t");
		} else {
			printf("%d\t", file_info.fsize);
		}
		
		/* TL;DR: just use fname unless you really need 8.3 name */
		printf("%s\n", file_info.fname);
/* http://elm-chan.org/fsw/ff/doc/readdir.html
"When LFN is enabled, a member altname[] is defined in the file
information structure to store the short file name of the object. If the
long file name is not accessible due to any reason listed below, short
file name is stored to the fname[] and the altname[] has a null string."
*/
	}
	
	vol_container.close(&vol_container);

	return 0;
}

static int cmd_format(int argc, char *argv[]) {
	char *image_filename;
	char *volumelabel = NULL;
	volume_container vol;
	FATFS fatfs;
	FRESULT result;
	BYTE fmt = 0;
	int i;
	
	int arg_num = 0;
	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--fat12") == 0) {
			fmt = FS_FAT12;
		} else if (strcmp(argv[i], "--fat16") == 0) {
			fmt = FS_FAT16;
		} else if (strcmp(argv[i], "--fat32") == 0) {
			fmt = FS_FAT32;
		} else {
			switch (arg_num) {
				case 0:
					image_filename = argv[i];
					arg_num++;
					break;
				case 1:
					volumelabel = argv[i];
					arg_num++;
					break;
				default:
					arg_num++;
			}
		}
	}

	if (arg_num < 1 || arg_num > 2) {
		printf("Usage: hdfmonkey format [--fat12|--fat16|--fat32] <imagefile> [volumelabel]\n");
		return -1;
	}

	if (open_image(image_filename, &vol, &fatfs, 1) == -1) {
		return -1;
	}
	
	result = f_mkfs_compat(0, 0, 0, volumelabel, fmt);
	if (result != FR_OK) {
		fat_perror("Formatting failed", result);
		return -1;
	}
	
	return 0;
}

static int cmd_create(int argc, char *argv[]) {
	char *image_filename;
	volume_container vol;
	FATFS fatfs;
	FRESULT result;
	double unconverted_size;
	unsigned long converted_size;
	char *unit;
	char *volumelabel = NULL;
	BYTE fmt = 0;
	int i;

	int arg_num = 0;
	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--fat12") == 0) {
			fmt = FS_FAT12;
		} else if (strcmp(argv[i], "--fat16") == 0) {
			fmt = FS_FAT16;
		} else if (strcmp(argv[i], "--fat32") == 0) {
			fmt = FS_FAT32;
		} else {
			switch (arg_num) {
				case 0:
					image_filename = argv[i];
					arg_num++;
					break;
				case 1:
					unconverted_size = strtod(argv[i], &unit);
					arg_num++;
					break;
				case 2:
					volumelabel = argv[i];
					arg_num++;
					break;
				default:
					arg_num++;
			}
		}
	}

	if (arg_num < 2 || arg_num > 3) {
		printf("Usage: hdfmonkey create [--fat12|--fat16|--fat32] <imagefile> <size> [volumelabel]\n");
		return -1;
	}

	if (*unit == 'G' || *unit == 'g') {
		converted_size = (unsigned long) (unconverted_size * (1<<30) / 512);
	} else if (*unit == 'M' || *unit == 'm') {
		converted_size = (unsigned long) (unconverted_size * (1<<20) / 512);
	} else if (*unit == 'K' || *unit == 'k') {
		converted_size = (unsigned long) (unconverted_size * (1<<10) / 512);
	} else if (*unit == 'B' || *unit == 'b' || *unit == 0) {
		converted_size = (unsigned long) (unconverted_size / 512);
	} else {
		printf("Unrecognised size unit specifier: %c\n", *unit);
		return -1;
	}
	
	if (filename_is_hdf(image_filename)) {
		if (hdf_image_create(&vol, image_filename, converted_size) == -1) {
			return -1;
		}
	} else {
		if (raw_image_create(&vol, image_filename, converted_size) == -1) {
			return -1;
		}
	}
	
	disk_map(0, &vol);
	
	if (f_mount(&fatfs, DRV_0, 0) != FR_OK) {
		printf("mount failed\n");
		vol.close(&vol);
		return -1;
	}
	
	result = f_mkfs_compat(0, 0, 0, volumelabel, fmt);
	if (result != FR_OK) {
		fat_perror("Formatting failed", result);
		vol.close(&vol);
		return -1;
	}
	
	vol.close(&vol);
	return 0;
}

static int cmd_mkdir(int argc, char *argv[]) {
	char *image_filename;
	char *dir_name;
	volume_container vol;
	FATFS fatfs;
	FRESULT result;
	
	if (argc < 3) {
		printf("No image filename supplied\n");
		return -1;
	}
	image_filename = argv[2];
	
	if (argc < 4) {
		printf("No directory name supplied\n");
		return -1;
	}
	dir_name = argv[3];
	
	if (open_image(image_filename, &vol, &fatfs, 1) == -1) {
		return -1;
	}
	
	result = f_mkdir(dir_name);
	if (result != FR_OK) {
		fat_perror("Directory creation failed", result);
		return -1;
	}
	
	return 0;
}

static int cmd_rm(int argc, char *argv[]) {
	char *image_filename;
	char *filename;
	volume_container vol;
	FATFS fatfs;
	FRESULT result;
	
	if (argc < 3) {
		printf("No image filename supplied\n");
		return -1;
	}
	image_filename = argv[2];
	
	if (argc < 4) {
		printf("No filename supplied\n");
		return -1;
	}
	filename = argv[3];
	
	if (open_image(image_filename, &vol, &fatfs, 1) == -1) {
		return -1;
	}
	
	result = f_unlink(filename);
	if (result != FR_OK) {
		fat_perror("Deletion failed", result);
		return -1;
	}
	
	return 0;
}

/* Recursively copy directory contents file-by-file from one filesystem to another.
The destination directory must exist. */
static int copy_dir(TCHAR *source_dirname, TCHAR *destination_dirname) {
	FIL source_file, destination_file;
	FRESULT result;
	char buffer[BUFFER_SIZE];
	UINT bytes_read, bytes_written;
	FATDIR source_dir;
	FILINFO file_info;
	TCHAR *source_filename, *destination_filename;

	result = f_opendir(&source_dir, source_dirname);
	if (result != FR_OK) {
		fat_perror("Error opening source directory", result);
		return -1;
	}

	while(1) {
		if ((result = f_readdir(&source_dir, &file_info)) != FR_OK) {
			fat_perror("Error reading dir", result);
			return -1;
		}
		if (file_info.fname[0] == '\0') break;
		
		/* TL;DR: just use fname unless you really need 8.3 name */
		source_filename = concat_filename(source_dirname, file_info.fname);
		destination_filename = concat_filename(destination_dirname, file_info.fname);
/* http://elm-chan.org/fsw/ff/doc/readdir.html
"When LFN is enabled, a member altname[] is defined in the file
information structure to store the short file name of the object. If the
long file name is not accessible due to any reason listed below, short
file name is stored to the fname[] and the altname[] has a null string."
*/

		if (file_info.fattrib & AM_DIR) {
			/* File is a directory - copy recursively */
			result = f_mkdir(destination_filename);
			if (result != FR_OK) {
				fat_perror("Error creating directory", result);
				free(source_filename);
				free(destination_filename);
				return -1;
			}
			if (copy_dir(source_filename, destination_filename) != 0) {
				free(source_filename);
				free(destination_filename);
				return -1;
			}
		} else {
			/* File is a regular file */
			result = f_open(&source_file, source_filename, FA_READ);
			if (result != FR_OK) {
				printf("error on file %s\n", source_filename);
				fat_perror("Error opening source file", result);
				free(source_filename);
				free(destination_filename);
				return -1;
			}

			result = f_open(&destination_file, destination_filename, FA_WRITE | FA_CREATE_ALWAYS);
			if (result != FR_OK) {
				fat_perror("Error opening destination file", result);
				free(source_filename);
				free(destination_filename);
				f_close(&source_file);
				return -1;
			}

			do {
				result = f_read(&source_file, buffer, BUFFER_SIZE, &bytes_read);
				if (result != FR_OK) {
					fat_perror("Error reading file", result);
					f_close(&source_file);
					f_close(&destination_file);
					free(source_filename);
					free(destination_filename);
					return -1;
				}
				result = f_write(&destination_file, buffer, bytes_read, &bytes_written);
				if (result != FR_OK) {
					fat_perror("Error writing file", result);
					f_close(&source_file);
					f_close(&destination_file);
					free(source_filename);
					free(destination_filename);
					return -1;
				}
			} while (bytes_read == BUFFER_SIZE);

			f_close(&source_file);
			f_close(&destination_file);
		}

		free(source_filename);
		free(destination_filename);
	}

	return 0;
}

static int cmd_rebuild(int argc, char *argv[]) {
	char *source_filename;
	char *destination_filename;
	char *volumelabel = NULL;
	volume_container source_vol, destination_vol;
	FATFS source_fatfs, destination_fatfs;
	FRESULT result;

	BYTE fmt = 0;
	int i;

	int arg_num = 0;
	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--fat12") == 0) {
			fmt = FS_FAT12;
		} else if (strcmp(argv[i], "--fat16") == 0) {
			fmt = FS_FAT16;
		} else if (strcmp(argv[i], "--fat32") == 0) {
			fmt = FS_FAT32;
		} else {
			switch (arg_num) {
				case 0:
					source_filename = argv[i];
					arg_num++;
					break;
				case 1:
					destination_filename = argv[i];
					arg_num++;
					break;
				case 2:
					volumelabel = argv[i];
					arg_num++;
					break;
				default:
					arg_num++;
			}
		}
	}

	if (arg_num < 2 || arg_num > 3) {
		printf("Usage: hdfmonkey rebuild [--fat12|--fat16|--fat32] <source-image-file> <destination-image-file> [volumelabel]\n");
		return -1;
	}
	
	if (open_image(source_filename, &source_vol, &source_fatfs, 0) == -1) {
		return -1;
	}
	
	if (filename_is_hdf(destination_filename)) {
		if (hdf_image_create(&destination_vol, destination_filename, source_vol.sector_count) == -1) {
			source_vol.close(&source_vol);
			return -1;
		}
	} else {
		if (raw_image_create(&destination_vol, destination_filename, source_vol.sector_count) == -1) {
			source_vol.close(&source_vol);
			return -1;
		}
	}
	
	disk_map(1, &destination_vol);
	
	if (f_mount(&destination_fatfs, DRV_1, 0) != FR_OK) {
		printf("mount failed\n");
		source_vol.close(&source_vol);
		destination_vol.close(&destination_vol);
		return -1;
	}
	
	result = f_mkfs_compat(1, 0, 0, volumelabel, fmt);
	if (result != FR_OK) {
		fat_perror("Formatting failed", result);
		source_vol.close(&source_vol);
		destination_vol.close(&destination_vol);
		return -1;
	}

	copy_dir("0:", "1:");

	source_vol.close(&source_vol);
	destination_vol.close(&destination_vol);
	return 0;
}

static int cmd_help(int argc, char *argv[]) {
	if (argc < 3) {
		printf("hdfmonkey: utility for manipulating HDF disk images v" VERSION "\n");
		printf("           using FatFs revision %d\n\n", FF_DEFINED);
		printf("usage: hdfmonkey <command> [args]\n\n");
		printf("Type 'hdfmonkey help <command>' for help on a specific command.\n");
		printf("Available commands:\n");
		printf("\tclone\n\tcreate\n\tformat\n\tget\n\thelp\n\tls\n\tmkdir\n\tput\n\trebuild\n\trm\n");
	} else if (strcmp(argv[2], "clone") == 0) {
		printf("clone: Make a new image file from a disk or image, possibly in a different container format\n");
		printf("usage: hdfmonkey clone <oldimagefile> <newimagefile>\n");
	} else if (strcmp(argv[2], "create") == 0) {
		printf("create: Create a new FAT-formatted image file\n");
		printf("usage: hdfmonkey create [--fat12|--fat16|--fat32] <imagefile> <size> [volumelabel]\n");
		printf("Size is given in bytes (B), kilobytes (K), megabytes (M) or gigabytes (G) -\n");
		printf("e.g. 64M, 1.5G\n");
	} else if (strcmp(argv[2], "format") == 0) {
		printf("format: Formats the entire disk image as a FAT filesystem\n");
		printf("usage: hdfmonkey format [--fat12|--fat16|--fat32] <imagefile> [volumelabel]\n");
	} else if (strcmp(argv[2], "get") == 0) {
		printf("get: Copy a file from the disk image to a local file\n");
		printf("usage: hdfmonkey get <imagefile> <sourcefile> [destfile]\n");
		printf("Will write the file to standard output if no destination file is specified.\n");
	} else if (strcmp(argv[2], "help") == 0) {
		printf("help: Describe the usage of this program or its commands.\n");
		printf("usage: hdfmonkey help [command]\n");
	} else if (strcmp(argv[2], "ls") == 0) {
		printf("ls: Show a directory listing\n");
		printf("usage: hdfmonkey ls <imagefile> [path]\n");
		printf("Will list the root directory if no path is specified.\n");
	} else if (strcmp(argv[2], "mkdir") == 0) {
		printf("mkdir: Create a directory\n");
		printf("usage: hdfmonkey mkdir <imagefile> <dirname>\n");
	} else if (strcmp(argv[2], "put") == 0) {
		printf("put: Copy local files to the disk image\n");
		printf("usage: hdfmonkey put <image-file> <source-files> <dest-file-or-dir>\n");
	} else if (strcmp(argv[2], "rebuild") == 0) {
		printf("rebuild: Copy contents of the source image file-by-file to a new disk image;\n\tensures that the resulting image is unfragmented.\n");
		printf("usage: hdfmonkey rebuild [--fat12|--fat16|--fat32] <source-image-file> <destination-image-file> [volumelabel]\n");
	} else if (strcmp(argv[2], "rm") == 0) {
		printf("rm: Remove a file or directory\n");
		printf("usage: hdfmonkey rm <imagefile> <filename>\n");
		printf("Directories must be empty before they can be deleted.\n");
	} else {
		printf("Unknown command: '%s'\n", argv[2]);
	}
	return 0;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		/* fall through to help prompt */
	} else if (strcmp(argv[1], "clone") == 0) {
		return cmd_clone(argc, argv);
	} else if (strcmp(argv[1], "create") == 0) {
		return cmd_create(argc, argv);
	} else if (strcmp(argv[1], "format") == 0) {
		return cmd_format(argc, argv);
	} else if (strcmp(argv[1], "get") == 0) {
		return cmd_get(argc, argv);
	} else if (strcmp(argv[1], "help") == 0) {
		return cmd_help(argc, argv);
	} else if (strcmp(argv[1], "ls") == 0) {
		return cmd_ls(argc, argv);
	} else if (strcmp(argv[1], "mkdir") == 0) {
		return cmd_mkdir(argc, argv);
	} else if (strcmp(argv[1], "put") == 0) {
		return cmd_put(argc, argv);
	} else if (strcmp(argv[1], "rebuild") == 0) {
		return cmd_rebuild(argc, argv);
	} else if (strcmp(argv[1], "rm") == 0) {
		return cmd_rm(argc, argv);
	} else {
		printf("Unknown command: '%s'\n", argv[1]);
	}
	printf("Type 'hdfmonkey help' for usage. v" VERSION "\n");
	return 0;
}
