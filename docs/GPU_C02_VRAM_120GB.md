# GPU C02 · VRAM de 120 GB y transferencias PCI

El adaptador PCI 00:03.0 dispone de 120.000.000.000 bytes lógicos, repartidos en 29.296.875 páginas de 4 KiB. El primer byte tiene dirección 0 y el último, 119.999.999.999. Los rangos sin escribir devuelven ceros. BAR0 presenta registros de control de la GPU y la aplicación invitada solicita las transferencias en bloques.

En sesiones con disco, la máquina abre el volumen gráfico QSSD independiente junto al disco del sistema y lo crea al primer acceso. El disco del sistema conserva su formato y tamaño. Cada escritura de VRAM se confirma después de publicar una transacción QSSD con sincronización al almacenamiento. Para sesiones sin disco se usa un volumen temporal que se descarta al reiniciar el dispositivo. Hasta 8 MiB de páginas completas pueden quedar en una caché residente. Android libera esa caché al detectar presión de memoria.

## Interfaz ABI 2

La detección PCI, la región BAR0 de 4 KiB, la dirección inicial y la reubicación dentro del rango ACPI siguen la especificación de C01. Los registros son little-endian. Escribe los campos de la orden antes del byte inferior de 0Ch. Cuando termina la escritura, puedes consultar el estado en 08h y el resultado en 20h y 24h.

| Registro | Desplazamiento | Significado |
| --- | --- | --- |
| ABI | 00h | Valor 2 |
| Capacidades | 04h | Copia, suma, subida y bajada en los bits 0 a 3 |
| Estado | 08h | 0 en reposo, 1 completo, 2 rango inválido, 3 orden desconocida, 4 no disponible |
| Orden | 0Ch | 1 copia dentro de VRAM. 2 suma de índices. 3 subida de RAM a VRAM. 4 bajada de VRAM a RAM. |
| Origen | 10h / 28h | Dirección de 64 bits, palabra baja y alta |
| Destino | 14h / 2Ch | Dirección de 64 bits, palabra baja y alta |
| Conteo | 18h / 1Ch | Tamaño de 64 bits en bytes, o número de índices para la suma |
| Resultado | 20h / 24h | Bytes confirmados o suma exacta, palabras baja y alta |
| Capacidad | 30h / 34h | Tamaño de la VRAM en bytes, palabras baja y alta |
| Transferencia máxima | 38h | 524.288 bytes por orden alineada. Una escritura que empieza dentro de una página debe contar ese desplazamiento en el límite de 128 páginas. |

Las órdenes que tocan RAM requieren el bit Bus Master del registro Command de PCI y reciben direcciones físicas de RAM del invitado. Las regiones de MMIO, ROM y vídeo de firmware quedan fuera del espacio de RAM aceptado. La copia admite solapamientos. Los rangos fuera de capacidad se rechazan antes de la escritura y el resultado se limpia al iniciar cada orden. El ancho de los registros de dirección permite acceder a toda la VRAM con un programa x86 o x64.

## Comprobación de ancho de banda

La prueba release ejecuta instrucciones x64 para transferir 8 MiB únicos por pasada en bloques de 512 KiB, por encima de 4 GiB en VRAM. Cronometra la subida con confirmación QSSD, la primera descarga tras volver a montar el volumen y la descarga desde páginas residentes. Comprueba la igualdad byte por byte, verifica el último byte disponible y rechaza la dirección siguiente. La primera descarga incluye reapertura y reconstrucción del índice. Los datos del archivo pueden seguir en la caché del sistema anfitrión.

El flujo de GitHub publica las tres mediciones en GB/s decimales. NVIDIA publica 1.792 GB/s teóricos para el bus de memoria GDDR7 de la RTX 5090. La comparación distingue el pico de ese bus, el transporte entre RAM invitada y VRAM virtual y el coste de confirmar las escrituras en QSSD. [Especificación de NVIDIA](https://www.nvidia.com/en-us/geforce/graphics-cards/50-series/)

La capacidad se comprueba desde código invitado. Para que Windows la use como memoria gráfica administrada todavía se requiere el controlador WDDM y una prueba en el sistema invitado.
