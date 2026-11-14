# REVERSE Windows Prime C08

El adaptador de render PCI `00:03.0` tiene identificación experimental `1b36:c083`, clase `0302` y un BAR de 512 KiB con registros, comandos y un área de transferencia de 256 KiB. ACPI publica el rango PCI y el firmware excluye el rango de la RAM utilizable en E820. El controlador del invitado selecciona la dirección del BAR y habilita la decodificación. Los tickets de la cola son consecutivos desde 1. Un reinicio reinicializa el dispositivo y conserva los datos de las páginas ya confirmadas por QSSD.

GPU Virtual C12.3 valida y agrupa los triángulos en orden de dibujo. Descarta los que están completamente fuera de un plano de recorte y preserva los que cortan una arista. REgraphics crea un destino GLES2 con profundidad, traduce el intervalo de profundidad Direct3D al de GLES y devuelve BGRX de arriba hacia abajo. QSSD retiene el plan de geometría confirmado. Cuando el invitado pide presentar en el modo VBE activo, C07 detecta los píxeles resultantes como cualquier otra modificación del framebuffer.

## Capacidad y mediciones

El dispositivo publica **120.000.000.000 bytes lógicos** respaldados por páginas dispersas de 4 KiB en QSSD. Los volúmenes nuevos usan la capacidad exacta. Los volúmenes anteriores de 112 GiB se abren con el mismo límite visible para el invitado. Android abre el respaldo C08 existente y reutiliza el respaldo junto al disco de PrimeVGC cuando es el único disponible. El tamaño real del respaldo, los bytes de vértices, el tiempo de preparación y lectura, el temporizador del renderizador y su identificación de hardware se registran en `guest_gpu`. En hardware, `gpu_query_ns` depende de una consulta válida de `GL_EXT_disjoint_timer_query`.

La prueba del invitado x86 verifica descubrimiento PCI, tamaño y reasignación del BAR, el límite de VRAM y el dibujo de geometría subida directamente a VRAM. Comprueba píxeles, profundidad y recorte, después repite por el comando de dibujo original. Las órdenes de transferencia por ventana intermedia conservan su límite de 15 páginas y su prueba de 7,5 MiB.

## PrimeVGC en el adaptador C08

Los registros de C08 conservan la ABI 1. Los bits de capacidad nuevos anuncian estas órdenes en el BAR de C08.

| Orden | Origen | Destino o resultado |
| --- | --- | --- |
| `UPLOAD` (5) | RAM física del invitado | VRAM lógica, con confirmación QSSD |
| `DOWNLOAD` (6) | VRAM lógica | RAM física del invitado |
| `COPY` (7) | VRAM lógica | VRAM lógica, admite solapamiento |
| `SUM_INDICES` (8) | Conteo de 64 bits, hasta 2³² índices | Suma exacta en los registros de resultado |
| `DRAW_VRAM` (9) | Vértices en VRAM lógica | REgraphics y presentación VBE opcional |
| `READ_COLOR_RAM` (10) | Último destino BGRX renderizado | RAM física del invitado |

Las órdenes directas usan `OFFSET` para el origen, `AUX` para el destino y `LENGTH_HI:LENGTH` para el tamaño o conteo. `DRAW_VRAM` usa `AUX` para los indicadores de presentación. Todas las órdenes usan la misma secuencia de tickets. Las transferencias con RAM requieren PCI Bus Master y descartan los rangos reservados. PrimeVGC C03 amplía a **8 MiB** el tamaño máximo anunciado por orden directa. Las escrituras admiten hasta 2.048 páginas modificadas, incluido el desplazamiento inicial. Las órdenes de 512 KiB siguen disponibles.

PrimeVGC conserva hasta 8 MiB de páginas residentes. Si las páginas solicitadas están contiguas en la caché, el dispositivo copia directamente a RAM del invitado y registra esos bytes en `resident_download_bytes`. Android libera y deshabilita esa caché durante la presión de memoria.

QSSD agrupa registros contiguos en lecturas, calcula varios checksums FNV-1a en paralelo y actualiza el índice de bloques consecutivos en una sola operación por tramo. Al reabrir verifica el historial en lecturas de 1 MiB. Cada orden de escritura conserva una confirmación durable antes de devolver éxito. Los volúmenes anteriores se abren sin conversión. Para volver a un lector anterior a C03, la compactación fuera de línea genera un volumen en transacciones de hasta 128 páginas.

El invitado x64 ejecuta `reverse_windows_primevgc_transfer_test --bulk` para transferir 256 MiB densos a 108 GiB de desplazamiento, con 32 confirmaciones. `--bulk-512k` mueve los mismos bytes en 512 confirmaciones. Ambas pruebas comparan todos los bytes, comprueban el acceso al último byte de los 120 GB y rechazan corrupción al leer y al reabrir. Los registros dan tasas en GB/s decimales para escritura durable, reapertura, QSSD abierto con la caché VRAM vacía y descarga residente. El sistema anfitrión puede conservar páginas del archivo en su propia caché.

El mismo núcleo C12.3 conserva la exportación periódica y la transformación reversible XI de PrimeVGC. La prueba `reverse_windows_primevgc_compute_test` verifica la salida materializada, la suma de los 2³² índices y la transformación de 1.024 bits en CPU, con un oráculo de Mundo X. La suma exacta también se prueba mediante los registros del adaptador.

## Invitado Windows

`guest/windows/build.ps1` compila el transporte PCI de Windows x64 y ejecuta `prime_gpu_scene.exe --reference` desde un entorno x64 de Visual Studio. El ejecutable renderiza la escena de referencia mediante Direct3D 11 WARP y comprueba cuatro puntos elegidos por color, oclusión, recorte y fondo.

La ejecución normal de `prime_gpu_scene.exe` comprueba y usa DMA en el transporte WDM. Envía un bloque denso a la VRAM, recoge los mismos bytes, solicita el dibujo desde VRAM y compara los píxeles que el adaptador entrega directamente a RAM del invitado. El controlador solicita a Windows un búfer DMA común de hasta 8 MiB y comunica su tamaño efectivo. Las aplicaciones pasan desplazamientos lógicos de VRAM. El controlador programa únicamente la dirección DMA asignada por Windows. La bandera `--staged` prueba la ruta de compatibilidad del BAR.

El paquete del transporte sale sin firmar. La prueba normal del invitado requiere el sistema instalado y un paquete de controlador firmado para su entorno de prueba. El cierre de compatibilidad para apps Direct3D requiere el miniport de render WDDM y un UMD que el runtime pueda cargar, con asignaciones, contexto, DMA, completados y capacidades compatibles. `prime_gpu_scene.exe --wddm` exige el adaptador 1b36:c083 como dispositivo DXGI y repite la escena mediante Direct3D. CI comprueba que esa prueba rechaza otros adaptadores. La habilitación de DXGI, Direct3D a través de WDDM y la aceleración física en el Samsung permanecen pendientes.

[Arquitectura de WDDM](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/windows-vista-and-later-display-driver-model-architecture) · [Envío de comandos](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/submitting-a-command-buffer) · [Recursos PCI traducidos](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/raw-and-translated-resources).
