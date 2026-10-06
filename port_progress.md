# Registro de Progreso — Gangstar Rio (PS Vita)

## Estado actual — 2026-10-06 (Fase 23: Eliminación de speedhacks inseguros causantes de congelamiento total en nueva partida, build OK)

Build OK (release). El análisis de `log_020.log` mostró que el menú principal funcionó a 60 FPS estables y con texturas correctas tras la corrección de Fase 21/22. Sin embargo, al pulsar "Nueva Partida" e ingresar a la carga pesada del mundo 3D abierto de Río de Janeiro (frame 1183, step de 35.328 ms), la consola se congeló por completo sin generar volcado de crash (`.psp2dmp`), señal inequívoca de un bloqueo duro a nivel de GPU (SceGxm/SGX543) o del bus de memoria.

La investigación cruzada con los ports de referencia (`Dungeon-Hunter-3-Vita`, `9mm-vita`, `asphalt8-vita-main`) identificó los culpables:
1. **`CIRCULAR_POOL_SPEEDHACK`**: Convierte el búfer circular de vitaGL en un único búfer global. Cuando la reserva desborda el límite, en lugar de asignar memoria mapeada para CPU de forma segura, reinicia el puntero inmediatamente al inicio del pool (`circular_data_pool_ptr = circular_data_pool + size`). En un juego de mundo abierto masivo como Gangstar Rio (con un pool de 64 MB reservado en `gl_init`), hilos de carga en segundo plano sobrescriben los vértices y comandos que el rasterizador/tiler de la GPU SGX543 todavía está procesando en el fotograma anterior, provocando un bloqueo duro del hardware gráfico que congela el sistema operativo de la PS Vita.
2. **`SAFER_DRAW_SPEEDHACK` (`DRAW_SPEEDHACK=2`)**: Si `count * stride > 0x8000` (32 KB), este flag pasa punteros crudos del espacio de usuario directamente a la GPU (`ptrs[i] = (void *)cur_vao->vertex_attrib_offsets[...]`) omitiendo la copia a memoria GPU mapeada (`gpu_alloc_mapped_temp`). Dado que los vértices dinámicos de Gangstar Rio residen en el heap de newlib, la MMU de SceGxm genera un fallo de bus fatal al intentar acceder a memoria no mapeada para el rasterizador.

**Solución aplicada**:
- Se eliminaron `CIRCULAR_POOL_SPEEDHACK` y `SAFER_DRAW_SPEEDHACK` de `CMakeLists.txt` en `vitaGL_local`.
- Se preservaron las optimizaciones seguras y estables: `SAMPLERS_SPEEDHACK`, `MATH_SPEEDHACK`, `DISABLE_HW_ETC1`, `HAVE_SOFTFP_ABI` y `SKIP_SPLASHSCREEN`.
- Compilado y empaquetado `build/gangstarrio.vpk` (2.2 MB) y `build/gangstarrio.elf` (12 MB) listo para probar en hardware.

---

## Fase 23 — 2026-10-06: Diagnóstico de congelamiento total al iniciar nueva partida

**Evidencia:** `logs/log_020.log` (congelamiento en frame 1183 tras step de 35.328 ms al pulsar nueva partida, sin archivo `.psp2dmp`) + auditoría de vitaGL en `CMakeLists.txt`.

### Diagnóstico
1. **Comportamiento en log_020:**
   - Inicio fluido, menús y tutorial a 60 FPS estables.
   - En el frame 1181 y frame 1183, el juego inicia la carga de la partida (`LEVEL UP`, carga de scripts y texturas del mundo 3D).
   - Frame 1183 tarda 35.328 ms en completar la carga pesada de activos.
   - Tras este paso, la PS Vita se congeló totalmente requiriendo reinicio forzado manteniendo pulsado el botón de encendido. No hubo volcado `.psp2dmp`.
2. **Causa del congelamiento:**
   - La ausencia de volcado de crash de SceLibKernel confirma que no fue una excepción de CPU (Data Abort / Prefetch Abort / SIGSEGV). Fue un GPU lockup / MMU bus fault.
   - En Fase 22 se introdujeron `CIRCULAR_POOL_SPEEDHACK` y `SAFER_DRAW_SPEEDHACK`.
   - `CIRCULAR_POOL_SPEEDHACK` recicla el pool de 64 MB inmediatamente desde el byte 0 sin esperar a que la GPU termine de rasterizar la escena del fotograma previo.
   - `SAFER_DRAW_SPEEDHACK` pasa búferes mayores de 32 KB directo a SceGxm sin copiar a memoria asignada para GPU (`gpu_alloc_mapped_temp`). Al ser memoria de newlib sin mapeo en SceGxm, el controlador SGX bloquea el bus de memoria.

### Solución aplicada
1. Se retiraron ambos speedhacks incompatibles con el streaming dinámico de Gangstar Rio en `CMakeLists.txt`.
2. Recompilación limpia y generación del nuevo VPK.

---

## Fase 22 — 2026-10-06: Crash en realloc de mipmaps en CDRAM y optimizaciones de rendimiento

**Evidencia:** `logs/log_019.log` (1.238 frames, 334 segundos, 60 FPS en menús) + `logs/gangstarrio-psp2core-1791258841-0x000de92a3d-eboot.bin.psp2dmp` + cross-reference con `Dungeon-Hunter-3-Vita`, `asphalt8-vita-main` y `9mm-vita`.

### Diagnóstico
1. **Comportamiento en log_019:**
   - Menú del juego corriendo a 58-60 FPS sostenidos (frames 360, 653, 901, 1160).
   - El juego ejecutó hasta el frame 1238 (334 segundos de ejecución).
   - Al seleccionar inicio de juego/misión, el motor lanza la carga pesada de geometría y texturas de la ciudad en hilos trabajadores en segundo plano.
2. **Causa del Data abort en `0xe0006b68`:**
   - El hilo `pthread` `0x40030245` abortó en `0xe0006b68` (SceLibKernel).
   - Registro `R4 = 0x62000010` (cabecera del mutex del `sceClibMspace` de CDRAM).
   - Instrucción que falla: `strex r2, r3, [r4]`.
   - La textura base era de 1024x1024 DXT1. Con niveles 0, 1 y 2 acumulaba 688.128 bytes (`0xA8000`). Al cargar el nivel 3 (128x128, 8 KB adicionales), el tamaño total ascendía a 696.320 bytes (`0xAA000`).
   - Upstream vitaGL llamaba a `vgl_realloc(tex->data, tex_size)` que invoca `sceClibMspaceRealloc` en CDRAM. Al no poder extender el bloque in-situ y tratarse de memoria CDRAM no cacheada (`0x09408060`), las primitivas exclusivas de ARM provocaban la excepción de aborto de datos del procesador.

### Solución aplicada
1. **Reasignación segura sin `sceClibMspaceRealloc` en CDRAM (`gpu_utils.c`):**
   - En `gpu_alloc_compressed_texture` y `gpu_alloc_mipmaps`, se reemplazó la llamada arriesgada a `vgl_realloc` por la asignación directa de un nuevo búfer con `gpu_alloc_mapped_for_gpu(tex_size)`.
   - Se copian los datos acumulados hasta el momento con `vgl_memcpy(texture_data, tex->data, old_data_size)`.
   - El búfer viejo se delega de forma asíncrona y segura al garbage collector vía `gpu_free_texture_data(tex)`.
2. **Optimizaciones de velocidad (Speedhacks):**
   - Se activaron en `CMakeLists.txt` para `vitaGL_local`:
     - `SAMPLERS_SPEEDHACK`: Evita llamadas redundantes a cambios de estado de muestreadores de texturas.
     - `MATH_SPEEDHACK`: Utiliza funciones matemáticas optimizadas para Neon/Cortex-A9.
     - `CIRCULAR_POOL_SPEEDHACK`: Optimiza las reservas continuas de vértices en el búfer circular de 64 MB.
     - `SAFER_DRAW_SPEEDHACK`: Optimiza el pipeline de dibujo de primitivas manteniendo estabilidad.

---

## Fase 21 — 2026-10-05: Corrección de texturas en modelos CPU-skinned y corrección de mipmaps ETC1

**Evidencia:** `logs/log_017.log` + `logs/gangstarrio-psp2core-1791257174-0x00086b38dd-eboot.bin.psp2dmp` + cross-reference con `Dungeon-Hunter-3-Vita`.

### Diagnóstico
1. **Personaje sin texturas ("maniquí"):**
   - El motor gráfico Glitch de Gameloft implementa skinning por software en la CPU para personajes y mallas dinámicas.
   - En este proceso, genera un buffer continuo donde solo Vertex y Normal están intercalados (interleaved), mientras que las coordenadas UV (`texcoord0`) provienen de un buffer o puntero separado.
   - vitaGL por defecto solo comparaba los offsets de los dos primeros atributos (`attr_map[0]` y `attr_map[1]`) con el stride. Al coincidir el stride con la distancia Vertex-Normal, vitaGL asumía erróneamente que **todo** el layout de vértices (incluyendo TexCoord0) estaba intercalado en un solo stream. Como resultado, las coordenadas de textura se leían de direcciones incorrectas de memoria de vídeo, renderizando el personaje sin su textura.
2. **Crash al cargar mips > 0 en texturas ETC1:**
   - En `log_017.log` el hilo `pthread` abortó con Data abort en `0xe0006476` (SceLibKernel) sobre `vgl_realloc` en `lib/vitaGL/source/utils/gpu_utils.c:702`.
   - VitaGL en `textures.c` (~línea 1661) recibía mipmaps con decodificador de CPU activo (`read_cb`), pero pasaba la data cruda sin empaquetar a 4x4 (los mips finales menores a 4 px como 2x2 o 1x1 generaban 0 bloques para `dxt_compress`, corrompiendo el tamaño de reasignación y el heap de memoria GPU).

### Solución aplicada
1. **Comprobación completa de atributos intercalados (`custom_shaders.c`):**
   - Se añadió `dh3_attribs_interleaved(program *p, uint32_t stride)` que itera por todos los atributos habilitados en el VAO y verifica que caigan estrictamente dentro del rango de offset base + stride.
   - Se aplicó esta comprobación en `glDrawArrays`, `glDrawElements` y `glDrawRangeElements`.
2. **Padding y conversión RGBA8888 en mipmaps de texturas comprimidas (`textures.c`):**
   - En `textures.c`, los niveles `level > 0` con `read_cb` ahora asignan un búfer RGBA con dimensiones acolchadas a mínimo 4x4 (`pad_w = width < 4 ? 4 : width`, `pad_h = height < 4 ? 4 : height`), convirtiendo los píxeles a RGBA8888 antes de pasarlos a `gpu_alloc_compressed_texture`.
3. **Pipeline ETC1 a DXT1:**
   - En `CMakeLists.txt` se agregó `DISABLE_HW_ETC1` a las definiciones de compilación de `vitaGL_local`.
   - En `source/utils/glutil.c` se configuró `glHint(GL_TEXTURE_COMPRESSION_HINT, GL_NICEST)` y `fast_texture_compression = GL_TRUE`.

---

## Fase 20 — 2026-10-05: Sincronización en userspace (Asphalt 8 architecture) — Eliminación total de semáforos de kernel y ENOSPC 0x1c

**Evidencia:** `logs/log_016.log` + `logs/gangstarrio-psp2core-1791254369-0x0009cc305d-eboot.bin.psp2dmp` + análisis de notas de core dump (`SEMAPHORE_INFO` 1.959).

### Diagnóstico
- En `log_016.log`:
  - `pthr: mutex live=256 (ok=257 fail=0 destroyed=1)`
  - `pthr: mutex live=512 (ok=513 fail=0 destroyed=1)`
  - `pthr: mutex live=1024 (ok=1031 fail=0 destroyed=7)` durante `CreateDataHandles`.
  - Al entrar al title (frame 1520):
    `cond initialization for 0x862908f8 (attr=0x0) has failed: 0x1c (conds live~13 ok=15 fail=1 destroyed=2)`
    `cond initialization for 0x8f1008f8 (attr=0x0) has failed: 0x1c`
- En el core dump:
  - 1.959 semáforos totales en el proceso (límite del kernel de Vita). 1.941 creados por PTE con `maxCount = 32767`.
  - El hilo trabajador `pthread` (0x4002037f) abortó en `0xe0006ba4` (SceLibKernel + 0x3854) con Data abort sobre stack `0x8f1006e0` al interactuar con el handshake roto.

### Solución aplicada
1. **Mutexes en Userspace (`userspace_mutex`):**
   - Estructura ligera de 16 bytes: `owner`, `depth`, `waiters`, `kind`.
   - Adquisición lock-free mediante `__atomic_compare_exchange_n(&entry->owner, ...)`.
   - Soporte para mutex normal, recursivo y errorcheck.
   - Si hay contención, espera con backoff exponencial vía `sceKernelDelayThread(wait_us)` (50 µs a 1 ms).
   - Liberación atómica directa. Cero llamadas a syscalls de creación o destrucción de primitivas de kernel.
2. **Variables de Condición en Userspace (`userspace_cond`):**
   - Estructura de 8 bytes: `sequence` atómico y `waiters`.
   - `pthread_cond_signal` y `broadcast` incrementan `sequence` con barrera release.
   - `userspace_cond_wait` libera el mutex, espera activamente a que `sequence` cambie (o expire el plazo en `timedwait`) y readquiere el mutex.
   - Retorno traducido a `ETIMEDOUT_BIONIC` (110) en caso de timeout.
3. **Semáforos en Userspace (`userspace_sem`):**
   - Manejo puramente atómico del contador `value` sin consumir `SceUID` de semáforos de kernel.
4. **Registro de Objetos y Cache de Alto Rendimiento:**
   - Tabla hash abierta con 16.384 buckets para hasta 8.192 objetos con resolución en O(1) y gestión de tombstones.
   - Cache lock-free directa de 2.048 ranuras para evitar bloqueos en el hot path.
   - Reciclaje seguro de entradas stale / re-inicializadas por el juego.
   - Corrección en `pthread_attr_setdetachstate_soloader` (`state = !state;` por discrepancia entre PTE y Bionic).

### Resultado
- Compilación limpia de `gangstarrio.elf`, `eboot.bin` y `gangstarrio.vpk`.
- Consumo de semáforos del kernel por primitivas de sincronización reducido de **1.941 a 0**.

---

## Fase 19 — 2026-10-03: unificación pthr anti-ENOSPC (tracking + re-init sin leaks + contadores)

**Evidencia:** `logs/log_014.log` + `logs/gangstarrio-psp2core-1791006003-0x0007883dbd-eboot.bin.psp2dmp`
+ disasm PTE en el propio ELF + conteo SEMAPHORE_INFO.

### Diagnóstico
- `pthread_cond_init` (PTE, verificado por disasm: `calloc(1,32)` + 2×`sem_init` +
  1×`pthread_mutex_init`, cada fallo retorna su errno) → el `0x1c` sale de `sem_init`
  sin semáforos libres. Cada cond ≈ 2-3 semas kernel, cada mutex ≈ 1.
- Dump actual: **1.958 semáforos vivos** (coincide con los 1.959 de tu Fase 17 en el
  dump anterior: saturación sostenida, no pico). CONDVAR_INFO = 2 (PTE no usa condvars
  nativas), MUTEX_INFO = 13, o sea que casi todo son semas PTE de nuestros sync.
- El crash es el mismo de siempre (SP `0x8f2006e0`, R2=R3=`0x94949441`, PC kernel
  +0x3854, 3er dump idéntico): roto el handshake, el worker lee datos no listos.
- Dos fuentes de fuga identificadas (una por versión del wrapper):
  1. Tracking con techo (tu Fase 17: cap 1024 + miss → re-init ciego per-uso).
  2. Re-init explícito sin destroy previo (tu Fase 18 quitó el destroy-old para curar
     el `_free_r`, pero cada re-init del mismo objeto pierde el handle viejo: el cond
     del stack `0x8f2008f8` re-inicializado 2x = +4-6 semas por pasada).
- Además la heurística `IS_VALID_PTR` no distingue basura con rango Vita: el raw
  `0x833b9020` la pasa como handle válido y el wait va a basura SIN log. Solo vale
  tracking de objetos que nosotros inicializamos (el fast-path sin scans es la cache
  lock-free, no el rango).

### Unificación aplicada (`source/reimpl/pthr.c`, conserva lo bueno de ambas versiones)
- De tu rewrite: recheck bajo lock (cierra la race), retornos EINVAL, chequeos de
  malloc/memset-0, meta-lock atómico, `unlock` con lazy-init, traducción de condattr,
  errno en logs. Tu diagnóstico Fase 17/18 queda intacto abajo.
- Restaurado: tracking 1024→**4096** + cache lock-free + publish-after-init. Si la
  tabla se llenara, fail-fast ruidoso (`tracking table FULL`, retorna ENOSPC) en vez
  de re-inicializar silencioso.
- Nuevo: **re-init explícito destruye el handle viejo trackeado antes de crear**
  (cero leaks en el patrón re-init por trabajo; sin tocar memoria del juego → no
  reintroduce tu bug Fase 18 de `_free_r`, que era `free()` sobre residual no
  trackeado). Destroy desregistra de verdad.
- Nuevo: **contadores live** (`ok/fail/destroyed` por tipo, costo cero en hot path):
  reportan por `l_error` al cruzar 128/256/512/... vivos y en cada fallo
  (`conds live~N ok=X fail=Y destroyed=Z`).
- Quitado: `IS_VALID_PTR` como test de inicializado (solo queda mencionado en
  comentarios) y `rememberObject` muerto.
- Build release OK, sin warnings en `pthr.c`.

**Qué mirar en `log_015`:** curva `pthr: cond/mutex live=` (¿sube sin `destroyed`? →
el juego no destruye y Fase 20 = pool o cond custom sin semas kernel) y si el menú
pasa el showcase. Si `live` se estabiliza bajo el techo → resuelto.

---

## Fase 18 — 2026-10-03: Crash en _free_r por desreferenciación y free() de memoria no inicializada en pthread_mutex_init_soloader

**Evidencia:** `logs/log_013.log` (frame 1) + `logs/gangstarrio-psp2core-1791004938-0x0004cd3a1d-eboot.bin.psp2dmp`.

### Bug (CONFIRMADO por core dump + desensamblado + reconstrucción de pila)
- **Log:** el juego arrancó, pasó intro de video y llegó al bucle principal:
  `[022] main loop: frame 1 enter`
  `[GLF] success setting context 2`
  y se interrumpió de inmediato en el frame 1.
- **Core Dump:**
  - `PC: 0x811d9d60` en `_free_r` de newlib libc (`ldr.w r6, [ip, #4]` con `ip = 0xa1313f8a` basura).
  - Pila: `pthread_mutex_init_soloader` (línea 244) llamado desde `vox::Mutex::Mutex()` (`0x94a360`), invocado por `vox::VoxEngine::VoxEngine()` (`0x91082c`) en el frame 1 al inicializar el audio del juego (`SoundManager`).
- **Causa raíz:**
  - En C/C++, `pthread_mutex_init(mutex, attr)` se invoca sobre una estructura asignada en memoria cruda (vía `malloc`, `new`, `VoxAlloc` o variables de stack sin inicializar).
  - En `vox::VoxEngine::VoxEngine()` se ejecutaba `VoxAlloc(4)` para crear un `vox::Mutex`. La memoria devuelta de 4 bytes contenía un valor residual del heap previo (`0x833c54d0`).
  - Como `0x833c54d0` cae en el rango de memoria de usuario de Vita (`0x80000000`..`0xEFFFFFFF`), la macro `IS_VALID_PTR(uid->real_ptr)` evaluó a verdadero.
  - El código de `pthread_mutex_init_soloader` intentó hacer `free(uid->real_ptr)` sobre esa dirección arbitraria, que no era un bloque asignado por `malloc()`, desatando una corrupción inmediata de las estructuras de bloques libres de `newlib` (`_free_r`).
  - Idéntico riesgo residía en `pthread_cond_init_soloader`.

### Solución aplicada (`source/reimpl/pthr.c`)
1. **Inicialización limpia en `pthread_mutex_init_soloader`:**
   - Se removió cualquier lectura o llamada a `free()` sobre `uid->real_ptr`.
   - Se crea directamente un nuevo `pthread_mutex_t *real = malloc(sizeof(pthread_mutex_t))` con `memset(0)`, se inicializa con `pthread_mutex_init` y se asigna a `uid->real_ptr`.
2. **Inicialización limpia en `pthread_cond_init_soloader`:**
   - Se removió cualquier lectura o llamada a `free()` sobre `cond->real_ptr`.
   - Se crea un nuevo `pthread_cond_t *real = malloc(sizeof(pthread_cond_t))` con `memset(0)`, se inicializa con `pthread_cond_init` y se asigna a `cond->real_ptr`.
3. **Mantenimiento de la protección contra fuga de semáforos:**
   - La comprobación `IS_VALID_PTR` en `pthread_mutex_lock/unlock/trylock` y `pthread_cond_wait/signal/broadcast` sigue activa para detectar inicializadores estáticos (`0`, `0x4000`, `0x8000`) sin re-inicializar objetos válidos preexistentes ni provocar fugas de semáforos del kernel.

### Estado de compilación
- Compilación limpia con `psvita-toolkit build --preset release`.
- VPK generado: `build/gangstarrio.vpk`.

---

## Fase 17 — 2026-10-03: Fuga y agotamiento de semáforos del kernel (ENOSPC 0x1c) y crash 0x8000 en pthr.c

**Evidencia:** `logs/log_012.log` (frame 92) + `logs/gangstarrio-psp2core-1791002973-0x0007cc2f7f-eboot.bin.psp2dmp`.

### Bug 1 (CONFIRMADO por log + core dump): Agotamiento de semáforos de PS Vita (ENOSPC / 0x1c)
- **Log:** en frame 92 aparece:
  `cond initialization for 0x837a2ed0 (attr=0x0) has failed: 0x1c`
  seguido inmediatamente de fallos para `0x8f1008f8` con `0x1c`.
- **Significado del código:** `0x1c` = 28 decimal = `ENOSPC` (No space left on device / sin recursos en el sistema).
- **Core Dump:** La inspección de la sección `SEMAPHORE_INFO` del dump reveló **1.959 semáforos kernel vivos** en el sistema (1.906 de ellos creados por `pte_osSemaphoreCreate` en la librería `libpthread.a` de VitaSDK). El kernel de PS Vita impone un límite global de ~1.900-2.000 semáforos por proceso. Al agotarse este cupo, `sceKernelCreateSema` falla con `0x8002001c`, propagándose como `ENOSPC` (28 / 0x1c) en `pthread_cond_init`.
- **Causa raíz en `pthr.c`:**
  1. `initializedObjects[1024]` tenía un tope duro de 1.024 punteros.
  2. La caché directa `pthr_init_cache[1024]` sufría colisiones directas de hash.
  3. Una vez alcanzados los 1.024 objetos (o al colisionar la caché), `isObjectInitialized` devolvía `0`.
  4. Por tanto, cada `pthread_mutex_lock/unlock` o `pthread_cond_wait/signal` posterior re-ejecutaba `pthread_mutex/cond_init`, llamando a `malloc(sizeof(pthread_..._t))` y creando un nuevo semáforo kernel sin destruir ni liberar el semáforo previo. Esto fugaba miles de semáforos durante el streaming del menú y la ciudad.

### Bug 2 (CONFIRMADO por core dump + desensamblado): Data Abort en 0x8000
- Tras fallar `pthread_cond_init` con `0x1c`, `_cond_t_static_init` retornaba `ENOSPC` sin setear `cond->real_ptr`.
- `pthread_cond_wait_soloader` contenía:
  ```c
  if (!cond->real_ptr) {
      _cond_t_static_init(cond, NULL);
  }
  return pthread_cond_wait(cond->real_ptr, ...);
  ```
- Si `_cond_t_static_init` fallaba, el código ignoraba el error. Como `cond->real_ptr` aún conservaba el centinela estático de Bionic (`0x8000` = `PTHREAD_COND_INITIALIZER`), el código ejecutaba `pthread_cond_wait((pthread_cond_t*)0x8000, ...)`.
- La función de VitaSDK intentaba leer `[r0, #0]`, disparando un `Data Abort (0x81013368)` al intentar acceder a la dirección `0x00008000`.

### Solución aplicada (`source/reimpl/pthr.c`)
1. **Identificación universal y O(1) de objetos válidos (`IS_VALID_PTR`):**
   - En PS Vita, la memoria de usuario para heap y particiones dinámicas reside en el rango `0x80000000` a `0xEFFFFFFF`.
   - Las constantes centinela de Android Bionic (`0`, `0x4000`, `0x8000`) y los manejadores de VitaSDK PTE (`-1`, `-2`, `-3`) caen fuera de este rango o no están alineados a 4 bytes.
   - Definición de `#define IS_VALID_PTR(p) (((uintptr_t)(p) >= 0x80000000) && ((uintptr_t)(p) < 0xF0000000) && (((uintptr_t)(p) & 3) == 0))`.
   - Fast path instantáneo sin locks: si `cond->real_ptr` o `mutex->real_ptr` satisface `IS_VALID_PTR`, se sabe con 100% de certeza que ya fue inicializado por VitaSDK y jamás se vuelve a re-inicializar ni a fugar semáforos.
2. **Eliminación de estructuras de tamaño fijo:**
   - Se removió `PTHR_MAX_OBJECTS 1024`, el arreglo `initializedObjects` y `pthr_init_cache`, eliminando por completo cualquier techo de concurrencia y el riesgo de colisiones de hash.
3. **Limpieza estricta de punteros antiguos:**
   - En `pthread_mutex_init_soloader` y `pthread_cond_init_soloader`, si se invoca explícitamente `init` sobre un objeto que ya tenía un `real_ptr` válido previo, se invoca `pthread_mutex/cond_destroy` y se libera la memoria anterior antes de crear uno nuevo.
4. **Protección contra punteros centinela y comprobación de errores:**
   - Todas las funciones (`pthread_cond_wait`, `pthread_cond_timedwait`, `pthread_cond_signal`, `pthread_cond_broadcast`, `pthread_cond_destroy`, `pthread_mutex_lock`, `pthread_mutex_trylock`, `pthread_mutex_unlock`, `pthread_mutex_destroy`) ahora validan el retorno de `_static_init` y verifican `IS_VALID_PTR`. Si un objeto no es válido, retornan inmediatamente `EINVAL` en vez de provocar un Data Abort en el kernel.
5. **Robustez en semáforos:**
   - `sem_destroy_soloader` ahora verifica la validez del puntero y pone a `0` el UID antes de liberar la estructura.

### Estado de compilación
- Compilación limpia con `psvita-toolkit build --preset release` sin advertencias en `pthr.c`.
- Binario `build/gangstarrio.vpk` generado exitosamente.

---

## Fase 16 — 2026-10-03: race check-then-act en pthr (port del fix confirmado de Miami)

**Evidencia previa:** dumps `...1790993939...` (log_010) y `...1790994694...` (log_011, VPK
viejo): mismo crash de menú (cond init failed x3 → data abort en thread de streaming),
así que **no hay bug nuevo: el fix de condattr + el hardening de esta fase siguen
sin probarse**. Instalar el VPK actual (`build/gangstarrio.vpk`, eboot del 22:39+) y
traer `log_012.log` (+ dump si hay). Nota: `log_009` era build Debug y desde `log_010`
Release — en Release `l_info/warn/success` no compilan (`logger.h`), solo sobreviven
`l_error`/`l_note` (los `errno` nuevos son `l_error`, visibles).

**Revisión pedida del hermano Miami + `README VITAGL.md`:**
- `README VITAGL.md` es el README genérico upstream de vitaGL, **idéntico** en ambos
  repos (diff vacío): no trae guía específica del port, pero su tabla de flags es la
  referencia (HAVE_TEX_CACHE, TEXTURE_UPLOADS_SPEEDHACK, HAVE_SHADER_CACHE, ...).
- Miami (`../Gangstar-Miami-Vindication-Vita`, misma familia glitch, más avanzado)
  confirmó en hardware DOS bugs de `source/reimpl/pthr.c` con firma parecida a la
  nuestra y los fijó (su `port_progress.md` Fases del 2026-08-31/09-01):
  1. check-then-act en `_mutex/_cond_t_static_init`: dos threads inicializando el
     MISMO objeto Bionic publicaban `real_ptr` a medias → unlock saltaba a basura;
  2. la MISMA carrera en la creación lazy del propio meta-mutex `pthr_mutex`.
  Dato clave: Miami pasa el attr CRUDO a `pthread_cond_init` y en sus logs **jamás**
  aparece `cond initialization ... has failed` (grep en todos sus logs: 0 hits) —
  su juego no usa conds con attr, o Vita los acepta. Nuestra falla determinista 3/3
  con attr no-NULL sigue apuntando al attr biónico como causa del `ret != 0`, con la
  race como causa del uso corrupto posterior. Se cubren ambas.
- `pthread_cond_t_bionic` es ABI-idéntico en ambos (`diff` de `pthr.h` vacío): el port
  es seguro.

**Cambios (`source/reimpl/pthr.c`, estructura de Miami + traducción de attr de Fase 15):**
- Meta-lock atómico (`PTHR_META_UNINIT/CREATING/READY` con CAS, espera con delay) en
  vez del `pthr_mutex_inited` racy; cache directa lock-free (`_pthr_cache_*`) para el
  hot path (un miss cae al slow path exacto, solo puede ser optimización).
- Publish-after-init: `real_ptr` se asigna SOLO tras init exitoso y bajo lock;
  `memset(0)` en vez de `memcpy` desde stack sin inicializar; chequeos de `malloc`
  (`ENOMEM` logueado); `forgetObject` suelta la entrada de cache primero.
- `pthread_mutex_unlock_soloader` ahora hace lazy-init (caza magics 0x4000/0x8000 que
  el chequeo `!real_ptr` no veía: crash confirmado 2x en Miami en este motor).
- Se MANTIENE de Fase 15: traducción de condattr biónico → attr Vita con defaults,
  `errno` en los logs de fallo, guards `real_ptr==NULL → EINVAL` en uso/destroy.
- Trazas `PTHR_TRACE` ([011][012][013][014][015][021], sin colisión con nuestros
  checkpoints) apagadas por defecto; `-DPTHR_TRACE_LOCKS` las recupera.
- Build release OK, sin warnings en `pthr.c`.

**Qué mirar en `log_012`:** si no hay más `cond initialization ... has failed`, el menú
debería cargar el coche y dejar entrar al juego. Si reaparece CON errno (`: 0x...`),
ese valor discrimina attr (EINVAL) vs recursos (ENOMEM/EAGAIN) para la Fase 17.

---

## Fase 15 — 2026-10-03: crash en el menú (cond biónico con attr → EINVAL → uso sin inicializar)

**Evidencia:** `logs/log_010.log` + `logs/gangstarrio-psp2core-1790993939-0x000bbf25fd-eboot.bin.psp2dmp`.

### Bug (CONFIRMADO por log + dump + pila)
- Log: menú principal OK a 59.9 fps (`showMainMenu`, `initSniperStuff`, ...), luego
  `cond initialization for 0x8627b8d8 has failed` y 2x para `0x8f2008f8`. Fin.
- Dump: Data abort en un `pthread` nuevo (0x400203a5), PC en `SceLibKernel+0x3854`,
  registros basura (R2=R3=`0x94949441`), LR=`0x82000000`. Pila del hilo:
  `glCompressedTexImage2D` → `gpu_alloc_compressed_texture` (vitaGL) + strings de
  texturas del coche (`car_body_reflection`, `..._damage_d.tga`, `alpha.tga`) +
  stubs de `pthread_cond_wait`/`pthread_cond_broadcast` + `isObjectInitialized`.
  El cond `0x8f2008f8` está en el stack de ESTE hilo y su primera palabra es
  `0x833b9020` (no es un cond "cero": se inicializa por `pthread_cond_init` explícito).
- Cadena: el juego (job system de streaming del coche del menú) hace
  `pthread_cond_init(cond, attr)` con attr **biónico** (un int = clock). El wrapper lo
  pasaba crudo a `pthread_cond_init` de Vita, que lee otro layout (pshared/...) y
  falla. El juego no chequea el error y sigue → `pthread_cond_wait/broadcast` sobre
  `real_ptr` sin inicializar → abort en el kernel. Los cientos de conds anteriores no
  fallaron porque iban con `attr=NULL` (lazy-init en wait/signal); el streaming del
  menú es el primer código que usa attr.
- Por qué no es falta de recursos: `malloc` no falló (el `memcpy` posterior no
  crasheó), el fallo es determinista 3/3 con los mismos inputs, y recursos del kernel
  (semáforos) no se agotan de golpe justo al mostrar el menú tras 3 min sanos.

### Fix (`source/reimpl/pthr.c`)
- `_cond_t_static_init`: si `attr != NULL`, se traduce a un attr real de Vita con
  defaults (process-private) en vez de pasar el biónico (`pthread_condattr_init` +
  destroy locales; el clock pedido solo afecta la base de `timedwait`, el juego
  sobrevive sin él). Rama `attr=NULL` intacta.
- Higiene anti-crash: si el init falla, `free + real_ptr=NULL` (antes quedaba basura
  y el `free` posterior era un segundo crash latente); el error ahora loguea
  `cond/mutex, attr y errno` (`0x%x`). Guards `real_ptr==NULL → EINVAL` en
  `cond_wait/timedwait/signal/broadcast`, `mutex_lock/trylock`, y `destroy` ya no
  llama a `pthread_*_destroy(NULL)`. Efecto colateral bueno: tras un fallo, el
  lazy-init de wait/signal/broadcast (con attr NULL) reintenta limpio en vez de
  reventar en el kernel.
- No se tocó `mutexattr` (coincide por suerte: NORMAL/RECURSIVE/ERRORCHECK valen
  0/1/2 en ambos) ni el resto de pthr.

### Video intro (observación, sin cambio de código)
- `log_010` confirma `reads=2, total_bytes=131072` otra vez (idéntico a `log_009`):
  el decoder se rinde tras 128 KB de forma determinista. Sigue pendiente re-subir el
  mp4 a la consola (723304 bytes, modo binario) cuando se quiera retomar; el juego
  continúa sin él. Las líneas de video que "faltan" en `log_010` vs `log_009` no son
  un bug: en Release `l_info/warn/success` no compilan (ver nota arriba).

---

## Fase 14 — 2026-10-03: crash en glShaderBinary por .gxp vacío + video intro sin frames

**Evidencia:** `logs/log_009.log` + `logs/gangstarrio-psp2core-1790991355-0x001139274f-eboot.bin.psp2dmp`.

### Bug 1 (CONFIRMADO por log + dump + addr2line): `load_shader` ignoraba el retorno de `file_load`
- Log: última línea `file_load: .../gxp/B8BD905EA28DEC4C58BE50CD5764034E89819F95.gxp" is empty.`
- Dump: Data abort con PC en `SceLibKernel+0x38` (memcpy del kernel, R2 = `0xffffff8c`
  negativo). Pila simbolizada con `arm-vita-eabi-addr2line` sobre `build/gangstarrio.elf`:
  `load_shader (glutil.c)` → `glShaderBinary` → vitaGL `unserialize_shader`
  (`custom_shaders.c:712`, desreferencia el buffer de entrada sin chequear) → kernel.
- Cadena: `load_shader` declaraba `buffer/size` sin inicializar e ignoraba el `false` de
  `file_load` (archivo existe pero con 0 bytes) → `glShaderBinary` con puntero/tamaño
  basura → `unserialize_shader` lee `matrix_uniforms_num` de basura y `vgl_fast_memcpy`
  revienta. El `free(buffer)` posterior con puntero basura era un segundo crash latente.
- El `.gxp` vacío lo creó el propio loader en una corrida anterior:
  `glCompileShader_soloader` hacía `file_save(next_shader_fname, bin, len)` con `len`
  sin inicializar / 0 cuando la compilación fallaba → ciclo de envenenamiento
  (cada arranque reintentaba el binario corrupto y nunca recompilaba desde fuente).
- Fix (`source/utils/glutil.c`, las 4 variantes de `load_shader` + `glCompileShader_soloader`):
  `buffer=NULL/size=0` iniciales, se chequea `file_load(...) && size > 0`; si falla se
  borra el archivo envenenado (`sceIoRemove`) y se compila desde fuente (rama GLSL) o se
  usa el dummy (ramas CG); solo se guarda caché con `len > 0` y `next_shader_fname` no
  vacío. El shader del dump (`eAndVertexColorVP`, string visible en el core) ahora
  recompila desde fuente hasta que su binario sea válido.
- Mejora lateral: `#include <psp2/io/fcntl.h>` por `sceIoRemove`.

### Tema 2 (video intro, SIN CONFIRMAR causa raíz): 0 frames, STATE_STOP inmediato
- Log: `playing ...logo_gameloft_320x480.mp4` (size 723304 correcto, 2 reads de 64 KB),
  mismos 3 allocs de frame buffers que el run bueno de Shadow Guardian (1 MB + 391680 +
  5 MB, todos por fallback UNCACHE), pero en vez de `STATE_PLAY` llega `STATE_STOP`
  (x3) a los 0.24 s con `video_frames=0, audio_frames=0`. El juego sigue (no es fatal).
- Descartado por inspección: `source/video.cpp` es funcionalmente **idéntico** al de
  Shadow Guardian (diff: solo comentarios + `glCompileShader/LinkProgram` directos, que
  acá son intencionales para no pisar la caché `gxp` del motor); las firmas de los
  callbacks coinciden con `psp2/avplayer.h` del SDK (`readOffset(p, buf, u64 pos, u32 len)`
  etc.); el mp4 local está sano (`ffprobe`: H.264 Constrained Baseline 480x270 30 fps +
  AAC 44.1 kHz, `moov` antes que `mdat`, 220 frames). Hipótesis restantes (requieren
  consola): mp4 corrupto en la subida por FTP, o condición de entorno (memoria/hilos).
- Cambios (`source/video.cpp`, todos seguros, sin tocar la ruta de decode):
  `av_file_size` restaura el offset del fd tras el `SEEK_END`; se loguea el rc de
  `sceAvPlayerAddSource`; heartbeat cada 5 s sin frames; watchdog de 15 s sin frames
  (aborta el intro y sigue con el juego en vez de pantalla negra); diagnóstico final
  con `reads/total_bytes/expected_size` si sale con 0 frames.
- Experimento para `log_010`: si sigue en 0 frames con `reads=2`, re-subir el mp4 a la
  consola en modo binario y comparar tamaño (723304 bytes); el nuevo log dirá si el
  decoder pide más reads o se rinde igual.

---

## Fase 13 — 2026-10-02: lecturas del OBB corrompidas por la capa FILE* de SceLibc

**Evidencia:** `logs/log_008.log` + `logs/gangstarrio-psp2core-1790988167-0x0010a12fd7-eboot.bin.psp2dmp`.

### Bug (CONFIRMADO por dump + bytes del OBB en disco)
- Data abort en `glitch::res::File::Init()` (`0x834cb4`): `[File+0x34]` (bases de secciones
  externas) NULL indexado con 0 externos. Ghidra no decompiló esta función: disasm directo.
- Datos del recurso en el heap (core): BRES válido (`"WaterShader-fx"`, 0x10a0 bytes), pero desde
  el offset `0x340` alta entropía; la entrada de reubicación 5 apuntaba a un slot con `0x7856dd00`.
- El mismo recurso en `main.12030...obb` (en `0x7e4d65a7`) tiene en `+0x340` datos estructurados
  (`"DeepWaterColor"`...). Los bytes basura de memoria están en el disco **exactamente 0xFC00 más
  adelante** (dos muestras: `+0x340`→`+0xFF40`, `+0x420`→`+0x10020`): la posición del `FILE*`
  de SceLibc se desfasó a mitad de la lectura (buffering de SceLibc con `fseek`/`ftell`).
- Fix (`source/glf_filestream.c`, nuevo): los 7 métodos de `glf::FileStream::Impl` (Open/Close/
  Read/ReadWithOffset/Write/Seek/Tell — los ÚNICOS que tocan el `FILE*` en `impl+8`; el resto del
  motor va por la vtable) reimplementados sobre `sceIo`: handle `{SceUID, pos}` propio y **toda**
  lectura posicional con `sceIoPread` → sin buffer ni posición compartida en el fd. Modos
  deducidos de `Impl::Open` (`rb`/`r+b`/`wb` con flag 8/`a+b` previo con flag 0x10, `fseek(END)`
  con flag 4) + `FilePosition::SetFileSize(owner+24, size)`. Reemplaza el hook de Fase 10.

### Menores (vistos en el mismo log)
- `file_save: Could not open ... gxp/*.gxp`: `gl_init` no creaba `DATA_PATH "gxp"` → la caché de
  shaders compilados nunca se guardaba (cada arranque recompilaba). Ahora se crea (`glutil.c`).
- `setThreadPriority(I)V` (no-op) e `isAndroidOrientationAvailable()Z` (false) registrados.

### Video intro (pedido del usuario: "como en Asphalt 5 / Shadow Guardian")
- Único video del juego: `res/raw/logo_gameloft_320x480.mp4` (logo Gameloft), H.264 Constrained
  Baseline 480x270 30 fps + AAC 44.1 kHz estéreo (`ffprobe`). En Android lo mostraba la Activity
  Java; el nativo no lo pide (`MediaPlayerAndroid::*` stubs).
- Mismo formato que el `logo.m4v` de Shadow Guardian (SceAvPlayer, confirmado en consola allá);
  Asphalt 5 usa FFmpeg porque sus videos son MPEG-4 Part 2, no aplica acá.
- `source/video.{cpp,h}` copiados de `../Shadow-Guardian-vita` (allocator de texturas CDRAM →
  PHYCONT → UNCACHE, NV12→RGB en shader GLES2, audio en hilo propio, skip con Cross/Start).
  Cambios: `glCompileShader`/`glLinkProgram` directos (el `glCompileShader_soloader` de este port
  guarda el binario en la caché `gxp` con el nombre del último shader del motor y lo pisaría).
- `main.c`: `video_init()` → `video_play(...)` → `video_shutdown()` tras `gl_init()` y antes de
  `GL2JNILib_init` (checkpoint 42). Link `SceAvPlayer_stub`.

---

## Fase 12 — 2026-10-02: glot::TrackingManager, strcpy(dst, NULL)

**Evidencia:** `logs/log_007.log` + `logs/gangstarrio-psp2core-1790986927-0x0002c83437-eboot.bin.psp2dmp`.

### Bug (CONFIRMADO por log + dump + disasm)
- Log: `GetStaticMethodID(..., "getIdentifier", "()Ljava/lang/String;"): not found` →
  `GetStringUTFChars(NULL)`. Dump: `strcpy` con R1 = 0, LR en `TrackingManager::TrackingManager+0x1d4`.
- **Ghidra no decompiló la zona de glot** (`0xc3xxxx`): ni `glot::` ni sus strings aparecen en
  `out_ghidra.c`, así que el cruce de métodos JNI de la Fase 6 (hecho sobre el pseudo-C) no los vio.
  Disasm directo: `glot::GetDeviceIdentifier` / `GetDeviceHDIDFV` / `GetMacAddress`
  (`0xc3034c`/`0xc30438`/`0xc30524`) llaman `com/gameloft/glot/PortingJNI.getIdentifier` y
  `GLUtils/Device.getHDIDFV` / `getMac`, todos `()Ljava/lang/String;`, y devuelven NULL si Java falla;
  el constructor les hace `strcpy` sin chequear.
- Fix (`java.c`): los tres registrados (ids 55-57) con valores fijos, estables entre arranques.
- Cruce a nivel binario (strings de clase `com/gameloft/...` + los que les siguen en `.rodata`):
  no quedan otras clases/métodos sin cubrir fuera del installer/DRM.

### Riesgo anotado (no tocado sin evidencia)
- glot manda telemetría por HTTP (`ETS_URL`, `/get_last_sent_package_id.php`). `socket`/`connect`/
  `setsockopt`/`ioctl`/... van directo a VitaSDK sin convertir `sockaddr` (Linux: `sa_family` u16;
  Vita/BSD: `sin_len`+`sin_family` u8) ni constantes (`SOL_SOCKET` 1 vs 0xffff). Lo esperable es
  que falle limpio; si cuelga o crashea ahí, neutralizar glot en vez de emular la red.

---

## Fase 11 — 2026-10-02: AndroidOS_JavaVM nunca inicializado

**Evidencia:** `logs/log_006.log` + `logs/gangstarrio-psp2core-1790980572-0x0005962cfb-eboot.bin.psp2dmp`.

### Bug (CONFIRMADO por dump + disasm)
- Data abort en `AndroidOS_GetEnv+0x74` (`0x377610`), R3 = 0. Pila: `glot::TrackingManager::
  TrackingManager` → `glot::GetDeviceIdentifier` → `AndroidOS_GetEnv`.
- `AndroidOS_GetEnv` (TLS por `pthread_key`, y si no hay env: `AttachCurrentThread` sobre
  `AndroidOS_JavaVM`) usa su PROPIO global `AndroidOS_JavaVM` (`.bss 0xe9c5cc`, resuelto vía la
  entrada GOT `0xe88570`), no el `s_javaVm` de `JNI_OnLoad`. Estaba en NULL.
- Lo fija `AndroidOS_JNIOnload(vm)` (`out_ghidra.c:8992`), que en Android dispara el Java
  `InAppBilling.nativeInit`; además corre `AndroidOS::Init` (cachea SUtils/Device/GameInstaller;
  con FalsoJNI `FindClass` nunca da NULL, así que no cae en su `exit()`).
- Fix (`main.c`): llamar `AndroidOS_JNIOnload(&jvm)` justo después de `JNI_OnLoad` (checkpoint 31).

### Bug latente del mismo subsistema (corregido en la misma pasada)
- `AndroidOS::GetSDFolder` llama `SUtils.getPreferenceString("SDFolder", GameInstaller.
  mPreferencesName)`: el 2.º argumento es el nombre del archivo de prefs (no registrado → NULL),
  pero `Method_getPreferenceString` lo devolvía como valor → `GetStringUTFChars(NULL)` + `*src`.
  Ahora devuelve `""` (como el Java real con clave inexistente) → el motor cae a
  `/sdcard/gameloft/games/GloftG4HM/` → `io.c` lo traduce.

### No es bug
- `stat`/`fopen` fallidos de `rest.pak` y `est.pak`: `"est.pak"` es un string que el linker fusionó
  como sufijo de `"rest.pak"`, ninguno de los dos está en los OBB y el motor sigue sin ellos.

---

## Fase 10 — 2026-10-02: el header del OBB se lee como 0 bytes (struct stat mal empaquetado)

**Evidencia:** `logs/log_005.log` + `logs/gangstarrio-psp2core-1790973342-0x0000133e31-eboot.bin.psp2dmp`.

### Bug (CONFIRMADO por dump + memoria del core + disasm)
- Data abort en `CByteStream::ReadInt` (buffer NULL), LR en `CCustomPakReader::scanLocalHeader+0x160`:
  el stream de entradas usa `new[](count*16)`, que devolvió NULL.
- Leyendo la memoria del core (`vita-parse-core` `CoreParser.read_vaddr`): el buffer de 16 bytes
  del header contiene basura de heap (`c4373081 c4373081 ".com.gam"`) → el `read` no trajo nada →
  `count` = `0x2e67616d` (".gam" big-endian) → `new[]` de ~12 GB → NULL. El header real del patch
  OBB es `0000206e 00005266 00001740 00000173` (371 entradas).
- Cadena: `CGlfReadFile::read` → `glf::FileStream::Read` (vtable+0xc) → `Impl::Read` (impl
  vtable+0x18) = `fread(buf, 1, min(size, fileSize - ftell))`. `fileSize` lo fija `Impl::Open`
  con `stat()` leyendo **`[stat+48]`** (st_size de bionic ARM/EABI, struct de 104 bytes).
- `stat64_bionic` (`reimpl/io.h`) era `__packed__` con `nlink_t/uid_t/gid_t` de newlib (2 bytes):
  `st_size` quedaba en el offset **38** (verificado compilando con arm-vita-eabi-gcc). `fileSize`
  = basura → `Read` leía 0 bytes. Afectaba a TODO `stat`/`fstat` del motor.
- Fix: struct con tipos de ancho fijo y alineación natural EABI + `_Static_assert` de offsets
  (st_size 48, st_blocks 64, st_mtim 80, sizeof 104). El conversor hace `memset` primero.

### Mismo origen (layout de FILE), corregido en la misma pasada
- `glf::FileStream::Impl::ReadWithOffset` (`0x90ce78`) saca el fd con `ldrsh [FILE+14]` (`_file`
  de bionic, inline) y llama `pread`. Con `USE_SCELIBC_IO` el `FILE*` es de SceLibc → fd basura.
  Hook en `patch.c`: `fseek`/`fread`/`fseek` sobre el mismo `FILE*` (impl+8) bajo un LwMutex.
- `rewind` apuntaba al de newlib con `FILE*` de SceLibc → `rewind_soloader` (`io.c`).

---

## Fase 9 — 2026-10-02: los OBB no se montan → ChoosePerfProfile sin perfiles

**Evidencia:** `logs/log_004.log` + `logs/gangstarrio-psp2core-1790972883-0x000e5c2fb9-eboot.bin.psp2dmp`.

### Bug (CONFIRMADO por log + dump + RE)
- Data abort en el primer frame, LR en `ChoosePerfProfile` (desde `Application::SetupFolders`).
  `LoadPerfProfiles("_packRes/PhonePerformanceProfiles.gmap")` falla en las 4 rutas que prueba
  (`out_ghidra.c:35424`) → `gPerfProfiles` vacío → `gPhonePerf` NULL → se desreferencia.
- El `.gmap` está **dentro del patch OBB** (formato de archivo propio de Gameloft, no zip:
  `grep -a` encuentra `_packRes/PhonePerformanceProfiles.gmap` en `patch.12221...obb`).
- Los OBB no se montaban: `getObb*FileName` devuelve `ux0:data/gangstarrio/<pkg>/...obb`
  (correcto, existe), pero el glitch FS (`addFileArchive`, vtable+0x7c) solo trata como absoluta
  una ruta que empieza con `/`, y le antepone sus directorios de trabajo: el log muestra
  `fopen(../Documents/ux0:data/...)` y `fopen(ux0:data/gangstarrio/data/ux0:data/...)`.
- Fix (`io.c`, `rio_translate_path`): si aparece un dispositivo Vita (`ux0:`, `uma0:`, `imc0:`,
  `app0:`) precedido de `/` en medio de la ruta, se descarta el prefijo. Vale para cualquier
  `VJoinPath` del motor sobre una ruta Vita, no solo para los OBB.
- Mejora: `mkdir`/`remove`/`unlink` iban directo a newlib sin traducir rutas (los saves habrían
  fallado igual). Ahora tienen wrappers `_soloader` en `io.c` y apuntan ahí desde `dynlib.c`.

---

## Fase 8 — 2026-10-02: prefetch abort en 0xffff0fc0 (__kuser_cmpxchg)

**Evidencia:** `logs/log_003.log` + `logs/gangstarrio-psp2core-1790968518-0x0002303893-eboot.bin.psp2dmp`
(PSVGR0001). `analyze --so-base 0x98000000` da la simbolización correcta.

### Bug (CONFIRMADO por dump + disasm)
- Prefetch abort, `PC = 0xffff0fc0`, `R8 = 0xffff0fc0`. Pila: `getViewSettings` →
  `ThreadMgr::ThreadMgr` → `ThreadMgr::Add` → `glf::SpinLock::Lock` → `__sync_*` de libgcc
  (`0x387270`: `ldr r8, =0xffff0fc0; ... blx r8`).
- Es el libgcc `linux-atomic.c` del NDK: todos los `__sync_*` del `.so` llaman a los *kernel user
  helpers* de Linux/ARM en direcciones fijas (`0xffff0fc0` cmpxchg, `0xffff0fa0` memory_barrier).
  Android mapea esa página; la Vita no. Afecta a TODO lo atómico del motor (spinlocks, refcounts,
  `__emutls`), no solo a este call site.
- Escaneo del `.so`: 43 literales `0xffff0fc0` + 4 `0xffff0fa0`, todos en el bloque
  `0x3866a4-0x387554`; ningún `0xffff0fe0` (get_tls) ni lectura directa de TPIDRURO
  (`mrc p15,0,rX,c13,c0,3`): el TLS pasa por `__emutls` → `pthread_key_*` (ya puenteados).
- Fix (`patch.c`, `patch_kuser_helpers`): se reescriben los literales (vía
  `kuKernelCpuUnrestrictedMemcpy`, antes de `so_flush_caches`) para apuntar a `kuser_cmpxchg` /
  `kuser_memory_barrier`, funciones `naked` en ARM con `ldrex/strex` + `dmb ish` que respetan el
  contrato del kernel (r0=old, r1=new, r2=ptr → r0=0 y C=1 si intercambió; solo pisa r3/ip/flags;
  una función C normal pisaría r1/r2). Loguea la cuenta y avisa si no son 43+4.

---

## Fase 7 — 2026-10-02: crash en setPaths (App todavía no creada)

**Evidencia:** `logs/log_002.log` (corta tras `[005] GL2JNILib_init() done`) +
`logs/gangstarrio-psp2core-1790968206-0x0003d022f5-eboot.bin.psp2dmp` (title ID verificado: PSVGR0001).

### Bug (CONFIRMADO por dump + disasm)
- Data abort en `strcpy` con `R0 = 0x000100f8` y R3/R4 = `"ux0:" "data"` en ASCII.
- `glf::AndroidSetPaths` (`0x373f14`, ARM): `ldr r0, [global App*]` → `App::GetFs` (`r0 + 0x100f8`)
  → `Fs::SetDataDir` (strcpy). Con la App sin crear, `r0 = NULL` → escribe en `0x100f8`.
- La App la crea `getViewSettings` (`NewApp`), que acto seguido llama `glf::AndroidSetupPaths`
  → Java `setupPaths()` → en Android la Activity llamaba al nativo `setPaths(data, home, temp)`.
  `main.c` llamaba `setPaths` ANTES de `getViewSettings` y `Method_setupPaths` era un no-op.
- Fix: `Method_setupPaths` (`java.c`) crea los directorios y llama a `GL2JNILib_setPaths`; `main.c`
  ya no lo llama. (Nota: el `analyze` del toolkit sigue auto-detectando mal la base del `.so`, la
  simbolización de LR no es confiable; la real es `0x98000000`.)

---

## Fase 6 — 2026-10-02: crash en AndroidGetDeviceModel (jstring con representación equivocada)

**Evidencia:** `logs/log_001.log` (corta en `[GLF] AndroidCreateView`). El dump que vino con él,
`...1790913164-0x0001772033...psp2dmp`, **NO es de este juego**: adentro dice
`ux0:/app/PSVDH0003/eboot.bin` y `ux0:data/dungeonhunter3/...` (módulo `dungeonhunter3`). Es un
crash del port de Dungeon Hunter 3 que se bajó con el prefijo `gangstarrio-`. Antes de analizar un
dump, confirmar el title ID: `gunzip -c X.psp2dmp | LC_ALL=C grep -a -o "ux0:/app/[A-Z0-9]*/eboot.bin"`.

### Bug (CONFIRMADO por log + RE)
- `glf::AndroidInit` (`out_ghidra.c:5778`) imprime `AndroidCreateView` y llama
  `AndroidGetDeviceModel()` (`out_ghidra.c:353067`): `getPhoneModel()Ljava/lang/String;` +
  `GetStringUTFChars` (0x2a4). La línea siguiente (`AndroidGetDeviceModel : %s`) nunca salió.
- `jstr_ret()` en `java.c` devolvía un `JavaDynArray` (byte-array crudo, receta del hermano Miami),
  pero el FalsoJNI de este repo modela `jstring` como `JavaString {utf16*, utf8*}`
  (`FalsoJNI_ImplBridge.h:143`). `GetStringUTFChars` leía `jda->len` como puntero `utf16` → data abort.
- No había warnings JNI en el log porque todos los métodos estaban registrados: el fallo era de
  representación, no de registro.
- Fix: `jstr_ret()` = `NewStringUTF`. Corrige de una vez a todos los métodos que devuelven `String`
  (getPhoneModel/FW/Manufacturer, getIMEI, getVersion, getObb*FileName, getSaveFolder, getPackage...).

### Métodos que pide el `.so` y no están registrados (revisados, seguros)
- `getDefault` (Locale): `GetPreferredLanguages` chequea `methodID != NULL` → cae a "en".
- `getContext`: se cachea en `AndroidOS::Init` pero no se usa.
- `startGame`, `getSDFolder`, `randomUUID`/`toString`: solo installer/DRM, que el loader no ejecuta.
- `setProcessPositionEvents`, `UpdatePreferences2`: `void`.

---

## Fase 5 — 2026-10-01: primer arranque en consola -- crash en GL2JNILib_init + logs a archivo

**Evidencia:** `logs/gangstarrio-psp2core-1790912977-0x0009fe23a5-eboot.bin.psp2dmp`, sin ningún log.

### Bug 1 (CONFIRMADO por dump + RE): `*lockPointer4 = 1` con lockPointer4 == NULL
- Data abort, PC `0x9837256c` = `libgl2jni.so + 0x37256c` (el .so carga en `0x98000000`; el
  `analyze` del toolkit auto-detectó mal la base, `0x80889000`), LR en `NewGlobalRef` de FalsoJNI.
- Disasm: `GL2JNILib_init` hace `NewGlobalRef(cls)` y enseguida
  `if (isCheckLicence == 0) *lockPointer4 = 1;` (`out_ghidra.c:5032`), después `*lockPointer3 += 1`
  dos veces. Esos punteros los reserva el DRM (`LicenseCheck`, `GameInstaller.nativeStart`) que el
  loader no corre → NULL. La afirmación anterior de que el juego "nunca consulta lockPointer" era
  falsa.
- Fix (`main.c`): antes de `init`, `lockPointer1..4` = `malloc` con valor 1, el estado exacto que
  deja una licencia válida (`out_ghidra.c:14679-14682`, `15224-15226`).

### Bug 2: no se generaba ningún log
- `l_checkpoint`/`l_success` eran `l_debug` → compilados fuera en Release; el crash fue antes de
  cualquier `l_note`.
- El log propio del motor (`glf::Console::Println` → `ConsoleScopeBuffer` → `glf::Console::PrintRaw`)
  no pasa por `__android_log_*`: ahora `patch.c` engancha `PrintRaw` (0x905024) → `[GLF] ...`.
- Logs ahora en `ux0:data/gangstarrio/logs/log_001.log`, `log_002.log`, ... (contador en
  `log_counter.txt`), texto plano con tiempo `[ssss.mmm]` y sin códigos ANSI. Durante el arranque
  se sincroniza cada línea a disco (sobrevive a un crash); a partir del frame 11, cada 256 líneas.
- `l_checkpoint` y los pasos de `soloader_init_all` se loguean siempre, también en Release.

---

## Fase 4 — 2026-10-01: terminar el bootstrap, gráficos, input, audio, video, LiveArea

**Punto de partida:** la sesión anterior dejó `main.c`/`java.c`/`dynlib.c`/`io.c` escritos con
buen RE del ciclo de vida y de la tabla JNI, pero el build no compilaba y la documentación no
reflejaba nada (`PORTING_PLAN.md` seguía con la detección automática).

### Build
- `java.c`: `fjni_logv_warn` sin argumentos variádicos → `fjni_log_warn` (error de compilación).
- `main.c` usaba `l_checkpoint`/`l_note`, que no existían: se trajo `utils/logger.{c,h}` del
  hermano Miami (archivo local por sesión + UDP para `logs-live`), con su `LOCAL_LOG_DIR` apuntado a
  `ux0:data/gangstarrio/logs` y creación del directorio en instalación limpia. Link +SceNet,
  SceNetCtl, SceSysmodule.
- `reimpl/log.c` del hermano: el log del motor (`__android_log_*`) va por `l_note` y sobrevive a
  Release, con el filtro de spam de glitch.
- `dynlib.c`: faltaba `_ZnajRKSt9nothrow_t`; `fatal_error` sin declarar.
- `lib/falso_jni/converter.c` no estaba en CMake (`utf8_to_utf16` sin resolver).

### Gráficos
- El `libvitaGL.a` global del vitasdk lo había recompilado otra sesión (Sacred-Odyssey) con EGL
  propio → símbolos duplicados contra `reimpl/egl.c`. Se vendorizó vitaGL + vitashark como
  submódulos upstream, compilados con `HAVE_SOFTFP_ABI` (el `.so` es softfp).
- El `.so` **no importa EGL**: se sacó `reimpl/egl.c` y se usa el `egl.c` de vitaGL.
- Receta de memoria de Miami: heap 192 MB, pool circular 64 MB, sin MSAA, threshold 8 MB.
- Contextos GL extra: el motor no los usa si nadie llama `setNumExtraContext` → todo en el hilo GL.

### Assets
- El formato de textura está fijo en 3 (ETC + patch); el obb del cache es ETC. `getObbFileName`/
  `getObbPatchFileName` ahora aceptan el nombre Android original del zip (no hay que renombrar)
  y loguean un error claro si falta. Antes ambos usaban la misma función (el patch caía al main).
- `io.c`: `/sdcard/gameloft/games/GloftG4HM` → `data/` (respaldo de archivos sueltos de
  `_packRes`), buffer rotativo de rutas con índice atómico y 16 slots (lo llaman varios hilos).

### Input (bug real corregido + mando físico)
- **Bug:** `main.c` mandaba `touchEvent` con la convención de MotionEvent (0=down, 1=up). El motor
  usa la de Gameloft: 0=UP, 1=DOWN, 2=MOVE (`AndroidAddTouchEvent` + `TouchScreenBase`, ver plan
  sección 5). Cada toque habría llegado como "soltar".
- **Descubrimiento:** el motor tiene esquema de mando Xperia Play / PowerA completo, y el comentario
  anterior ("sólo BACK/MENU, todo lo demás es no-op") era incorrecto: `keyEvent` pasa primero por
  `transfer_POWER_A_toGLF`. Nuevo `utils/controls.c`: botones → keycodes Xperia, stick izquierdo →
  touchpad izquierdo del Xperia, stick derecho → `nativeSetPowerARightJoystick`.
  `IsPowerAConnected()` devuelve 1 para activar el esquema.

### Audio
- El stub de `slCreateEngine` devolvía FEATURE_UNSUPPORTED (juego mudo). Vox usa OpenSL estándar
  (BufferQueue PCM 16/44.1k estéreo): se enlaza `libOpenSLES.a` real del vitasdk (+ sndfile, ogg,
  vorbis, FLAC, opus, mpg123, mp3lame que pide).

### Video
- No aplica: `MediaPlayerAndroid::*` son stubs en el binario e `isFinish()` = 1.

### LiveArea
- Los PNG eran los placeholders del boilerplate (mismo md5). Nuevos desde `ic_launcher.png` y
  `gi_background.png` del APK, convertidos/validados con `psvita-toolkit livearea --validate`.
- `NAME ${VITA_APP_NAME}` sin comillas partía "Gangstar Rio" en dos argumentos.

### Higiene
- `.gitignore`: `decompiled/disasm/` (332 MB derivado del binario) no estaba ignorado; también
  `compile_commands.json`.

### Qué mirar en el primer log
1. ¿Llega a `[ALOG]... GS3: JNI_OnLoad called.` y a los heartbeats `[022]`?
2. `[JNI] getObbFileName(...) -> ...` sin `not found`.
3. `slCreateEngine`/Vox sin errores; ¿se oye?
4. Errores de traducción de shaders de vitaGL.
5. En gameplay: sentido de ambos sticks y si el HUD de mando aparece.

---

## Fase 3: Análisis del Motor Real (Completada — 2026-10-01)
- Motor glitch + glf + Vox + gameswf (ver `PORTING_PLAN.md` sección 0).
- Ciclo de vida nativo y tabla JNI reconstruidos desde el `.so` (APK empaquetado, jadx no sirve).

## Fase 2: Decompilación (Completada — 2026-09-01)
- jadx: corrido, resultados en decompiled/apk_jadx/ (sólo el stub del protector).
- Ghidra (.so): corrido para cada .so.

## Fase 1: Configuración y Preparación (Completada — 2026-09-01)
- Repo creado desde soloader-boilerplate, `.gitignore` anti-DMCA.
- APK `Gangstar-Rio-1-2-2b-patched.apk` copiado y extraído.
