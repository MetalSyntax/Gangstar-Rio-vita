/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/threadmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

/*
 * Gangstar Rio: el nativo compone rutas desde DataDir/HomeDir/TempDir
 * (que main.c ya apunta a ux0:data/gangstarrio/...), pero quedan restos
 * absolutos de Android en el .so (strings verificadas):
 *   /sdcard/gameloft/games/GloftG4HM  -> DATA_PATH "data"  (respaldo de
 *       archivos sueltos, p.ej. _packRes/PhonePerformanceProfiles.gmap en
 *       out_ghidra.c:35433; mismo dir que el dataDir de setPaths)
 *   /sdcard/...                       -> DATA_PATH
 *   /data/data/com.gameloft.android.ANMP.GloftG4HM -> DATA_PATH "saves"
 * Ademas se colapsan "//" (el Fs hace VJoinPath chapuceros).
 *
 * Buffers rotativos con indice atomico: lo llaman a la vez el hilo GL, el
 * de streaming de texturas y el de Vox. 16 slots alcanzan para que ninguna
 * ruta se pise mientras el llamador la usa (solo vive hasta el open/stat).
 */
#define RIO_PATH_SLOTS 16

const char *rio_translate_path(const char *path) {
    static char bufs[RIO_PATH_SLOTS][1024];
    static volatile unsigned slot = 0;
    char *out;

    if (!path) return path;

    /* El glitch FS solo reconoce como absoluta una ruta que empieza con
     * '/': a una ruta Vita ("ux0:data/...", p.ej. la del obb que devuelve
     * getObbFileName) le antepone sus directorios de trabajo
     * ("../Documents/", el DataDir) -> "../Documents/ux0:data/...". Un
     * dispositivo Vita en medio de la ruta significa que lo anterior es un
     * prefijo espurio: se descarta (log_004, el obb no se montaba y
     * ChoosePerfProfile moria sin perfiles). */
    static const char *const vita_devs[] = { "ux0:", "uma0:", "imc0:", "app0:" };
    for (unsigned i = 0; i < sizeof(vita_devs) / sizeof(vita_devs[0]); i++) {
        const char *dev = strstr(path, vita_devs[i]);
        if (dev && dev != path && dev[-1] == '/') {
            path = dev;
            break;
        }
    }

    out = bufs[__atomic_fetch_add(&slot, 1, __ATOMIC_RELAXED) % RIO_PATH_SLOTS];

    static const struct { const char *from; const char *to; } map[] = {
        { "/sdcard/gameloft/games/GloftG4HM", DATA_PATH "data" },
        { "/data/data/com.gameloft.android.ANMP.GloftG4HM", DATA_PATH "saves" },
        { "/sdcard", DATA_PATH },
    };

    const char *rest = NULL;
    const char *prefix = NULL;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        size_t n = strlen(map[i].from);
        if (strncmp(path, map[i].from, n) == 0 && (path[n] == '/' || path[n] == '\0')) {
            prefix = map[i].to;
            rest = path + n;
            break;
        }
    }

    if (prefix) {
        snprintf(out, sizeof(bufs[0]), "%s/%s", prefix, rest);
    } else {
        snprintf(out, sizeof(bufs[0]), "%s", path);
    }

    /* Colapsar "//" -> "/" in place (conserva "ux0:/" inicial... DATA_PATH
     * ya trae el formato "ux0:data/..." sin "://" asi que es seguro). */
    char *dst = out, *src = out;
    while (*src) {
        *dst = *src;
        if (*src == '/' && *(src + 1) == '/') src++;
        dst++; src++;
    }
    *dst = '\0';

    if (out != path && strcmp(out, path) != 0)
        l_debug("rio_translate_path(%s) -> %s", path, out);
    return out;
}

FILE * fopen_soloader(const char * filename, const char * mode) {
    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }

    filename = rio_translate_path(filename);

#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(filename, mode);
#else
    FILE* ret = fopen(filename, mode);
#endif

    if (ret)
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    else
        l_warn("fopen(%s, %s): %p", filename, mode, ret);

    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    }

    path = rio_translate_path(path);

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    struct stat st;
    path = rio_translate_path(path);
    int res = stat(path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i", path, res);
    return res;
}

/* mkdir/remove/unlink: mismas rutas que fopen/stat (saves del HomeDir,
 * temporales del TempDir), asi que pasan por la misma traduccion. */
int mkdir_soloader(const char * path, mode_t mode) {
    path = rio_translate_path(path);
    int res = mkdir(path, mode);
    l_debug("mkdir(%s): %i", path, res);
    return res;
}

int remove_soloader(const char * path) {
    path = rio_translate_path(path);
    int res = remove(path);
    l_debug("remove(%s): %i", path, res);
    return res;
}

int unlink_soloader(const char * path) {
    path = rio_translate_path(path);
    int res = unlink(path);
    l_debug("unlink(%s): %i", path, res);
    return res;
}

/* rewind: el FILE* viene de fopen_soloader, que con USE_SCELIBC_IO es un
 * FILE de SceLibc -- el rewind de newlib lo interpretaria con otro layout. */
void rewind_soloader(FILE * f) {
#ifdef USE_SCELIBC_IO
    sceLibcBridge_fseek(f, 0, SEEK_SET);
#else
    rewind(f);
#endif
}

int fclose_soloader(FILE * f) {
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

int close_soloader(int fd) {
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    _pathname = (char *)rio_translate_path(_pathname);
    DIR* ret = opendir(_pathname);
    l_debug("opendir(\"%s\"): %p", _pathname, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}
