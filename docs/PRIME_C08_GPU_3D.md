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

Las órdenes directas usan `OFFSET` para el origen, `AUX` para el destino y `LENGTH_HI:LENGTH` para el tamaño o conteo. `DRAW_VRAM` usa `AUX` para los indicadores de presentación. Todas las órdenes usan la misma secuencia de tickets. Las transferencias con RAM requieren PCI Bus Master y descartan los rangos reservados. Cada orden directa mueve hasta 512 KiB, con un máximo de 128 páginas modificadas por escritura.

PrimeVGC conserva hasta 8 MiB de páginas residentes. Si las páginas solicitadas están contiguas en la caché, el dispositivo copia directamente a RAM del invitado y registra esos bytes en `resident_download_bytes`. Android libera y deshabilita esa caché durante la presión de memoria. La prueba `reverse_windows_primevgc_transfer_test` ejecuta instrucciones x64 y reproduce el trabajo de PrimeVGC C02 a 108 GiB de desplazamiento. Transfiere 8 MiB por pasada en bloques de 512 KiB y verifica todos los bytes. Los registros de CI dan tres tasas en GB/s decimales, para subida durable, primera descarga tras reabrir el volumen y descarga residente. La primera descarga incluye la reconstrucción del índice y puede aprovechar la caché del sistema anfitrión.

El mismo núcleo C12.3 conserva la exportación periódica y la transformación reversible XI de PrimeVGC. La prueba `reverse_windows_primevgc_compute_test` verifica la salida materializada, la suma de los 2³² índices y la transformación de 1.024 bits en CPU, con un oráculo de Mundo X. La suma exacta también se prueba mediante los registros del adaptador.

## Invitado Windows

`guest/windows/build.ps1` compila el transporte PCI de Windows x64 y ejecuta `prime_gpu_scene.exe --reference` desde un entorno de compilación x64 de Visual Studio. El ejecutable renderiza la escena de referencia mediante Direct3D 11 WARP y comprueba cuatro puntos elegidos por color, oclusión, recorte y fondo. Su ejecución normal solicita la misma escena al controlador PCI propio y compara la salida con la referencia. El controlador solo acepta esta operación de render acotada. Usa el recurso BAR traducido que entrega PnP.

El paquete del transporte sale sin firmar. La prueba normal del invitado requiere el sistema instalado y un paquete de controlador firmado para su entorno de prueba. El cierre de compatibilidad para apps Direct3D requiere el miniport de render WDDM y un UMD que el runtime pueda cargar, con asignaciones, contexto, DMA, completados y capacidades compatibles. `prime_gpu_scene.exe --wddm` exige el adaptador 1b36:c083 como dispositivo DXGI y repite la escena mediante Direct3D. CI comprueba que esa prueba rechaza otros adaptadores. La habilitación de DXGI, Direct3D a través de WDDM y la aceleración física en el Samsung permanecen pendientes.

[Arquitectura de WDDM](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/windows-vista-and-later-display-driver-model-architecture) · [Envío de comandos](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/submitting-a-command-buffer) · [Recursos PCI traducidos](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/raw-and-translated-resources).
