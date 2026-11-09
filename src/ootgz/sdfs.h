/* The SD card through the game's own nn::fs (exported by sdk), as settings.c and lang.c use it. */
#ifndef SDFS_H
#define SDFS_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t nn_result_t;
typedef struct { void *handle; } fs_file_t;
typedef struct { int32_t flags; } fs_write_option_t;
enum { FS_READ = 1, FS_WRITE = 2, FS_ALLOW_APPEND = 4, FS_WRITE_FLUSH = 1 };

nn_result_t fs_mount_sd(const char *name) __asm__("_ZN2nn2fs11MountSdCardEPKc");
nn_result_t fs_mount_sd_debug(const char *name) __asm__("_ZN2nn2fs19MountSdCardForDebugEPKc");
nn_result_t fs_create_dir(const char *path) __asm__("_ZN2nn2fs15CreateDirectoryEPKc");
nn_result_t fs_create_file(const char *path, int64_t size) __asm__("_ZN2nn2fs10CreateFileEPKcl");
nn_result_t fs_delete_file(const char *path) __asm__("_ZN2nn2fs10DeleteFileEPKc");
nn_result_t fs_open(fs_file_t *out, const char *path, int mode) __asm__("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
nn_result_t fs_size(int64_t *out, fs_file_t f) __asm__("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE");
nn_result_t fs_read(size_t *read, fs_file_t f, int64_t offset, void *buf, size_t size)
  __asm__("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm");
nn_result_t fs_write(fs_file_t f, int64_t offset, const void *buf, size_t size, const fs_write_option_t *opt)
  __asm__("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE");
void fs_close(fs_file_t f) __asm__("_ZN2nn2fs9CloseFileENS0_10FileHandleE");

#define SD_MOUNT "ootgz"
#define SD_DIR SD_MOUNT ":/sloperinaGZ"
#define SD_OLD_DIR SD_MOUNT ":/ootgz" /* before the rename: read if the new folder has nothing yet */

#endif
