# Prime r105 · Auditoría de la tarjeta de vídeo y asistencia Éter

La C08 prepara EGL y OpenGL ES al encender la placa, puede recibir mallas mediante PCI y devuelve imágenes al invitado. En r105 Éter puede reducir la geometría antes de llegar al controlador de Android. Las escenas repetidas también pueden completar una orden sin volver a rasterizar. La pantalla de arranque tiene su propia mejora de presentación VBE. La magnitud que se comprueba en esta versión es el trabajo que llega a cada ruta.

## Qué ocurrió durante el arranque analizado

El diagnóstico de Prime 0.3.75 registra 27 921 cuadros VBE a 1024 × 768. El framebuffer BGRX ocupa 3 145 728 bytes. El dispositivo C08 registra cero dibujos, envíos al planificador, cargas y bytes residentes de su caché. El nombre del renderizador está vacío. Esa sesión muestra que el invitado actualizaba el framebuffer de arranque. La información disponible para esa versión deja indeterminado qué parte de la composición final hizo el chip de Android.

El reloj de GPU ya indicaba 4 500 000 000 000 Hz. En r105 la GPU y su memoria exponen esa frecuencia en el dominio temporal virtual de la placa. Los 120 000 000 000 bytes son capacidad direccionable. El caudal físico, los núcleos del chip y su DVFS se miden en el dispositivo. La ocupación de la caché crece con solicitudes que la usan. La captura VBE, las texturas del controlador y la memoria residente de C08 tienen contabilidades distintas.

El invitado entró en modo de 64 bits. Entre los puntos de control de 10 200 y 10 550 millones de pasos del procesador de arranque, las interrupciones entregadas subieron de 672 128 a 1 146 194. Los contadores de lecturas BIOS del medio, operaciones ATA y lecturas/escrituras de desbordamiento de RAM permanecieron constantes. En la sesión completa llegaron a ejecutar siete de dieciséis procesadores lógicos anunciados. Hay seis arranques secundarios aplicados y seis destinos solicitados. Estos datos orientan la investigación del cargador hacia el código ejecutado por la CPU y la temporización de dispositivos. Atribuir la espera a un controlador, un bloqueo concreto o una tormenta de IRQ requiere una traza adicional con el punto de arranque alcanzado. Una finalización MSI de la GPU sí tiene efecto observable para Windows. La prueba WDDM verifica que se entrega incluso al reutilizar la imagen.

## Capacidades inspeccionadas

| Componente | Ruta actual de Prime | Qué falta para ampliar la compatibilidad o el rendimiento |
| --- | --- | --- |
| Encendido y presentación del arranque | La placa prepara C08 antes de la primera instrucción. El capturador de páginas VBE y GPU-VIRTUAL producen rectángulos, qSSD publica la revisión y Android conserva listas de dibujo por franja. | Medir tiempo hasta el instalador de la ISO en el móvil y el coste de captura, subida y composición por separado. |
| Ejecución física | REgraphics crea un contexto GLES 2 y un pbuffer con color RGBA8 y profundidad de al menos 24 bits. El sistema anfitrión ejecuta la rasterización. | Consultar límites y extensiones por modelo. El detector actual de hardware reconoce nombres de renderizador. Un nombre desconocido necesita comprobación. C08 carece de un contador de núcleos físicos. |
| Geometría | Hasta 12 288 vértices por orden. El planificador respeta el orden y elimina triángulos completamente fuera de un mismo plano de recorte. Se dibujan listas de triángulos con posición y color. | El UMD devuelve operación no soportada para dibujos indexados e instanciados. Hacen falta buffers de índices, comprobaciones de límites, instancias y pruebas contra una referencia Direct3D. |
| Sombreadores y texturas | El UMD reconoce un subconjunto de bytecode de vértices y píxeles para transportar posición y color. REgraphics usa un programa fijo. | Soportar texturas 2D, formatos, muestreo, mipmaps y sombreadores de aplicación. Cada capacidad anunciada necesita resultados comparables frente a WARP y pruebas de errores. |
| Salidas y estados de dibujo | Un objetivo por orden de hasta 1920 × 1080, profundidad, un único muestreo y salida BGRA o BGRX. | Mezcla programable, superficies persistentes, más formatos y objetivos múltiples. El límite de 1920 × 1080 se refiere a esta ruta de dibujo C08. |
| VRAM, transferencias y residencia | qSSD respalda el espacio lógico disperso. La caché de transferencias reserva bajo demanda hasta 8 MiB y la de geometría hasta 128 páginas de 4 KiB. Las transferencias directas comprueban el rango y el permiso de bus master. | Conservar recursos nativos en el chip entre envíos y seguir su vida con versiones y fences. Actualmente un cuadro nuevo lee la imagen completa con glReadPixels y convierte RGBA a BGRA en CPU. |
| Controladores Windows y colas | El transporte WDM expone órdenes PCI. El UMD/KMD de render usa DDI WDDM 1.3, direcciones DMA, paginación, fences, MSI y preempción en límites de buffers. | Validar instalación y recuperación por timeout bajo carga dentro de Windows, concurrencia entre contextos y tiempos máximos de servicio. El ejecutable de referencia y los controladores tienen pruebas y alcances diferentes. |
| Cómputo y vídeo multimedia | Hay operaciones específicas, como suma de índices y acceso CPU/GPU al estado compartido de InfiniteCore. La representación de las puertas H y CX es una simulación clásica. | Una API general de compute necesita kernels, recursos de lectura/escritura, sincronización y despacho. Descodificación de vídeo y codificación por hardware requieren rutas propias, distintas de dibujar el escritorio. |
| Prestaciones avanzadas | El backend conserva una imagen confirmada por contexto de placa para la reutilización exacta de r105. | En un objetivo de escritorio Direct3D 12_2 también entrarían mesh shaders, sombreado de tasa variable y trazado de rayos. Son objetivos posteriores a texturas, índices, sombreadores y presentación eficiente. |
| Verificación en el chip | Android puede informar del backend HWUI, preparativos GLES y contadores separados de dibujos, bytes y reutilizaciones. Una consulta GPU se publica cuando hay resultado válido. | Medir un teléfono identificado, frecuencia real, P50/P95, actividad, ancho de banda y energía con la misma carga y condiciones térmicas. El reloj del dispositivo virtual no permite deducir esos valores. |

Microsoft especifica las capacidades por nivel de Direct3D, y Android publica perfiles de Vulkan para comparar familias de dispositivos. La recuperación TDR también impone obligaciones al controlador. [Niveles de Direct3D](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-devices-downlevel-intro) · [Perfiles Vulkan de Android](https://developer.android.com/ndk/guides/graphics/android-vulkan-profile) · [Recuperación TDR](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/timeout-detection-and-recovery)

## Cómo ayuda Éter a la GPU anfitriona

**Geometría seleccionada.** La nueva orden PCI 23, anunciada mediante el bit de capacidad 4096, toma vértices del área de preparación y una máscara del estado Éter compartido. El bit de menor peso del primer byte selecciona el primer triángulo. El invitado decide qué triángulos contribuyen a la escena. Solo los tramos seleccionados se copian al planificador de GPU-VIRTUAL y desde allí al buffer GLES. Se conserva el orden relativo. La geometría deseleccionada carece de efecto, incluso si contiene coordenadas sin usar. El transporte WDM expone esta operación y la utilidad de Windows incluye `prime_gpu_scene --ether-mask`.

Los bytes de máscara, incluidos los bits de relleno en el último byte, deben tener un valor de base definido. La lectura verifica la época del estado antes y después de recoger todas las ventanas. Una superposición o un cambio concurrente devuelve `RWGPU_QUANTUM_UNAVAILABLE` y conserva la imagen anterior. La orden no mide qubits. El invitado puede reintentar después de establecer una máscara definida.

**Reutilización exacta.** Para las rutas ordinarias de dibujo, C08 compara las dimensiones, el color de limpieza, el modo alfa y todos los bytes de los vértices visibles en orden. Cada coincidencia usa la imagen ya calculada. Se vuelven a entregar los píxeles al destino pedido, incluso si el invitado lo modificó entre órdenes. DMA, tickets, publicación de la revisión y MSI conservan sus efectos. La presión de memoria y el reinicio de motor invalidan la entrada. Al incorporar nuevos estados de mezcla, texturas o programas habrá que incluir sus identidades y versiones en la comparación.

**Vídeo de arranque.** Después de recibir un rectángulo opaco de pantalla completa, Android pinta el fondo solo en las bandas exteriores. Si la imagen ocupa toda la vista, omite esa limpieza. Una primera actualización parcial conserva el fondo completo. Los bordes con escalado fraccional mantienen su cobertura. El contador `host_vbe.background_pixels_skipped` mide el área interior excluida de la orden de fondo en coordenadas de la vista. El tráfico físico real también depende de cómo el controlador agrupe las operaciones. [Renderizado por teselas y ancho de banda, Khronos](https://docs.vulkan.org/guide/latest/tile_based_rendering_best_practices.html)

## Resultados y reproducción

Siete pares de ejecuciones con orden alternado, contexto GLES precalentado, 24 cuadros de 128 × 96 y 64 triángulos por cuadro. El programa compara cada byte de cada imagen con el control, incluidos los cuadros cuyo contenido cambia. El intervalo cronometrado comprende el envío de la orden x86 hasta que se completa. La descarga adicional de comprobación queda fuera del cronómetro. Estos tiempos de referencia son del host x86-64 con Mesa llvmpipe (LLVM 19.1.7).

| Carga | Cuadros ejecutados por GLES, control → Éter | Bytes de vértices, control → Éter | Bytes leídos de GLES, control → Éter | Mediana de las 24 órdenes, control → Éter |
| --- | ---: | ---: | ---: | ---: |
| Escena idéntica en los 24 cuadros | 24 → 1 | 92 160 → 3 840 | 1 179 648 → 49 152 | 38,47 → 28,65 ms |
| Fondo distinto en cada cuadro, 8 de 64 triángulos visibles | 24 → 24 | 92 160 → 11 520 | 1 179 648 → 1 179 648 | 38,25 → 37,69 ms |

El resultado transferible es la reducción de órdenes y bytes en estas dos cargas. En la segunda, la lectura de píxeles continúa en los 24 cuadros y el tiempo local varía poco. Las pruebas incluyen máscara de 257 bytes, puertas entrelazadas y su inversión, límites de VRAM, destinos sobrescritos y cambios individuales de dimensiones, alfa, color y geometría. Para obtener los tiempos y contadores reales de Adreno, Mali o PowerVR, hace falta capturar la misma carga en el dispositivo. [Contadores de GPU en Android](https://developer.android.com/agi/sys-trace/counters)

Después de sincronizar las dependencias fijadas y configurar CMake con EGL/GLES, se pueden reproducir las mediciones con:

```sh
cmake --build build --target reverse_windows_prime_gpu_guest_test
EGL_PLATFORM=surfaceless ./build/reverse_windows_prime_gpu_guest_test --measure-ether
```

La integración continua conserva los resultados por arquitectura en `prime-r105-gpu-x86_64` y `prime-r105-gpu-aarch64`. El código de la ruta se encuentra en `src/c08_guest_gpu.hpp`, el contrato PCI en `src/c08_gpu_protocol.h`, el controlador en `guest/windows/gpu_transport.c` y la presentación en `android/app/src/main/java/com/reversewindows/android/GuestMonitor.java`.
