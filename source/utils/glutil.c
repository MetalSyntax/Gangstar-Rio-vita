/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

#ifdef USE_GLSL_SHADERS
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
#endif
}

void gl_init() {
    // Receta confirmada en hardware por el port hermano Miami (mismo motor
    // glitch, port_progress.md Fases 29/60/64 de ese repo): pool circular de
    // 64 MB para que las copias de vertex arrays por draw no desborden a
    // gpu_alloc por reserva, sin MSAA (cada swap pagaba un resolve y MSAA +
    // FBOs es riesgoso en vitaGL) y threshold de 8 MB. Requiere el heap de
    // newlib en 192 MB (main.c); con 256 MB el pool #2 se derramaba a VRAM.
    vglSetCircularPoolSize(64 * 1024 * 1024);
    vglInitExtended(0, 960, 544, 8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);

    // ETC1 texture compression handling (Glitch engine / DH3 recipe):
    // Con DISABLE_HW_ETC1 vitaGL decodifica ETC1 en CPU; NICEST le pide a
    // vitaGL recomprimir a DXT1 (4 bpp) en lugar de inflar a RGB 24-bit (x6 memoria).
    // fast_texture_compression mantiene stb_dxt en velocidad normal para no pausar cargas.
    glHint(GL_TEXTURE_COMPRESSION_HINT, GL_NICEST);
    extern GLboolean fast_texture_compression;
    fast_texture_compression = GL_TRUE;

#ifdef DUMP_COMPILED_SHADERS
    // Cache de shaders compilados (load_shader): sin el directorio cada
    // file_save fallaba (log_008) y cada arranque recompilaba todo.
    sceIoMkdir(DATA_PATH "gxp", 0777);
    sceIoMkdir(DATA_PATH "cg", 0777);
#endif
}

void gl_swap() {
    vglSwapBuffers(GL_FALSE);
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glShaderSource<%p>(shader: %i, count: %i, string: %p, length: %p)\n", __builtin_return_address(0), shader, count, string, _length);
#endif
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            total_length += strlen(string[i]);
        } else {
            total_length += _length[i];
        }
    }

    char * str = malloc(total_length+1);
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], _length[i]);
            l += _length[i];
        }
    }
    str[total_length] = '\0';

    load_shader(shader, str, total_length);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        glCompileShader(shader);
#ifdef DUMP_COMPILED_SHADERS
        // Solo cachear binarios reales. Si la compilacion fallo,
        // vglGetShaderBinary deja len en 0 y el file_save creaba un .gxp
        // VACIO que en el siguiente arranque daba file_exists=true +
        // file_load=false (ignorado) y glShaderBinary con puntero/tamano
        // basura -> data abort en unserialize_shader (dump 1790991355,
        // PC SceLibKernel+0x38, pila: load_shader -> glShaderBinary ->
        // unserialize_shader -> memcpy del kernel). Ademas
        // next_shader_fname puede estar vacio si todavia no hubo ningun
        // cache-miss en esta sesion.
        if (next_shader_fname[0] != '\0') {
            void *bin = vglMalloc(32 * 1024);
            GLsizei len = 0;
            vglGetShaderBinary(shader, 32 * 1024, &len, bin);
            if (len > 0) {
                file_save(next_shader_fname, bin, (size_t)len);
            } else {
                l_warn("gl: not caching shader %s (compile failed or empty binary)",
                       next_shader_fname);
            }
            vglFree(bin);
        }
#endif
    }
    skip_next_compile = GL_FALSE;
#endif
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);

    // OJO: file_load puede fallar (cache ausente, vacia o corrupta) dejando
    // buffer/size sin setear. La version anterior ignoraba el retorno y
    // llamaba glShaderBinary con basura -> data abort en unserialize_shader
    // (dump 1790991355). Los .gxp de 0 bytes los dejo el bug de
    // glCompileShader_soloader (guardaba len==0); se borran para no
    // envenenar el proximo arranque y se compila desde fuente.
    uint8_t *buffer = NULL;
    size_t size = 0;

    if (file_load(gxp_path, &buffer, &size) && size > 0) {
        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else {
        free(buffer); // NULL-safe: file_load solo lo setea en exito
        if (file_exists(gxp_path)) {
            sceIoRemove(gxp_path);
            l_warn("gl: removed empty/unreadable shader cache %s, compiling from source",
                   gxp_path);
        }
        glShaderSource(shader, 1, &string, &length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);
    snprintf(cg_path, sizeof(cg_path), DATA_PATH"cg/%s.cg", sha_name);

    // Misma guardia que en la variante GLSL de arriba: file_load puede
    // fallar (cache vacia/corrupta) dejando buffer/size sin setear; en ese
    // caso se borra el archivo envenenado y se sigue con la siguiente
    // fuente (cache .cg, o compilar desde fuente).
    uint8_t *gxp_buffer = NULL;
    size_t gxp_size = 0;
    char *cg_buffer = NULL;
    size_t cg_size = 0;

    if (file_load(gxp_path, &gxp_buffer, &gxp_size) && gxp_size > 0) {
        glShaderBinary(1, &shader, 0, gxp_buffer, (int32_t) gxp_size);

        free(gxp_buffer);
        skip_next_compile = GL_TRUE;
    } else if (file_load(cg_path, (uint8_t **) &cg_buffer, &cg_size) && cg_size > 0) {
        free(gxp_buffer); // NULL-safe
        glShaderSource(shader, 1, &string, (const GLint *) &cg_size);
        strcpy(next_shader_fname, gxp_path);

        free(cg_buffer);
        skip_next_compile = GL_FALSE;
    } else {
        free(gxp_buffer); // NULL-safe
        free(cg_buffer);  // NULL-safe
        // Cache vacia o corrupta: borrar para no envenenar el proximo
        // arranque. En modo CG no se puede compilar el GLSL original
        // (el pipeline espera Cg), asi que se usa el dummy como antes.
        if (file_exists(gxp_path)) {
            sceIoRemove(gxp_path);
            l_warn("gl: removed empty/unreadable shader cache %s", gxp_path);
        }
        if (file_exists(cg_path)) {
            sceIoRemove(cg_path);
            l_warn("gl: removed empty/unreadable shader cache %s", cg_path);
        }
        // Sin cache valida de ningun tipo: guardar el GLSL para traducirlo
        // offline y usar un dummy esta sesion (igual que antes).
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), DATA_PATH"cg/%s.cg", sha_name);
#else
    snprintf(path, sizeof(path), DATA_PATH"gxp/%s.gxp", sha_name);
#endif

    // Guardia anti-cache-envenenada (ver variante GLSL arriba): si el
    // archivo existe pero no se puede leer, se borra y se usa el dummy en
    // vez de pasar basura a GL.
    uint8_t *cache_buffer = NULL;
    size_t cache_size = 0;
    bool cache_ok = file_load(path, &cache_buffer, &cache_size) && cache_size > 0;
    if (!cache_ok) {
        free(cache_buffer); // NULL-safe
        if (file_exists(path)) {
            sceIoRemove(path);
            l_warn("gl: removed empty/unreadable shader cache %s", path);
        }
    }

    if (cache_ok) {
#ifdef USE_CG_SHADERS
        glShaderSource(shader, 1, &string, (const GLint *) &cache_size);

        free(cache_buffer);
#else
        glShaderBinary(1, &shader, 0, cache_buffer, (int32_t) cache_size);

        free(cache_buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif
