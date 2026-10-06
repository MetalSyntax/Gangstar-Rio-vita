/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  io.h
 * @brief Wrappers and implementations for some of the IO functions.
 */

#ifndef SOLOADER_IO_H
#define SOLOADER_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/dirent.h>
#include <sys/syslimits.h>
#include <sys/fcntl.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef DT_DIR
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14
#endif

/* struct stat de bionic en ARM (== stat64 del kernel, EABI): 104 bytes,
 * st_size en el offset 48. Tipos de ancho fijo y alineacion NATURAL (no
 * packed): los nlink_t/uid_t/gid_t de newlib son de 2 bytes y el struct
 * packed anterior dejaba st_size en 38. glf::FileStream::Impl::Open lee
 * [stat+48] como tamano del archivo -> 0 -> todas las lecturas del obb
 * devolvian 0 bytes (dump 1790973342, CCustomPakReader::scanLocalHeader). */
typedef struct stat64_bionic {
    uint64_t st_dev;            /* 0 */
    uint8_t  __pad0[4];         /* 8 */
    uint32_t __st_ino;          /* 12 */
    uint32_t st_mode;           /* 16 */
    uint32_t st_nlink;          /* 20 */
    uint32_t st_uid;            /* 24 */
    uint32_t st_gid;            /* 28 */
    uint64_t st_rdev;           /* 32 */
    uint8_t  __pad3[4];         /* 40 (+4 de relleno EABI) */
    int64_t  st_size;           /* 48 */
    uint32_t st_blksize;        /* 56 (+4 de relleno EABI) */
    uint64_t st_blocks;         /* 64 */
    struct { int32_t tv_sec; int32_t tv_nsec; } st_atim, st_mtim, st_ctim; /* 72, 80, 88 */
    uint64_t st_ino;            /* 96 */
} stat64_bionic;

_Static_assert(offsetof(stat64_bionic, st_size) == 48, "bionic stat: st_size @ 48");
_Static_assert(offsetof(stat64_bionic, st_blocks) == 64, "bionic stat: st_blocks @ 64");
_Static_assert(offsetof(stat64_bionic, st_mtim) == 80, "bionic stat: st_mtim @ 80");
_Static_assert(sizeof(stat64_bionic) == 104, "bionic stat: 104 bytes");

typedef struct __attribute__((__packed__)) dirent64_bionic {
    int16_t d_ino; // 2 bytes // offset 0x0
    int64_t d_off; // 8 bytes // offset 0x2
    uint64_t d_reclen; // 8 bytes // 0xA
    unsigned char d_type; // 1 byte // offset 0x12
    char d_name[256]; // 256 bytes // offset 0x13
} dirent64_bionic;

int open_soloader(const char * path, int oflag, ...);

FILE * fopen_soloader(const char * filename, const char * mode);

/* Traduce rutas absolutas de Android a DATA_PATH. Devuelve puntero a
 * buffer estatico rotativo (4 entradas); valido hasta la 5a llamada. */
const char *rio_translate_path(const char *path);

DIR *opendir_soloader(char *name);

int stat_soloader(const char * path, stat64_bionic * buf);

int mkdir_soloader(const char * path, mode_t mode);
int remove_soloader(const char * path);
int unlink_soloader(const char * path);
void rewind_soloader(FILE * f);

int fstat_soloader(int fd, stat64_bionic * buf);

struct dirent64_bionic * readdir_soloader(DIR *dir);

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result);

int close_soloader(int fd);

int fclose_soloader(FILE *f);

int closedir_soloader(DIR *dir);

int fcntl_soloader(int fd, int cmd, ...);

int ioctl_soloader(int fd, int request, ... /* arg */);

int fsync_soloader(int fd);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_IO_H
