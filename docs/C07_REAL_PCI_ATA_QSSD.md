# C07 — Controladora PCI IDE/ATA real conectada al mismo QSSD

Fecha de implementación: 12 noviembre 2026 (GitHub Actions también puede registrar 13 noviembre UTC).

## Objetivo y arquitectura

Una sola máquina `cosmic_pc::PcMachine` (Mundo X) sobre Android ARM64. La BIOS INT 13h y el dispositivo PCI ATA moderno apuntan **al mismo objeto `BlockDisk`**. No existe un disco sombra, una emulación gráfica de Windows ni una segunda CPU.

```text
HUÉSPED x86 real (IN/OUT/REP INSW/OUTSW)
               │
PCI 0:2.0 clase 0x0101 (IDE, legacy)
               │
Primario ATA: 1F0–1F7 / 3F6 (PIO, IRQ14)
               │
ATA C07   -- IRQ14 --> PIC esclavo IRQ6 --> PIC maestro IRQ2 --> IDT/IRETD
               │
BlockDisk & disk  <--------------- BIOS INT13 DL=80h
               │
QssdDisk .rqvd (journal, commit, fsync, Estado QSSD)
     o DiskImage .rvdisk (sistema de archivos anfitrión)
```

## Funcionalidad probada

1. `PcMotherboardC06` publica el dispositivo PCI IDE sólo si se conectó un `BlockDisk`. ID PCI sintético `1b36:c071`, clase `0x0101`, modo legacy, IRQ14, I/O habilitado; sin afirmar DMA ni SATA AHCI.
2. `AtaC07` implementa task file de 8 registros, alt status/registro de control, selección de master, ATA status DRDY/DSC/DRQ/ERR, identificador del fabricante y la geometría/capacidad desde el BlockDisk real. El slave está ausente.
3. **IDENTIFY DEVICE (0xEC):** 256 palabras con modelo, serial, 512 B por sector, LBA28 y LBA48; no anuncia UDMA inexistente.
4. **READ SECTORS (0x20) y WRITE SECTORS (0x30):** FIFO PIO de un sector de 512 bytes a la vez, multi-sector con decremento de contador e IRQ por disponibilidad/completado. Se prueban instrucciones de huésped `REP INSW/OUTSW` con 2 sectores.
5. **READ SECTORS EXT (0x24), WRITE SECTORS EXT (0x34):** direccionamiento de 48 bits hacia QSSD incluso por encima de la capacidad LBA28. Comprobado en el sector lógico `0x10000020` de un disco disperso de 256 GiB.
6. **FLUSH CACHE (0xE7) y EXT (0xEA):** usan `BlockDisk::flush()`. QSSD sincroniza el descriptor real mediante `fsync`; sus escrituras ya confirmaban el journal con `fsync` antes de reconocer éxito. `DiskImage` sólo ofrece `fstream::flush`, y por tanto **no garantiza durabilidad ante pérdida de alimentación del anfitrión**.
7. Los comandos inválidos devuelven ATA `ABRT=0x04`, accesos fuera de capacidad devuelven `IDNF=0x10`, errores del almacenamiento devuelven ATA `UNC=0x40`/ERR; no se fabrican sectores.
8. **IRQ14 real** mediante la cascada de dos PICs: IRQ6 del esclavo, IRQ2 del maestro; el huésped maneja la interrupción en IDT y envía EOI a ambos PIC por instrucciones x86. Se conservan registros del invitado (EAX/EDX) durante la ISR.
9. El registro de control SRST aborta un PIO incompleto antes de escribirlo en QSSD. La reanudación/reinicio de `PcMachine` restaura el dispositivo ATA, mantiene la misma unidad y no altera sus sectores.
10. El `reverse_windows_boot_probe` monta una sola instancia del BlockDisk y la pasa a BIOS y motherboard. El JSON muestra contadores de ATA, transferencias, flush e IRQ14 sin declarar arranque Windows.

## Pruebas y criterios de cierre

`tests/c07_ata_guest_test.cpp` en CMake/CTest:

- Código invitado PCI configura CF8/CFC y verifica ID `c071:1b36`, clase `0x0101`.
- Código invitado emite IDENTIFY y transfiere las 256 palabras usando `REP INSW`.
- Código invitado envía datos desde su RAM por `REP OUTSW` a un QSSD; `REP INSW` recupera los mismos 512 bytes.
- El invitado programa PICs, recibe IRQ14 y ejecuta EOI e IRETD.
- Dos sectores consecutivos se escriben/leen en un mismo comando PIO y se recuperan del journal tras reinicio.
- Pruebas LBA48 `READ/WRITE EXT` para un sector fuera de la ventana LBA28.
- El invitado emite FLUSH/FLUSH EXT; se verifica que QSSD reabre todos los sectores con bytes idénticos y cantidad exacta de commits.
- Un WRITE parcial cancelado por SRST nunca alcanza el archivo QSSD.
- Una dirección fuera de la capacidad y comando desconocido generan IDNF/ABRT.
- Una imagen QSSD copiada y corrompida se rechaza al recuperar; una cola de transacción incompleta se poda sin admitir la escritura.
- Una nueva máquina BIOS usa INT 13h DL80 para leer bytes escritos por ATA en la **misma** unidad.
- Se conservan las 7 pruebas C00–C06, más dos pruebas independientes de C07 (ATA/QSSD y CPU REP PIO); suite total **9/9** en GitHub Actions. 
- Prueba C07 ATA/QSSD: **1.068 comprobaciones**; prueba de CPU Mundo X: **527 comprobaciones**. Los contadores no son mediciones de rendimiento.
- En el disco QSSD de prueba de 256 GiB lógicos, cuatro escrituras sectoriales originadas por instrucciones x86 se recuperaron tras reabrir el registro, con **16.896 bytes de archivo físico** en esa ejecución. Esto no implica compresión universal ni un SSD de 256 GiB físicamente disponible.

Archivo CI: `.github/workflows/c00-real-guest.yml`, con log y artefactos de ATA real. Pin SHA de Mundo X incluido en `config/dependencies.lock.json`; toda la compilación usa el intérprete original, no una CPU alternativa. El repositorio Mundo X amplía sus tests nativos para `REP INSW/OUTSW` y el contrato de ancho/uno por paso.

## Limitaciones precisas

- Todavía NO hay Windows 10 ni Windows PE ejecutados ni disco detectado por un Windows real. El dispositivo es **estándar a nivel de clase/puertos**, pero la compatibilidad Windows queda pendiente de pruebas con el instalador auténtico.
- No hay DMA, bus mastering IDE, UDMA, AHCI/SATA, canal secundario operativo, ATAPI, IDENTIFY PACKET, MSI/MSI-X, APIC completo o timing físico del 8259/PIT.
- PIO de un sector por fase; las transferencias largas se realizan secuencialmente, QSSD hace sincronización frecuente y el rendimiento no está medido. No se atribuye aceleración cuántica física.
- Las operaciones complejas de PCI BAR sizing, ACPI/AML completo, drivers Windows, IRQ cascadas especiales y modos mixtos requieren más compatibilidad.
- La promesa lógica de 16 TiB para QSSD exige particionado GPT/UEFI para usar todo el volumen en Windows; el primer Windows BIOS/MBR debe utilizar un volumen de tamaño razonable, por ejemplo 64 GiB.
- No se subió una ISO privada, disco físico, datos de usuario o APK no verificada.

**Sin merge automático.** La PR C07 se encadena sobre la PR #10 C06, y la evolución de CPU en Mundo X se encadena sobre PR #88.
