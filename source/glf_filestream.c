/*
 * glf_filestream.c -- glf::FileStream::Impl sobre sceIo (sin FILE*).
 *
 * glf::FileStream::Impl (0x90cd50-0x90cf78, ARM) es la UNICA capa del motor
 * que toca el FILE* (impl+8): Open/Close/Read/ReadWithOffset/Write/Seek/
 * Tell; el resto (FileStream, FileCache, CGlfReadFile, los pak readers) va
 * por la vtable del Impl. Por ella pasan los OBB y todos los recursos.
 *
 * Por que se reemplaza entera (log_008, dump 1790988167):
 *  - glitch::res::File::Init murio reubicando un .bdae ("WaterShader-fx",
 *    0x10a0 bytes, en main.obb @0x7e4d65a7): los primeros 0x340 bytes eran
 *    correctos y el resto venia de 0xFC00 bytes MAS ADELANTE en el obb
 *    (verificado contra el archivo en disco con dos muestras). La posicion
 *    del FILE* de SceLibc se desfasaba a mitad de una lectura (buffering de
 *    SceLibc + fseek/ftell).
 *  - ReadWithOffset original hace pread() con el `_file` leido INLINE de
 *    FILE+14 (layout de bionic): con FILE* de SceLibc es un fd basura.
 *
 * Aca cada handle es un SceUID con posicion propia, y TODA lectura es
 * posicional (sceIoPread): no hay buffer ni posicion compartida en el fd,
 * asi que Read y ReadWithOffset concurrentes no se pisan.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <so_util/so_util.h>

#include "reimpl/io.h"
#include "utils/logger.h"

extern so_module so_mod;

/* glf::ios::openflag (deducido de Impl::Open @ 0x90cd50) */
#define GLF_IN      0x01
#define GLF_OUT     0x02
#define GLF_ATEND   0x04  /* fseek(END) tras abrir */
#define GLF_TRUNC   0x08  /* "wb" en vez de "r+b" */
#define GLF_CREATE  0x10  /* fopen("a+b") previo: crear si no existe */

typedef struct {
    SceUID fd;
    SceOff pos;
} glf_vfile;

/* Campos del Impl que usa el codigo original */
#define IMPL_OWNER(impl) (*(uintptr_t *)((uintptr_t)(impl) + 4))
#define IMPL_FILE(impl)  (*(glf_vfile **)((uintptr_t)(impl) + 8))

static void (*FilePosition_SetFileSize)(void *filepos, unsigned size);

static int glf_Impl_Open(void *impl, const char *path, unsigned flags) {
    IMPL_FILE(impl) = NULL;

    int out = (flags & GLF_OUT) != 0;
    if (!(flags & GLF_IN) && !out)
        return 0;

    path = rio_translate_path(path);

    if (flags & GLF_CREATE) {
        SceUID c = sceIoOpen(path, SCE_O_RDWR | SCE_O_CREAT, 0777);
        if (c < 0) return 0;
        sceIoClose(c);
    }

    int mode;
    if (!out)
        mode = SCE_O_RDONLY;                              /* "rb"  */
    else if (flags & GLF_TRUNC)
        mode = SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC;  /* "wb"  */
    else
        mode = SCE_O_RDWR;                                /* "r+b" */

    SceUID fd = sceIoOpen(path, mode, 0777);
    if (fd < 0) {
        l_debug("[glf_io] open(%s, 0x%x): 0x%08x", path, flags, fd);
        return 0;
    }

    glf_vfile *f = malloc(sizeof(*f));
    if (!f) {
        sceIoClose(fd);
        return 0;
    }
    f->fd = fd;
    f->pos = 0;

    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size < 0) size = 0;
    if (FilePosition_SetFileSize)
        FilePosition_SetFileSize((void *)(IMPL_OWNER(impl) + 24), (unsigned)size);
    if (flags & GLF_ATEND)
        f->pos = size;

    IMPL_FILE(impl) = f;
    l_debug("[glf_io] open(%s, 0x%x): fd 0x%x, %lld bytes", path, flags, fd, size);
    return 1;
}

static void glf_Impl_Close(void *impl) {
    glf_vfile *f = IMPL_FILE(impl);
    if (!f) return;
    sceIoClose(f->fd);
    free(f);
    IMPL_FILE(impl) = NULL;
}

static unsigned glf_Impl_Read(void *impl, void *buf, unsigned size) {
    glf_vfile *f = IMPL_FILE(impl);
    if (!f || !size) return 0;
    int r = sceIoPread(f->fd, buf, size, f->pos);
    if (r <= 0) return 0;
    f->pos += r;
    return (unsigned)r;
}

static int glf_Impl_ReadWithOffset(void *impl, void *buf, unsigned size, unsigned offset) {
    glf_vfile *f = IMPL_FILE(impl);
    if (!f) return -1;
    int r = sceIoPread(f->fd, buf, size, offset);
    if (r < 0)
        l_warn("[glf_io] pread(fd 0x%x, %u @ %u): 0x%08x", f->fd, size, offset, r);
    return r;
}

static unsigned glf_Impl_Write(void *impl, const void *buf, unsigned size) {
    glf_vfile *f = IMPL_FILE(impl);
    if (!f || !size) return 0;
    int r = sceIoPwrite(f->fd, buf, size, f->pos);
    if (r <= 0) return 0;
    f->pos += r;
    return (unsigned)r;
}

/* Devuelve 1 si ok (el original: fseek(...) == 0). whence = SEEK_SET/CUR/END
 * (Open usa 2 para END, igual que libc). */
static int glf_Impl_Seek(void *impl, int off, int whence) {
    glf_vfile *f = IMPL_FILE(impl);
    if (!f) return 0;
    SceOff base;
    switch (whence) {
        case 0: base = 0; break;
        case 1: base = f->pos; break;
        case 2:
            base = sceIoLseek(f->fd, 0, SCE_SEEK_END);
            if (base < 0) return 0;
            break;
        default: return 0;
    }
    SceOff np = base + off;
    if (np < 0) return 0;
    f->pos = np;
    return 1;
}

static int glf_Impl_Tell(void *impl) {
    glf_vfile *f = IMPL_FILE(impl);
    return f ? (int)f->pos : -1;
}

void glf_filestream_patch(void) {
    static const struct { const char *sym; void *hook; } hooks[] = {
        { "_ZN3glf10FileStream4Impl4OpenEPKcNS_3ios8openflagE", (void *)&glf_Impl_Open },
        { "_ZN3glf10FileStream4Impl5CloseEv",                    (void *)&glf_Impl_Close },
        { "_ZN3glf10FileStream4Impl4ReadEPvj",                   (void *)&glf_Impl_Read },
        { "_ZN3glf10FileStream4Impl14ReadWithOffsetEPvjj",       (void *)&glf_Impl_ReadWithOffset },
        { "_ZN3glf10FileStream4Impl5WriteEPKvj",                 (void *)&glf_Impl_Write },
        { "_ZN3glf10FileStream4Impl4SeekEiNS_3ios4seekE",        (void *)&glf_Impl_Seek },
        { "_ZNK3glf10FileStream4Impl4TellEv",                    (void *)&glf_Impl_Tell },
    };

    FilePosition_SetFileSize = (void *) so_symbol(&so_mod,
        "_ZN3glf8IOStream12FilePosition11SetFileSizeEj");
    if (!FilePosition_SetFileSize) {
        l_error("[patch] glf FilePosition::SetFileSize not found: glf::FileStream left on FILE*");
        return;
    }

    for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
        uintptr_t addr = so_symbol(&so_mod, hooks[i].sym);
        if (!addr) {
            l_error("[patch] %s not found", hooks[i].sym);
            continue;
        }
        hook_addr(addr, (uintptr_t)hooks[i].hook);
    }
    l_note("[patch] glf::FileStream::Impl -> sceIo (7 methods hooked)");
}
