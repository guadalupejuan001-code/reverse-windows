# C05 — Ring3 real, TSS de kernel y permisos U/S

Fecha: 2026-11-12. Rama experimental encadenada sobre C04 PR #8. Sin merge automático.

## Dependencia canónica

REVERSE Windows utiliza `PcMachine` original de Mundo X y no introduce CPU paralela ni modifica QSSD. Las referencias de `config/dependencies.lock.json` y GitHub Actions fijan el mismo commit C05 del motor para garantizar que la implementación probada sea la que ejecuta REVERSE Windows. Los demás repositorios (QSSD, GPU-VIRTUAL, REVERSE-PC) siguen en sus referencias previas.

## Funciones del invitado

- CPL0 y CPL3 con descriptores GDT DPL/RPL, EFLAGS IOPL y controles de privilegio de instrucciones.
- Entrada real al kernel desde usuario por INT con puerta IDT y pila de TSS (RSP0 en x64, SS0:ESP0 en protegido32).
- Salida por IRETQ/IRETD a Ring3, recuperando CS, SS, RIP/EIP, RSP/ESP y FLAGS.
- Accesos a páginas con P=1 y U/S=0 son rechazados desde CPL3; #PF P/U/S y W/R se verifican con códigos 5 y 7 y CR2.
- Retorno del manejador al programa original, cambios de contexto reproducibles y pruebas negativas de DPL/RPL.

## Pruebas automáticas reproducibles

`CMakeLists.txt` compila y ejecuta **la prueba fuente original** `pc-android/native/tests/x86_cpl_c05_test.cpp` del checkout Mundo X fijado; sin copiar o simplificar el código x86. Las demás cinco pruebas CTest permanecen:

1. x86 real de prueba;
2. x64 real de prueba;
3. HDD RAW persistente;
4. disco QSSD persistente, compactación de patrones y crash recovery;
5. C04 guest IDT;
6. C05 guest CPL3/TSS.

Workflow C00 Real Guest: verifica SHA de todos los componentes, ejecuta 6 CTest, El Torito sintético y evidencia de CPU. Mundo X tiene el workflow C05 con C04, pruebas x64 y suite nativa.

## Pendientes fundamentales

- **No se ha instalado Windows 10, ni se ha arrancado Windows PE**.
- No es una CPU conforme para Windows: faltan SSE2/x87, privilegiados adicionales, APIC, ACPI, reloj, PCI/IDE/AHCI y drivers; el modo protegido de 16 bits y compatibilidad IA32e requieren trabajo adicional.
- No hay JIT/DBT y no se ha medido la velocidad en Android ARM64 con Windows.
- No se demuestra compatibilidad general con interruptores IST, task switching, sistema de permisos I/O por TSS ni protocolos de anillos CPL1/2.
- El backend QSSD sigue siendo bloque exacto, no hardware cuántico físico.

**Próximo C06:** interfaz I/O y MMIO real de placa, PCI, PIT/PIC/RTC, APIC/ACPI por necesidad de trazas y dispositivos; conservar CPU/QSSD con pins exactos y pruebas de huésped.
