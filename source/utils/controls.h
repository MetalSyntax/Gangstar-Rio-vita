/**
 * @file  controls.h
 * @brief Vita front touch + buttons + sticks -> Gangstar Rio input JNI.
 *
 * Ver controls.c para el mapeo y el RE que lo respalda.
 */

#ifndef SOLOADER_CONTROLS_H
#define SOLOADER_CONTROLS_H

#ifdef __cplusplus
extern "C" {
#endif

/** Resuelve los Java_* de input y arranca el muestreo tactil. */
void controls_init(void);

/** Lee touch/pad una vez y despacha los eventos al motor (llamar por frame). */
void controls_poll(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_CONTROLS_H
