/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <kubridge.h>
#include <string.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

void glf_filestream_patch(void);

/*
 * glf::Console::PrintRaw(const char*) @ 0x905024 (ARM): sumidero final de
 * glf::Console::Print/Println/VPrint/VPrintln (via ConsoleScopeBuffer). En
 * Android iba a logcat sin pasar por __android_log_*, asi que sin este hook
 * TODO el log propio del motor ("GS3: JNI_OnLoad called.", "profile name",
 * "trying to set context", ...) era invisible.
 */
static void glf_Console_PrintRaw_hook(const char *s) {
    if (!s) return;
    char line[1024];
    size_t n = strnlen(s, sizeof(line) - 1);
    memcpy(line, s, n);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    line[n] = '\0';
    if (n > 0) l_note("[GLF] %s", line);
}

/*
 * Kernel user helpers de Linux/ARM (Documentation/arm/kernel_user_helpers).
 * El libgcc estatico del NDK (linux-atomic.c, todos los __sync_* del .so,
 * bloque 0x3866a4-0x387554) no usa ldrex/strex: carga 0xffff0fc0
 * (__kuser_cmpxchg) o 0xffff0fa0 (__kuser_memory_barrier) de un literal pool
 * y hace `blx`. Android mapea esa pagina; la Vita no -> prefetch abort con
 * PC = 0xffff0fc0 (dump 1790968518, desde glf::SpinLock::Lock en
 * ThreadMgr::Add, dentro de getViewSettings). Hay 43 + 4 literales y ningun
 * __kuser_get_tls (0xffff0fe0) ni __kuser_helper_version (0xffff0ffc).
 *
 * Fix: reescribir esos literales para que apunten a reimplementaciones con
 * el MISMO contrato que el kernel (no una funcion C: AAPCS pisaria r1/r2):
 *   cmpxchg: in r0=oldval r1=newval r2=ptr; out r0=0 y C=1 si intercambio,
 *            r0!=0 y C=0 si no; solo clobbera r3, ip y flags.
 *   memory_barrier: sin args, no clobbera nada.
 * Codigo ARM (bit 0 del puntero en 0): los call sites hacen `blx rN`.
 */
__attribute__((naked, target("arm"), used))
static void kuser_cmpxchg(void) {
    __asm__ volatile(
        "dmb ish\n"
        "1:\n"
        "ldrex r3, [r2]\n"
        "subs r3, r3, r0\n"
        "strexeq r3, r1, [r2]\n"
        "teqeq r3, #1\n"
        "beq 1b\n"
        "rsbs r0, r3, #0\n"
        "dmb ish\n"
        "bx lr\n");
}

__attribute__((naked, target("arm"), used))
static void kuser_memory_barrier(void) {
    __asm__ volatile(
        "dmb ish\n"
        "bx lr\n");
}

#define KUSER_CMPXCHG        0xffff0fc0u
#define KUSER_MEMORY_BARRIER 0xffff0fa0u
#define KUSER_GET_TLS        0xffff0fe0u

static void patch_kuser_helpers(void) {
    const uint32_t cmpxchg = (uint32_t)(uintptr_t)&kuser_cmpxchg;
    const uint32_t barrier = (uint32_t)(uintptr_t)&kuser_memory_barrier;
    uint32_t *p = (uint32_t *)so_mod.text_base;
    uint32_t *end = (uint32_t *)(so_mod.text_base + so_mod.text_size);
    int n_cmpxchg = 0, n_barrier = 0, n_tls = 0;

    for (; p < end; p++) {
        const uint32_t *src;
        if (*p == KUSER_CMPXCHG) {
            src = &cmpxchg;
            n_cmpxchg++;
        } else if (*p == KUSER_MEMORY_BARRIER) {
            src = &barrier;
            n_barrier++;
        } else {
            if (*p == KUSER_GET_TLS) n_tls++;
            continue;
        }
        /* .text esta en un bloque RX: escribir via kubridge. El flush de
         * caches lo hace so_flush_caches() despues de so_patch(). */
        kuKernelCpuUnrestrictedMemcpy(p, src, sizeof(*p));
    }

    l_note("[patch] kuser helpers: %d cmpxchg + %d memory_barrier literals redirected",
           n_cmpxchg, n_barrier);
    if (n_cmpxchg != 43 || n_barrier != 4)
        l_error("[patch] kuser helpers: expected 43 + 4 literals (libgl2jni 1.2.2b)");
    if (n_tls)
        l_error("[patch] kuser helpers: %d __kuser_get_tls literals NOT patched", n_tls);
}


void so_patch(void) {
    patch_kuser_helpers();

    /* Antes de todo uso del Fs: la capa de archivos de glf va por sceIo
     * (ver glf_filestream.c). */
    glf_filestream_patch();

    uintptr_t addr = so_symbol(&so_mod, "_ZN3glf7Console8PrintRawEPKc");
    if (addr) {
        hook_addr(addr, (uintptr_t)&glf_Console_PrintRaw_hook);
        l_note("[patch] glf::Console::PrintRaw hooked @ %p", (void *)addr);
    } else {
        l_error("[patch] glf::Console::PrintRaw not found");
    }
}
