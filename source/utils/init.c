/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/settings.h"

#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

// Base address for the Android .so to be loaded at
#define LOAD_ADDRESS 0x98000000

extern so_module so_mod;

void soloader_init_all() {
	// Launch `app0:configurator.bin` on `-config` init param
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }

    // Set default overclock values
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0)
        l_note("FIOS initialized.");
#endif

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_note("kubridge check passed.");

    // main.so o, si se copio sin renombrar, el libgl2jni.so del APK.
    const char *so_path = SO_PATH;
    if (!file_exists(so_path) && file_exists(DATA_PATH "libgl2jni.so"))
        so_path = DATA_PATH "libgl2jni.so";
    if (!file_exists(so_path)) {
        fatal_error("Looks like you haven't installed the data files for this "
                    "port, or they are in an incorrect location. Please make "
                    "sure that you have %s file exactly at that path.", SO_PATH);
    }

    if (so_file_load(&so_mod, so_path, LOAD_ADDRESS) < 0) {
        l_fatal("SO could not be loaded.");
        fatal_error("Error: could not load %s.", so_path);
    }

    settings_load();
    l_note("Settings loaded.");

    so_relocate(&so_mod);
    l_note("SO relocated.");

    resolve_imports(&so_mod);
    l_note("SO imports resolved.");

    so_patch();
    l_note("SO patched.");

    so_flush_caches(&so_mod);
    l_note("SO caches flushed.");

    so_initialize(&so_mod);
    l_note("SO initialized.");

    gl_preload();
    l_note("OpenGL preloaded.");

    jni_init();
    l_note("FalsoJNI initialized.");
}
