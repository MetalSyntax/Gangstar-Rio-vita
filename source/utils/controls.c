/**
 * @file  controls.c
 * @brief Vita front touch + buttons + sticks -> Gangstar Rio input JNI.
 *
 * Todo lo de aca sale del .so real (libgl2jni.so, pseudo-C de Ghidra
 * out_ghidra.c; direcciones Ghidra = vaddr + 0x10000):
 *
 * 1. touchEvent(env, cls, action, x, y, id) usa la convencion GAMELOFT, no
 *    la de MotionEvent: glf::AndroidAddTouchEvent (out_ghidra.c:6339) mapea
 *    0 -> 0xd8, 1 -> 0xd6, 2 -> 0xd7, y TouchScreenBase (out_ghidra.c:336714)
 *    trata 0xd6 = began (vtable +0x20), 0xd7 = moved (+0x24), 0xd8 = ended
 *    (+0x28). O sea 0 = UP, 1 = DOWN, 2 = MOVE (igual que el hermano Miami).
 *
 * 2. Ese mismo touchEvent con action >= 3 es el touchpad del Xperia Play
 *    (pad fisico de 966x360): x <= 360 -> pad izquierdo (actions 3/4/5 =
 *    up/down/move -> 0xde/0xdc/0xdd, fuente 1), x >= 606 -> pad derecho
 *    (+3 -> 0xdb/0xd9/0xda, fuente 2). glf::tx/ty (out_ghidra.c:4949) los
 *    recentran sobre centerX_L/centerY_L: el centro del pad es (180, 180).
 *    El stick izquierdo de la Vita se emula como un dedo en ese pad, que es
 *    lo que hacia el Java de MOGA/PowerA (su nativeSetPowerALeftJoystick es
 *    un `bx lr` vacio, 0x547b74).
 *
 * 3. El stick derecho va por la ruta analogica nativa PowerA:
 *    nativeSetPowerARightJoystick(env, cls, float x, float y) @ 0x547b78
 *    guarda x/y en glf::App +0x10/+0x14 y levanta 0xd9/0xda/0xdb con los
 *    floats crudos y el flag "normalizado" = 1. Floats por r2/r3 (softfp).
 *
 * 4. Botones -> keyEvent(env, cls, keycode, down). keyEvent prueba primero
 *    transfer_POWER_A_toGLF (tabla en vaddr 0xc8ff08) y si no, BACK/MENU.
 *    La tabla es la del Xperia Play, que tenia los mismos botones que la
 *    Vita: DPAD 19-22, Cruz = DPAD_CENTER 23, Cuadrado = BUTTON_X 99,
 *    Triangulo = BUTTON_Y 100, L1 102, R1 103, START 108, SELECT 109; Circulo
 *    era KEYCODE_BACK (4 -> 0x36). El esquema de mando (XPERIAisSTU,
 *    isKeyEnabled) se activa solo cuando GloftG4HM.IsPowerAConnected() != 0
 *    (out_ghidra.c:35587/37465) -- ver Method_IsPowerAConnected en java.c.
 *
 * TODO consola: confirmar sentido/escala de ambos sticks y que el HUD en
 * modo mando no tape nada; el touch frontal sigue activo para menus.
 */

#include "utils/controls.h"

#include "utils/dialog.h"
#include "utils/logger.h"

#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

extern so_module so_mod;

typedef void (*fn_touch)(JNIEnv *, jclass, jint, jint, jint, jint);
typedef void (*fn_key)(JNIEnv *, jclass, jint, jint);
typedef void (*fn_rstick)(JNIEnv *, jclass, jfloat, jfloat);

static fn_touch  GL2JNILib_touchEvent;
static fn_key    GL2JNILib_keyEvent;
static fn_rstick GL2JNILib_nativeSetPowerARightJoystick;

/* Convencion Gameloft (ver punto 1). */
#define TOUCH_UP   0
#define TOUCH_DOWN 1
#define TOUCH_MOVE 2
/* Touchpad Xperia (punto 2): mismas tres acciones desplazadas en 3. */
#define PAD_UP     3
#define PAD_DOWN   4
#define PAD_MOVE   5

#define ENGINE_W 960
#define ENGINE_H 544

/* 4 dedos en el frontal (IDs 0-3) + pad izquierdo (ID 4). TouchScreenBase
 * tiene 8 slots (getID, out_ghidra.c:336490). Nunca pasar el id crudo de
 * SceTouchReport: es un contador que crece sin limite. */
#define MAX_FRONT_SLOTS 4
#define LPAD_ID 4

/* Pad izquierdo Xperia: 0..360 en ambos ejes, centro (180,180). Radio un
 * poco menor que la mitad para no tocar el borde (x > 360 cae en la zona
 * muerta 361..605 que touchEvent descarta). */
#define LPAD_CENTER 180
#define LPAD_RADIUS 150

#define STICK_DEADZONE 28 /* sobre 128 */

static int front_hw_id[MAX_FRONT_SLOTS];
static int front_last_x[MAX_FRONT_SLOTS];
static int front_last_y[MAX_FRONT_SLOTS];

static int lpad_active = 0;
static int lpad_last_x = 0, lpad_last_y = 0;
static int rstick_active = 0;
static uint32_t old_buttons = 0;

static const struct {
    uint32_t btn;
    int keycode;
} keymap[] = {
    { SCE_CTRL_UP,       19 },  /* KEYCODE_DPAD_UP     -> glf 0x24 */
    { SCE_CTRL_DOWN,     20 },  /* KEYCODE_DPAD_DOWN   -> glf 0x26 */
    { SCE_CTRL_LEFT,     21 },  /* KEYCODE_DPAD_LEFT   -> glf 0x27 */
    { SCE_CTRL_RIGHT,    22 },  /* KEYCODE_DPAD_RIGHT  -> glf 0x25 */
    { SCE_CTRL_CROSS,    23 },  /* KEYCODE_DPAD_CENTER -> glf 0x20 */
    { SCE_CTRL_CIRCLE,    4 },  /* KEYCODE_BACK        -> glf 0x36 */
    { SCE_CTRL_SQUARE,   99 },  /* KEYCODE_BUTTON_X    -> glf 0x0f */
    { SCE_CTRL_TRIANGLE, 100 }, /* KEYCODE_BUTTON_Y    -> glf 0x0e */
    { SCE_CTRL_LTRIGGER, 102 }, /* KEYCODE_BUTTON_L1   -> glf 0x21 */
    { SCE_CTRL_RTRIGGER, 103 }, /* KEYCODE_BUTTON_R1   -> glf 0x0c */
    { SCE_CTRL_START,    108 }, /* KEYCODE_BUTTON_START  -> glf 0x10 */
    { SCE_CTRL_SELECT,   109 }, /* KEYCODE_BUTTON_SELECT -> glf 0x52 */
};

#define RESOLVE(dst, sym) \
    dst = (void *) so_symbol(&so_mod, sym); \
    if (!dst) fatal_error("Could not resolve required symbol: %s", sym);

void controls_init(void) {
    RESOLVE(GL2JNILib_touchEvent, "Java_com_gameloft_android_ANMP_GloftG4HM_GL2JNILib_touchEvent")
    RESOLVE(GL2JNILib_keyEvent, "Java_com_gameloft_android_ANMP_GloftG4HM_GL2JNILib_keyEvent")
    RESOLVE(GL2JNILib_nativeSetPowerARightJoystick,
            "Java_com_gameloft_android_ANMP_GloftG4HM_GL2JNILib_nativeSetPowerARightJoystick")

    for (int s = 0; s < MAX_FRONT_SLOTS; s++) {
        front_hw_id[s] = -1;
        front_last_x[s] = -1;
        front_last_y[s] = -1;
    }

    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
}

static void poll_front_touch(void) {
    SceTouchData touch;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) < 1)
        touch.reportNum = 0;

    int seen[MAX_FRONT_SLOTS] = {0};
    for (int r = 0; r < (int)touch.reportNum; r++) {
        int hw = touch.report[r].id;
        /* Panel frontal: 1920x1088 -> viewport del motor. */
        int x = (int)touch.report[r].x * ENGINE_W / 1920;
        int y = (int)touch.report[r].y * ENGINE_H / 1088;
        if (x < 0) x = 0; else if (x >= ENGINE_W) x = ENGINE_W - 1;
        if (y < 0) y = 0; else if (y >= ENGINE_H) y = ENGINE_H - 1;

        int slot = -1;
        for (int s = 0; s < MAX_FRONT_SLOTS; s++)
            if (front_hw_id[s] == hw) { slot = s; break; }
        if (slot == -1) {
            for (int s = 0; s < MAX_FRONT_SLOTS; s++)
                if (front_hw_id[s] == -1) { slot = s; break; }
            if (slot == -1) continue;
            front_hw_id[slot] = hw;
            front_last_x[slot] = -1;
        }
        seen[slot] = 1;

        if (front_last_x[slot] == -1)
            GL2JNILib_touchEvent(&jni, NULL, TOUCH_DOWN, x, y, slot);
        else if (front_last_x[slot] != x || front_last_y[slot] != y)
            GL2JNILib_touchEvent(&jni, NULL, TOUCH_MOVE, x, y, slot);
        front_last_x[slot] = x;
        front_last_y[slot] = y;
    }

    for (int s = 0; s < MAX_FRONT_SLOTS; s++) {
        if (front_hw_id[s] != -1 && !seen[s]) {
            GL2JNILib_touchEvent(&jni, NULL, TOUCH_UP, front_last_x[s], front_last_y[s], s);
            front_hw_id[s] = -1;
            front_last_x[s] = -1;
        }
    }
}

static int stick_axis(unsigned char raw) {
    int v = (int)raw - 128;
    if (v > -STICK_DEADZONE && v < STICK_DEADZONE) return 0;
    return v;
}

static void poll_left_stick(const SceCtrlData *pad) {
    int dx = stick_axis(pad->lx);
    int dy = stick_axis(pad->ly);

    if (dx == 0 && dy == 0) {
        if (lpad_active) {
            GL2JNILib_touchEvent(&jni, NULL, PAD_UP, lpad_last_x, lpad_last_y, LPAD_ID);
            lpad_active = 0;
        }
        return;
    }

    int x = LPAD_CENTER + dx * LPAD_RADIUS / 128;
    int y = LPAD_CENTER + dy * LPAD_RADIUS / 128;
    if (x < 0) x = 0; else if (x > 2 * LPAD_CENTER) x = 2 * LPAD_CENTER;
    if (y < 0) y = 0; else if (y > 2 * LPAD_CENTER) y = 2 * LPAD_CENTER;

    if (!lpad_active) {
        /* Apoyar en el centro y despues desplazar: un joystick virtual mide
         * el offset desde donde empezo el toque. */
        GL2JNILib_touchEvent(&jni, NULL, PAD_DOWN, LPAD_CENTER, LPAD_CENTER, LPAD_ID);
        lpad_active = 1;
    }
    GL2JNILib_touchEvent(&jni, NULL, PAD_MOVE, x, y, LPAD_ID);
    lpad_last_x = x;
    lpad_last_y = y;
}

static void poll_right_stick(const SceCtrlData *pad) {
    int dx = stick_axis(pad->rx);
    int dy = stick_axis(pad->ry);

    if (dx == 0 && dy == 0) {
        /* Un solo (0,0) para que el nativo levante 0xdb (release). */
        if (rstick_active) {
            GL2JNILib_nativeSetPowerARightJoystick(&jni, NULL, 0.0f, 0.0f);
            rstick_active = 0;
        }
        return;
    }

    float fx = (float)dx / 127.0f;
    float fy = (float)dy / 127.0f;
    if (fx > 1.0f) fx = 1.0f; else if (fx < -1.0f) fx = -1.0f;
    if (fy > 1.0f) fy = 1.0f; else if (fy < -1.0f) fy = -1.0f;
    GL2JNILib_nativeSetPowerARightJoystick(&jni, NULL, fx, fy);
    rstick_active = 1;
}

void controls_poll(void) {
    poll_front_touch();

    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);

    for (unsigned k = 0; k < sizeof(keymap) / sizeof(keymap[0]); k++) {
        uint32_t b = keymap[k].btn;
        if ((pad.buttons & b) && !(old_buttons & b))
            GL2JNILib_keyEvent(&jni, NULL, keymap[k].keycode, 1);
        else if (!(pad.buttons & b) && (old_buttons & b))
            GL2JNILib_keyEvent(&jni, NULL, keymap[k].keycode, 0);
    }
    old_buttons = pad.buttons;

    poll_left_stick(&pad);
    poll_right_stick(&pad);
}
