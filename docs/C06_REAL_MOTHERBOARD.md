# C06 — Placa base virtual conectada: PCI, PIC/PIT, RTC, MMIO, APIC y ACPI

**Fecha:** 12 noviembre 2026. **Objetivo real:** ofrecer a la CPU canónica de Mundo X un bus de dispositivos y un firmware de descubrimiento comprobables desde instrucciones x86 reales, preservando QSSD y los ciclos C04/C05.

## Integración exacta

- Autoridad única: `cosmic_pc::PcMachine` de Mundo X, sin segunda CPU. Código C06 MMIO/IRQ en `x86_cpu.hpp` y hooks de polling/reset en `pc_machine.hpp`.
- El SHA exacto de Mundo X se fija en `config/dependencies.lock.json` y el checkout de CI. Las dependencias GPU Virtual, QSSD y REVERSE-PC permanecen en sus SHA anteriores.
- El ejecutor `src/reverse_windows_boot_probe.cpp` crea **una sola** instancia de `PcMotherboardC06` después de montar el medio de arranque, BIOS HDD y QSSD, y la mantiene durante toda la ejecución.
- `src/c06_motherboard.hpp` encadena los hooks de I/O y MMIO; las operaciones no reclamadas continúan llegando a la CPU/BIOS/Ether originales. Los registros de dispositivo no se disfrazan de RAM. La ruta MMIO dentro de una página ausente sigue la traducción/paginación del huésped y no ignora la MMU.
- `src/c06_acpi.hpp` instala firmware ACPI en la región E0000 (E820 la declara reservada). El `device_reset_hook` vuelve a instalarlo y reinicializa dispositivos después de un reset de máquina real.

## Dispositivos C06 y pruebas del huésped

| Ruta | Función comprobada | Límites expresos |
|---|---|---|
| PCI 0xCF8 / 0xCFC | El invitado ejecuta OUT/IN x86 en bus0/dev0 host bridge y recibe ID, consulta dev31 y obtiene 0xffffffff | Sólo host bridge/ISA bridge; no hay controlador IDE/AHCI PCI hasta C07 |
| PIC maestro 8259 | ICW1-4, máscara, EOI, IRQ0 PIT→IDT→IRETD, contextos reales | Secundario expuesto parcialmente; prioridad avanzada, spurious y cascada no validadas |
| PIT canal 0 | Escribir control 0x43 y divisor 0x40; generar IRQ0 periódica por ticks de intérprete | NO equivale a 1.193182 MHz físicos ni permite calibrar Windows todavía |
| RTC/CMOS | Leer fecha UTC del anfitrión en BCD/24h por 0x70/0x71, status A/B/D | Periodic RTC/IRQ8 y todo CMOS/alarma sin implementar |
| APIC local + IOAPIC | MMIO `FEE00000` y `FEC00000`, ID/version/TPR/SVR/EOI y redirección GSI0 a IRQ31, manejador x86 en IDT | Modelo mínimo, sin IPIs/SMP, APIC arbitraje ni conformidad xAPIC total |
| ACPI | RSDP 2.0 con checksum base/ext, RSDT/XSDT, FADT/MADT/DSDT con checksum, reinicio, _S5 y PM control I/O | No AML completo, PCI IRQ routing AML, SCI completo ni soporte Windows probado |

## Pruebas

`tests/c06_motherboard_guest_test.cpp` se compila como séptima prueba CTest y ejerce instrucciones de máquina reales:
1. ICWs y PIT programados por código x86, entrada de IRQ0 por PIC, ISR con EOI, retorno IRETD.
2. Configuración PCI por `OUT DX,EAX` y `IN EAX,DX` con valores reales; RTC/CMOS por puertos; lectura de la firma RSDP por MOV del huésped; solicitud S5 por `OUT DX,AX`.
3. Verificación independiente de checksums y punteros de todas las tablas ACPI, y reposición tras reiniciar `PcMachine`.
4. Programación MMIO APIC/IOAPIC por `MOV DWORD PTR [addr],imm32`, enrutamiento GSI0 IRQ31, EOI y máscara de la IRQ en el propio manejador x86.
5. Lectura de un registro LAPIC no implementado debe **fallar** en lugar de devolver un dato inventado.
6. Regresiones C00-C05: CPU x86/x64, disco RAW, QSSD persistente y pruebas IDT/CPL3/TSS. Se reproduce ISO sintética El Torito; ninguna ISO privada se sube.

CI: `.github/workflows/c00-real-guest.yml` conserva el CTest completo y emite `build/reverse-windows-c06-motherboard.log` con marcadores de éxito junto a la evidencia de la CPU x64.

## Pendientes para arrancar Windows 10

- PCI device controller para disco de Windows (IDE PIO en C07 primero) y vínculo al MISMO `BlockDisk` QSSD existente; BIOS INT13 actual sólo sirve al firmware.
- Completar interrupciones PIC de cascada y APIC/IOAPIC de producción, PIT/HPET/RTC calibrados y entrega de IRQ según kernel. No afirmar exactitud temporal hasta pruebas comparativas.
- Ampliar ACPI FADT/AML y recursos PCI/PNP validados por sistema invitado; la presencia de tablas con checksum válido **no** acredita soporte Windows.
- CPU todavía carece de SSE2/x87, instrucciones del kernel, IRQ/IDT avanzadas y semántica IA-32e completa; no existe Windows PE ni Windows 10 iniciado.
- Sin mediciones de rendimiento, energía o temperatura en Galaxy A16; no hay APK del emulador en este ciclo y no se promete aceleración real.

**Integración física lógica:** QSSD sigue como backend de almacenamiento, Mundo X como CPU única, y C06 como controlador de periféricos probado. No se fusionan PR automáticamente.
