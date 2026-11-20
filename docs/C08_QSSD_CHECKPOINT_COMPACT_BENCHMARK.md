# C08 — QSSD persistente: índices verificables, checkpoints, compactación y mediciones

Fecha: 12 de noviembre de 2026, zona horaria del usuario; GitHub Actions registra ejecuciones del 13 de noviembre UTC.

## Objetivo y arquitectura

Se continúa estrictamente desde C07 en la rama `feature/c07-real-pci-ata-qssd-20261112` (PR #11) y se respetan el plan PR #7 y el CPU canónico de Mundo X PR #89. El firmware, la CPU, el bus PCI IDE/ATA, IRQ14 y el BIOS INT13 permanecen conectados al mismo `BlockDisk`.

Se amplía **`src/qssd_disk.hpp`**, el backend real de REVERSE Windows que implementa `BlockDisk` con `qssd::StateBus<Epoch>`. El repositorio original `Virtual-qSSD-` sigue en el SHA ya fijado; no se duplicó `StateBus` ni se importó un motor externo.

## Funcionalidad C08

### 1. Índice persistente de bloques y checkpoints

- `volume.rqvd.rqix`: archivo auxiliar descartable, escrito con fichero temporal, `fsync`, `rename` atómico y `fsync` del directorio. La imagen `.rqvd` conserva el formato de C03/C07 y sus bytes exactos; no se requiere una migración destructiva.
- El índice contiene generación del volumen, límite del COMMIT validado, estadísticas de journal, mapa ordenado `bloque lógico -> offset del registro`, y checksums de índice y prefijo de todo el diario hasta ese límite.
- La validación recorre la cabecera y el prefijo completo del journal, comprueba el COMMIT de anclaje, verifica cada entrada del índice y decodifica todos los bloques vivos. No confía únicamente en una firma del índice.
- Si el índice es ilegible, obsoleto o tiene checksum incorrecto, QSSD ignora ese acelerador y reconstruye el mapa desde el registro completo. Si se detecta corrupción en una transacción histórica terminada, la apertura **falla**; no devuelve datos falsos.
- Las transacciones posteriores al checkpoint son reproducidas sobre el índice reconstruido, sin perder el COMMIT más reciente. La recuperación continúa eliminando una cola de escritura incompleta.
- QSSD mantiene el historial de `qssd::StateBus` en ejecución. **Estos checkpoints conservan el estado del disco y su índice, no un snapshot completo de CPU/RAM/GPU.**

### 2. Compactación fuera de línea, segura y comprobable

- `QssdDisk::compact(path)` usa bloqueo exclusivo asesor de archivo `flock`: otro escritor no puede usar simultáneamente el mismo volumen bajo el mismo contrato.
- Recorre las entradas vivas del índice CoW, decodifica sus bytes, descarta representaciones que ya significan ceros y escribe una nueva imagen independiente con bloques RAW o patrones exactos.
- Agrupa como máximo 128 registros por transacción verificable, conserva bytes y capacidad lógica y genera una **nueva generación** de volumen; las imágenes antiguas con generación cero continúan abriéndose.
- Sin reemplazar el origen, hace `fsync` del candidato, lo abre independientemente y compara byte a byte los bloques activos. Después verifica que la ruta original sigue apuntando al inode esperado y ejecuta `rename` + `fsync` del directorio.
- Una caída antes del `rename` deja el fichero original intacto. Una caída durante la publicación deja como posibles estados el origen completo o el candidato completo, ambos previamente validados; la durabilidad exacta depende también del sistema de archivos del anfitrión.
- Los índices previos al compactado quedan obsoletos por generación. Tras reemplazar, se publica un checkpoint nuevo si es posible.
- Los contadores de `commits` del archivo compactado representan **transacciones de la nueva generación**; no equivalen al número histórico anterior. Las estadísticas de compactación devuelven ambos números.

### 3. Caché limitada, coherente con los estados

- Caché FIFO por offset de registro inmutable de **128 bloques × 4 KiB = 512 KiB de datos como máximo**.
- Se aprovechan bloques que el journal ya comprobó; las escrituras comprometidas pueden precargar su última versión. La política de solo agregar evita reutilizar offsets publicados mientras la misma generación esté montada.
- La memoria de datos tiene límite conocido; el índice radix CoW y otras estructuras siguen consumiendo RAM adicional. No se afirma un límite de memoria total del emulador.
- Contadores disponibles: aciertos, fallos de caché, bloques residentes y bytes residentes.

### 4. Interfaz de mantenimiento

```bash
reverse_windows_boot_probe --qssd-checkpoint --qssd-disk /ruta/disco.rqvd
reverse_windows_boot_probe --qssd-compact --qssd-disk /ruta/disco.rqvd
```

**Recomendación:** apagar/detener la máquina virtual y conservar una copia de seguridad antes de compactar una unidad importante. Los comandos usan bloqueo de volumen; no son procesos de mantenimiento en segundo plano. No modifican una ISO original.

## Validación con GitHub Actions

Workflow principal C08: `.github/workflows/c00-real-guest.yml`.

- **11/11 CTest**: los nueve ensayos C00–C07 (CPU x86/x64, disco RAW, QSSD, IDT, Ring3/TSS, motherboard y ATA PCI), nuevo ensayo de mantenimiento C08 y benchmark.
- `REVERSE_WINDOWS_C08_MAINTENANCE_PASS checks=365`: checkpoint válido, replay de cola, fallback de índice corrupto, rechazo de corrupción de un registro antiguo ya sobrescrito, compactación con inyección de interrupción antes de rename, generación nueva, índice obsoleto rechazado, checksum de cabecera y cola parcial.
- La prueba de compactación de mantenimiento redujo el **tamaño de journal** de `256976` B a `8424` B (96,7 % menos) con dos bloques activos no nulos. Son bytes del archivo de registro en esa prueba específica, no una relación universal de compresión.
- `REVERSE_WINDOWS_C07_REAL_ATA_PASS`: el mismo `BlockDisk` atiende IDENTIFY, READ/WRITE PIO y FLUSH por ATA e INT13 por BIOS, con IRQ14. No se tocaron CPU ni controladores.

## Benchmark RAW vs QSSD: resultados de una sola ejecución

Fuente: GitHub Actions x86_64, ejecución C08; fichero machine-readable `build/c08-measured-results.json` y log `build/reverse-windows-c08-benchmark.jsonl`.

Carga determinista de **128 escrituras de 4 KiB** (mezcla de datos de alta entropía, ceros, patrones y bloques sobrescritos) y **512 lecturas calientes de 4 KiB**, mismo orden y posiciones LBA en ambos discos. El backend RAW ejecutó `fstream::flush` seguido por un `fsync` explícito de descriptor tras cada escritura; QSSD ejecutó su `fsync` de transacción. Los buffers del sistema operativo **no** se vaciaron antes de las lecturas.

| Magnitud (una ejecución de CI) | RAW | QSSD |
|---|---:|---:|
| Tiempo para 128 escrituras con fsync | 10,989 ms | 23,430 ms |
| Tiempo de 512 lecturas calientes | 0,322 ms | 0,145 ms |
| Tiempo de apertura sin índice QSSD | 0,025 ms | 5,874 ms |
| Bytes asignados tras compactar | 176.128 B | 86.016 B |

Mediciones adicionales QSSD:
- Apertura con checkpoint validado: **2,527 ms** (vs 5,874 ms con replay completo en esta carga).
- Construcción de checkpoint: **1,318 ms**.
- Compactación fuera de línea, incluida verificación: **12,045 ms**.
- Tamaño del journal antes/después de compactar: **276.640 B → 83.809 B**.
- Índice auxiliar: **784 B**.
- Caché: **640 aciertos, 0 fallos** en fase observada, **524.288 B** de datos residentes.
- RSS máximo del **proceso completo del benchmark**, no de un backend individual: **5.664 KiB**.

**Interpretación:** en este único ensayo el QSSD tardó más en escribir de forma durable que RAW; fue más rápido en lecturas calientes y en apertura con índice que sin índice. Estas cifras son de un runner de GitHub Actions, **no** representan latencia en Android, SSD físico ni rendimiento general de Windows 10. No se fija ningún umbral de velocidad para aprobar CI.

## Límites y siguientes pasos

- La verificación de integridad emplea el checksum no criptográfico FNV64; no ofrece resistencia a modificaciones maliciosas dirigidas ni equivale a SHA-256.
- El checkpoint acelera la reconstrucción del índice, pero comprueba el prefijo completo del journal para no ocultar daños históricos; por tanto no elimina toda la E/S de apertura.
- La compactación mantiene **un segundo archivo temporal** hasta verificarlo, por lo que necesita espacio libre real suficiente. Puede dejar una copia de staging tras una caída abrupta; esa copia no se elige automáticamente como volumen de arranque.
- No se han persistido snapshots coherentes de CPU/RAM/dispositivos junto con el disco, ni se ha hecho prueba de pérdida real de alimentación.
- No hay `Windows PE`, instalador de Windows 10 ejecutándose ni métricas en un Samsung Android ARM64. Faltan funciones CPU del sistema invitado, ACPI/APIC completos y otras compatibilidades.
- C09 debe repetir un arranque de ISO original local y usar trazas reales; C10 requerirá instalar y reiniciar desde QSSD.

**Estado:** C08 tiene pruebas nativas y métricas reproducibles, pero no representa aún una computadora capaz de instalar Windows 10. PR encadenada y en borrador, sin fusiones automáticas.
