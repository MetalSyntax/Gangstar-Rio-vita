#ifndef SOLOADER_VIDEO_H
#define SOLOADER_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

// Video intro por SceAvPlayer (H.264 + AAC), copiado del port hermano
// Shadow Guardian (mismo formato de logo: h264 480x270 30fps + AAC 44.1k,
// confirmado en consola alli). Carga el sysmodule AVPLAYER. Llamar una vez,
// despues de gl_init(): el allocator de texturas de video mapea memoria con
// sceGxmMapMemory, que necesita el contexto GXM de vitaGL.
void video_init(void);

void video_shutdown(void);

// Reproduce `name` (relativo a DATA_PATH, o a DATA_PATH"files/") a pantalla
// completa, bloqueando el hilo llamador hasta que termina, el usuario lo
// saltea (Cross/Start) o falla al abrir/inicializar: SIEMPRE retorna.
void video_play(const char *name);

#ifdef __cplusplus
}
#endif

#endif // SOLOADER_VIDEO_H
