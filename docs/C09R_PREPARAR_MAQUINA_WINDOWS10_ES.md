# REVERSE Windows — C09R: preparar hardware antes de instalar Windows 10 x64 en español

Fecha: 2026-11-12. En GitHub Actions la fecha puede figurar como 2026-11-13 UTC.

## Contrato

- La ISO de Windows 10 x64 en español es del usuario. No descargarla, subirla, reconstruirla ni ejecutarla desde nuestros repositorios.
- Una sola máquina: cosmic_pc::PcMachine de Mundo X ejecuta x86 en Android ARM64. No hay un segundo motor de CPU ni aceleración x86 de hardware ARM.
- GPU Virtual, QSSD, Ether/XI y la placa base deben aportar trabajo real dentro de esa misma máquina, sin ventanas de demostración falsas.
- No fusionar automáticamente las PR; no prometer APK ni Windows instalado sin las pruebas correspondientes.

## Requisitos investigados

Fuentes oficiales:
- Microsoft, Windows 10 Minimum Hardware Requirements: https://download.microsoft.com/download/c/1/5/c150e1ca-4a55-4a7e-94c5-bfc8c2e785c5/Windows%2010%20Minimum%20Hardware%20Requirements.pdf
- Windows 10 system requirements: https://support.microsoft.com/en-us/windows/experience/windows-10-system-requirements
- Boot to UEFI or Legacy BIOS: https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/boot-to-uefi-mode-or-legacy-bios-mode?view=windows-10
- Microsoft Basic Display Driver: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/microsoft-basic-display-driver
- Intel SDM, initialización SSE2, FXSAVE/FXRSTOR y CR4.OSFXSR: https://cdrdv2-public.intel.com/671200/325462-sdm-vol-1-2abcd-3abcd.pdf

Windows 10 x64 requiere CPU x64 con PAE, NX, SSE2, CMPXCHG16B, LAHF/SAHF y PREFETCHW; 2 GiB de RAM, y 32 GiB o más de almacenamiento para versiones recientes. El objetivo inicial de REVERSE Windows mantiene BIOS heredado, MBR y QSSD de 64 GiB lógicos, sin forzar UEFI/GPT. La pantalla mínima es 800x600; que exista VBE no certifica driver Windows funcional.

## Auditoría del hardware real

| Área | Verificado | Pendiente para Windows 10 |
|---|---|---|
| Mundo X CPU | Motor x86/x64, algunos modos protegidos, Ring0/3, MMU/IDT e interrupciones; C09R implementa CX8, CX16 y PREFETCHW | SSE2, FPU/x87, XMM, MXCSR, FXSAVE/FXRSTOR, CR4.OSFXSR, más ISA y conformidad del kernel |
| Ether/XI | InfiniteSession y ventana de memoria Ether conectadas al MISMO PcMachine | Evitar alterar bytes o falsear capacidad RAM; comprobar presupuesto residente |
| QSSD | Un BlockDisk persistente con StateBus, journal, checksums, compactación y índice | Espacio físico suficiente en Android, recuperación CPU/RAM/dispositivos y datos del usuario |
| PCI / ATA | Controlador IDE PIO con IRQ14 y BIOS INT13 sobre el mismo QSSD | DMA/AHCI opcionales según drivers, temporizadores y routing completos |
| Firmware | El Torito/BIOS, PCI, PIC/APIC y ACPI parciales | ACPI/AML, servicios EDD, tablas y temporizadores realmente compatibles |
| VGA / GPU | Framebuffer generado por instrucciones del invitado; GPU Virtual selecciona cambios VGA; QSSD GraphicsWorkMemory registra el trabajo | Adaptador gráfico Windows, BasicDisplay/WDDM no verificados; REgraphics y presentación Android pendientes |
| Android | Fuentes de componentes ARM64/Android en repos existentes | Producto APK integrado, SAF para seleccionar ISO local, entrada y render real, rendimiento/temperatura |

**Las banderas SSE2 y x87 permanecen apagadas en CPUID** hasta que realmente existan las instrucciones y todo su estado arquitectónico. No basta con declarar un feature bit.

## Implementación verificable de C09R (sin ISO)

1. Motor CPU canónico Mundo X: CMPXCHG8B y CMPXCHG16B reales; prueba de coincidencia, diferencia, alineación, ZF, estado de registros y memoria; prefijo LOCK limitado a los opcodes realmente disponibles.
2. PREFETCHW como pista no-faulting que no crea RAM ni realiza E/S. CPUID anuncia sólo PAE, NX, CX8/CX16, LAHF/SAHF, PREFETCHW y modo largo probados.
3. Verificación del perfil x64 de 2 GiB lógicos con RAM dispersa de MemoryFabric sin asignación total anticipada.
4. Prueba de puerto PCI por instrucciones x86 sobre el MISMO PcMachine, con controladora C07 visible, y una escritura QSSD 512B recuperada tras reabrir.
5. Ventana Ether/XI real comprobada mediante lectura/escritura de bytes de la misma sesión del PcMachine.
6. GPU-VIRTUAL y qssd::GraphicsWorkMemory consumen el VGA real de la CPU. En el test, el huésped modifica una celda y se genera una actualización exacta de 4 bytes en vez de copiar todo el framebuffer de 4000 bytes. Esto es reducción de bytes planeados para presentación, **no aceleración física de GPU demostrada ni driver Windows**.
7. Ejecutable reverse_windows_c09r_hardware_preflight --self-test no acepta ISO. Devuelve JSON de capacidades verificadas y always ready_for_windows10_install=false mientras falte SSE2 y demás compatibilidad.

## Ruta corregida de los ciclos, antes de instalar Windows

| Secuencia | Objetivo | Criterio de cierre |
|---|---|---|
| **C09R-1 CPU esencial** | CX16/PREFETCHW ya implementados. Completar SSE2/XMM/MXCSR, FPU/x87, FXSAVE/FXRSTOR y CR4.OSFXSR | Pruebas de instrucciones reales, contexto/rollback; activar SSE2 CPUID solamente al pasar la suite completa |
| **C09R-2 Kernel y MMU** | Extender syscalls y estados privilegiados, paging/PAE/IDT/IST, LOCK/memoria coherente | Microkernel de prueba ejerce Ring3/Ring0, páginas y contexto FPU/SSE2 con bytes exactos |
| **C09R-3 Placa base y firmware** | ACPI/FADT/AML, PIC/APIC, PIT/RTC/HPET y tiempos calibrados, PCI IDE/ATA | Huésped de prueba enumera hardware, recibe IRQ y verifica ciclos/timers y reboot |
| **C09R-4 Pantalla e interacción** | VBE framebuffer real, GPU Virtual selecciona trabajo, REgraphics presenta; teclado y touch | Un programa x86 dibuja en el invitado y la pantalla Android presenta EXACTAMENTE ese contenido |
| **C09R-5 Android** | APK ARM64/NDK con un solo PcMachine, ISO local sólo seleccionada por el usuario, disco QSSD, pausa/reinicio y diagnóstico | Compilar APK, ejecutar huéspedes x86 propios, medir recursos; sin Windows antes de la instalación del usuario |
| **C09R-6 Entrega** | Perfil BIOS/MBR y QSSD 64 GiB, diagnóstico PASS con todas las capacidades comprobadas | Artefacto Android funcional y checklist de usuario; **el usuario instala su Windows 10 en español** |

## Flujo real de tu tecnología

Android ARM64 -> REVERSE Windows -> PcMachine (única CPU Mundo X + MemoryFabric)
- Ether/XI = ventana de estados de la sesión original, integrada en esa misma CPU.
- Placa base PCI/IDE -> BlockDisk único -> QSSD .rqvd + qssd StateBus.
- PcVideo/VGA del huésped -> GPU Virtual selecciona celdas/tareas -> qssd GraphicsWorkMemory conserva revisión -> REgraphics/Android pendiente de integración final.

Un estado compacto sólo cuenta como mejora si conserva bytes y reduce RAM, trabajo o transferencias **medidos**. El espacio lógico QSSD no inventa espacio físico; GPU Virtual no sustituye el driver Windows.

## Estado de salida

- Ramas/PR permanecen en borrador, sin merge.
- Pruebas: Mundo X CPU atómica, perfil CPUID honesto y suite nativa; REVERSE Windows preflight sin ISO, Ether, GPU, QSSD, ATA y regresiones C04-C09.
- Capacidad x64 todavía incompleta por SSE2/x87/contexto de CPU y firmware/video. Por ello la máquina se informa explícitamente como NO LISTA para instalar Windows 10.
- La ISO del usuario no se ha tocado ni ejecutado, y NO se declara Windows PE ni Windows 10 arrancado.
