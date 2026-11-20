# GPU C01 · Base PCI de REVERSE Windows

La interfaz actual del dispositivo se describe en [GPU C02](GPU_C02_VRAM_120GB.md). Este documento conserva la descripción de la revisión de C01.

El ciclo conecta el motor de GPU Virtual C12.1 con un dispositivo al que un programa invitado puede acceder. Parte del candidato de cálculo de la PR #35 y usa las revisiones del lockfile.

## Dispositivo

La función PCI 00:03.0 se enumera mediante los puertos CF8/CFC. Usa el identificador local de prototipo VEN_FFFE y DEV_C101 y la clase 0380h, controlador de pantalla de otro tipo. BAR0 declara una región MMIO de 4 KiB que empieza en FEC10000h. La raíz PCI ofrece en ACPI el rango FEC10000h–FEC1FFFFh. El huésped puede sondear el tamaño de BAR0 y reubicarlo en ese rango. El bit 1 del registro Command de PCI habilita la decodificación de memoria, que arranca desactivada.

### Registros de BAR0

| Desplazamiento | Acceso | Significado |
| --- | --- | --- |
| 00h | Lectura | Versión de interfaz. Valor 1. |
| 04h | Lectura | Capacidades. Bit 0 para copia y bit 1 para suma. |
| 08h | Lectura | Estado. 0 en reposo, 1 correcto, 2 rango inválido, 3 orden desconocida, 4 recursos insuficientes. |
| 0Ch | Escritura | El byte inferior ejecuta la orden. 1 copia, 2 suma. |
| 10h / 14h | Lectura y escritura | Origen y destino de copia, relativos a la ventana de datos. |
| 18h / 1Ch | Lectura y escritura | Conteo de 64 bits, palabra baja y alta. |
| 20h / 24h | Lectura | Resultado de 64 bits, palabra baja y alta. |
| 400h–BFFh | Lectura y escritura | Ventana de datos con 2 KiB volátiles. |

Los campos usan orden de bytes little-endian. Escribe los parámetros antes del registro de orden y consulta el estado antes de leer el resultado. La ejecución es síncrona. El resultado se limpia al recibir cada orden y se publica al completarla. El reinicio limpia la ventana y restablece BAR0 y el permiso de decodificación.

- La copia mueve exactamente el conteo de bytes indicado y admite solapamientos. Devuelve el número de bytes copiados. Un rango inválido deja intacta la ventana.
- La suma devuelve la suma sin signo de los índices desde cero hasta un elemento antes del conteo. Admite hasta 2^32 índices. Usa la reducción afín exacta de GPU Virtual C12.1 y devuelve 9.223.372.034.707.292.160 cuando el conteo es 2^32.

## Criterio de cierre

La prueba de placa madre ejecuta las órdenes mediante instrucciones x86 y x64 del invitado, usando su propio mapa de páginas en x64. Comprueba la clase PCI, el sondeo y la reubicación de BAR0, la copia de bytes, la suma exacta, el rechazo de rangos inválidos y el reinicio. Verifica además la coexistencia con disco y unidad óptica.

El adaptador utiliza el simulador y la CPU anfitriona. La salida de firmware VGA/VBE continúa por su ruta existente. La ventana de 2 KiB es volátil y el respaldo QSSD de 120 GB lógicos sigue siendo un prototipo aparte. Quedan pendientes su integración con el administrador de memoria gráfica, un controlador WDDM, presentación de pantalla desde ese controlador y ejecución 3D.
