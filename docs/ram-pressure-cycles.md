# Gestión y diagnóstico de presión de RAM

Este cambio conserva el perfil lógico de RAM, el presupuesto residente, la
topología de CPU y la regulación térmica de Prime. Atiende dos costes concretos:
la pérdida de páginas decodificadas al redimensionar la caché y las comprobaciones
de preparación que podían quedar separadas por casi diez segundos.

No demuestra todavía qué causó una caída de rendimiento durante un arranque de
Windows en un teléfono. Las pruebas utilizan páginas, respaldo real qSSD,
concurrencia y huéspedes sintéticos; la medición del dispositivo sigue siendo
necesaria para atribuir su caída de pasos/s.

## Ciclo 1: medir las rutas que pueden detener una escritura

La dependencia Mundo X expone `SparseRam::pressure_metrics()`. La sesión Android
la exporta como `ram_profile.access_pressure`, con `schema_version: 1`. Los
contadores son acumulados desde el reinicio de la memoria. El propietario obtiene
el snapshot después de reunir los lectores paralelos, como las métricas existentes.

No se añaden lecturas de reloj a los accesos residentes ni a los aciertos de
caché. Los contadores de recuperación del respaldo siempre están activos, pero
sus cronómetros detallados empiezan **apagados** en cada sesión. Las duraciones
de promoción para escritura y reclamación forzada siguen midiendo esas rutas
concretas, independientemente de la opción de lecturas del respaldo.

| Campos | Significado |
| --- | --- |
| `forced_reclaims`, `forced_failures` | Preparaciones de escritura que inicialmente no cabían y fallos al conseguir espacio. Una asignación o recuperación posterior puede fallar por separado. |
| `requested_pages` | Nuevas representaciones densas necesarias en esas operaciones; excluye las páginas ya residentes que también se tocaron. |
| `reclaimed_bytes` | Reducción del payload residente conseguida durante esos intentos. |
| `forced_reclaim_ns`, `forced_reclaim_max_ns` | Tiempo acumulado y máximo de conseguir espacio, antes de asignar o recuperar la página de destino. |
| `selection_ns` | Búsqueda de páginas retirables, incluida la compactación intentada durante esa búsqueda. |
| `compact_attempts`, `compacted_pages`, `compact_ns` | Intentos, éxitos y tiempo de compactación dentro de la reclamación forzada. |
| `spilled_pages`, `spill_ns` | Páginas guardadas y tiempo de escritura durante esa reclamación. |
| `clean_reclaims` | Páginas retiradas aprovechando una copia de respaldo vigente. |
| `write_promotion_attempts`, `write_promotions`, `write_promotion_ns` | Recuperaciones intentadas para escribir, promociones publicadas y tiempo de recuperación; excluye la asignación previa del buffer. |
| `backing_read_attempts`, `backing_read_failures` | Lecturas por fallos de caché, incluidos inspección y promoción. Se cuentan con los cronómetros encendidos o apagados. |
| `backing_timing_enabled` | Booleano que indica si los cronómetros detallados de lectura del respaldo están activos en este snapshot. |
| `backing_timed_attempts` | Intentos de lectura realizados con esos cronómetros activos; puede ser menor que `backing_read_attempts`. |
| `backing_read_ns`, `backing_read_max_ns` | Tiempo acumulado y máximo de las lecturas medidas; excluye la espera del bloqueo de almacenamiento. |
| `backing_lock_wait_ns` | Espera medida al adquirir `storage_mutex`; no incluye la espera anterior del mutex de cada conjunto de caché. |

Las duraciones **no se suman como categorías independientes**. `compact_ns`
está dentro de `selection_ns`; selección y escritura están dentro de
`forced_reclaim_ns`. La promoción puede incluir una lectura del respaldo y
sus bloqueos. Tampoco se deben sumar tiempos de distintos lectores concurrentes
para obtener un porcentaje del tiempo real de la sesión.

Los intentos fallidos conservan sus tiempos. Las promociones solo se anuncian
como completadas al publicar las páginas. Los contadores se transfieren al mover
la memoria y se reinician con `reset_zero()`.

### Activar una ventana de medición detallada

En **Último diagnóstico**, el estado visible de los cronómetros procede del
snapshot. **Activar cronómetros de lectura del respaldo** enciende la medición;
la misma acción pasa a **Desactivar cronómetros de lectura del respaldo**.
La nota del diálogo explica que medir añade coste y puede reducir la velocidad.
Copiar texto y exportar JSON/TXT siguen disponibles.

La acción se encola en `VmWorker` y verifica que corresponde a la misma sesión.
JNI utiliza el mutex de instancia compartido con la ejecución; el propietario
cambia la opción entre ventanas, una vez reunidos los lectores paralelos.
`AndroidBootSession::set_backing_read_timing_enabled(bool)` delega en
`MemoryFabric::set_backing_read_timing_enabled(bool)`. Tras el cambio se publica
y guarda un diagnóstico nuevo, que confirma el estado en el diálogo.

Se puede activar o desactivar con la máquina pausada. No se guarda una
preferencia: una sesión nueva empieza apagada. Apagar la medición **no borra**
contadores ni tiempos anteriores; por eso `backing_timing_enabled: false` puede
acompañar tiempos no nulos. Para obtener medias de tiempos medidos, comparar
incrementos de `backing_timed_attempts`, no dividir por todas las lecturas de la
sesión. Un diagnóstico antiguo sin el campo muestra «sin dato».

La pestaña **Telemetría → RAM** muestra operaciones que necesitaron espacio,
tiempo acumulado y máximo para conseguirlo, lecturas del respaldo, aciertos de
caché y memoria de la caché. Los detalles permanecen en el JSON exportado.

## Ciclo 2: conservar una caché válida

`BackingCache::resize()` reorganiza las entradas válidas en lugar de vaciarlas
todas. Mueve sus buffers, sin copiar el payload ni reservar una segunda copia.
Descarta entradas invalidadas y las que no caben después de las colisiones.

La prioridad por recencia es local a cada conjunto, no un LRU global. El cambio
mantiene el límite de 2 MiB y la proporción máxima de 1/128 del presupuesto. La
tabla temporal tiene hasta 512 entradas de ownership, no 512 páginas duplicadas.
El propietario sigue teniendo que reunir a los lectores antes de redimensionar.

Las pruebas exigen que entradas retenibles sigan siendo aciertos incluso si el
respaldo falla durante esa lectura. También comprueban colisiones, escrituras
posteriores, invalidación, descarte a ceros y presupuesto total.

No se amplía automáticamente la caché ni se promocionan páginas en cada lectura.
Esos cambios necesitan medir la reutilización real y el espacio que quitarían a
otras páginas residentes.

## Ciclo 3: preparar páginas con anticipación

`VmRamMaintenance` separa el sondeo de elegibilidad de la muestra de memoria de
Android:

| Estado | Próximo intervalo de programación |
| --- | --- |
| Preparación activa | 50 ms, como antes. |
| Máquina ejecutándose, sin lote activo | Hasta 250 ms para volver a comprobar elegibilidad. |
| Máquina pausada | Hasta la siguiente muestra de presión de Android. |
| Reanudación explícita | Comprobación inmediata. |

Una muestra de presión que venza antes conserva prioridad. El período ordinario
de `MemoryInfo` sigue siendo 10 s; durante reclamación conserva el período previo
de 1 s. La primera muestra después del inicio ahora puede producirse a los
250 ms. Los nuevos sondeos entre muestras no consultan `MemoryInfo` por sí solos.

El umbral del 75 %, el objetivo de reserva y las ventanas de trabajo nativo
permanecen iguales. Los intervalos son de programación: un trabajador ocupado
puede atenderlos más tarde. La ventana de preparación de 750 microsegundos es
cooperativa y no interrumpe una llamada de almacenamiento bloqueante.

`ram_profile.reclaim_preparation` añade `checks`, `last_reason`, objetivo,
reserva antes/después, déficit, ocupación y umbral de la última comprobación.
Incluye contadores de exclusión por estado, lotes sin páginas preparadas ni
bytes liberados, y tiempos total/último. Estos tamaños describen la última
comprobación; no son una lectura continua actual.

`last_reason` distingue `not-checked`, `no-backing`, `non-prime`,
`no-pressure-sample`, `android-low-memory`, `budget-adjustment`,
`below-threshold`, `reserve-ready`, `backing-failed`, `window-overrun`,
`scanning` y `preparing`. Un lote vacío puede haber avanzado la búsqueda o
consumido segundas oportunidades sin producir aún una página retirable.

## Cómo comparar una caída de rendimiento

Exportar diagnósticos de la misma sesión antes y durante la caída. Comparar
diferencias entre contadores acumulados y el tiempo transcurrido, no solamente
los totales finales. El archivo automático conserva el último snapshot.

1. Verificar `app_version`, `mundo_x_sha` y el manifiesto de dependencias del APK.
2. Comparar la tasa real con la tasa dentro del motor y el estado térmico.
3. Comprobar crecimiento de reclamaciones forzadas, lecturas del respaldo y sus
   tiempos, junto con presupuesto residente, disponibilidad de Android y PSS.
4. Revisar si la preparación se omite, queda buscando, pierde su reserva o
   excede la ventana; no atribuir todo el tiempo etiquetado RAM a falta de memoria
   física, pues también incluye traducción de direcciones.

Una reducción de lecturas tras redimensionar demuestra reutilización de caché.
No garantiza una tasa fija de pasos/s: el coste de los pasos y la carga de Windows
pueden cambiar durante el arranque.

## Validación de este candidato

Con el árbol de Mundo X publicado en `49bc18dbc845e9842c393e26c421539068ca5f82`,
idéntico al árbol local validado, la integración nativa
pasó **11/11 pruebas CTest**: ISO sintética, sesión Android, JSON de presión,
modo largo x86, respaldo de RAM, vídeo, SparseRam, instrumentación, memoria PC,
MemoryFabric y SMP. La suite de SparseRam contiene 42 casos y 339 aserciones;
la prueba de instrumentación ejecuta 122 comprobaciones. También pasaron las
pruebas Java de regulación térmica, tasa de pasos y calendario de mantenimiento.
Estas pruebas locales no ejecutan Windows real ni una interfaz Android.

La regresión de caché falla contra el encabezado original porque intenta leer
del respaldo después de un resize que debería conservar entradas. Con el cambio,
la misma prueba conserva los bytes y no necesita esa lectura.

### Coste de medir

Se compararon tres repeticiones con `g++ 14.2 -O2`, afinidad a una CPU del host y
respaldo sintético en RAM. El caso de misses recupera 4 KiB en cada operación;
es deliberadamente favorable a detectar el coste proporcional de los relojes.

| Comparación de misses | Mediana base | Mediana candidato | Interpretación |
| --- | --- | --- | --- |
| Cronómetros inicialmente siempre activos | 172,47 ns/op | 248,99 ns/op | +44,4 %; motivó hacerlos opcionales. |
| Nueva comparación, cronómetros apagados | 162,63 ns/op | 175,16 ns/op | +7,7 %, con rangos solapados; no demuestra coste cero. |

Los checksums y las transferencias coinciden. En la segunda comparación, OFF
conserva todos los intentos y deja los intentos temporizados y sus tiempos en cero.
Una muestra adicional ON confirma que los cronómetros se activan. Los aciertos
de caché no muestran una regresión clara en estas mediciones.

La prueba intensiva de resize conserva 64 páginas entre presupuestos de 32 y
64 MiB: pasó de 5.496.832 lecturas al respaldo a cero durante el tramo medido.
La carga redimensiona después de cada 64 lecturas; **su ratio no representa una
ganancia esperable en Windows**.

Se conserva el [harness](validation/ram-pressure/ram_paths.cpp), la
[comparación inicial](validation/ram-pressure/ram_paths_results.json) y la
[revalidación con modo opcional](validation/ram-pressure/ram_paths_opt_in_results.json).
El commit local de la última medición y el pin publicado tienen el mismo árbol
Git, registrado en el JSON. El campo `cpu` es el índice de afinidad del host de
pruebas, no una característica del teléfono.

Para reproducir un caso, extraer el encabezado de la revisión elegida como
`sparse_ram_selected.hpp` en un directorio temporal y compilar el harness con
`g++ -std=c++17 -O2 -pthread -I<directorio> ram_paths.cpp -o ram_paths`.
El ejecutable acepta `resident`, `hit`, `miss` o `resize` y un número de
iteraciones; por ejemplo, `ram_paths miss 1584768`.
Para el candidato final, `-DHAS_PRESSURE -DHAS_TIMING_FLAG` añade sus contadores;
`-DENABLE_TIMING` activa los cronómetros. Comparar el mismo trabajo y checksums,
alternar el orden de las versiones y no usar el tiempo como aserción de CI.
