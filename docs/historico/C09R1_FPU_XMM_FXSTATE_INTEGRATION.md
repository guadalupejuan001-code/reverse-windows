# C09R-1 — Estado XMM/x87/FXSAVE real en la CPU canónica, sin Windows

Fecha: 2026-11-12 (GitHub Actions registra fecha UTC 2026-11-13). Rama encadenada sobre PR #14 C09R. El usuario conserva su ISO de Windows 10 x64 español; aquí no se descarga, se sube ni se ejecuta.

## Propiedad del código

- Mundo X es autoridad exclusiva de x86/x64: PR #92, SHA exacto `7546446a40b318c8b969fc76faef39336a28e1b5` pin en `config/dependencies.lock.json` y GitHub Actions.
- REVERSE Windows NO crea ni intercepta una segunda CPU. Sus 15 pruebas históricas, la placa base PCI/PIC/APIC, BIOS, ATA IRQ14, QSSD persistente, GPU Virtual + StateBus y Ether/XI siguen conectados al **mismo** `cosmic_pc::PcMachine`.
- Las pruebas C09R-1 se compilan directamente desde `deps/mundo-X/pc-android/native/tests/x86_c09r1_extended_state_test.cpp`, no de una copia reducida.

## Implementación comprobada

| Función | Detalle real |
|---|---|
| Registros XMM0–15 | 128 bits exactos por registro, estado inicializado y copiado/restaurado con snapshots de CPU |
| MXCSR | Inicio 0x1F80, máscara 0xFFBF, LDMXCSR/STMXCSR con validación |
| CR4.OSFXSR | Se habilita mediante escritura real `MOV CR4`; se comprueban CR0.EM/TS y el rechazo de instrucciones cuando falta OSFXSR |
| x87 de 80 bits | FNINIT, FLD1, FLDZ, FCHS/FABS, FLD/FXCH, memoria m80 FLD/FSTP, control/status; bytes crudos sin aproximación en double host |
| FXSAVE/FXRSTOR | Zona de 512 bytes alineada, x87, XMM0–7 y XMM8–15 sólo en modo largo, FPU/MXCSR, rechazo de MXCSR corrupto y rollback |
| SSE2 entero parcial | MOVDQA/DQU, MOVAPD/UPD, MOVD/Q, MOVSD/SS como transportes, PXOR/PAND/POR/PANDN, PADD/PSUB enteros, PCMPEQ, PSHUFD, PSLLDQ/PSRLDQ |
| MMU/eficiencia | Prevalidación por página y transferencias exactas en bloques de 8 bytes para FXSAVE/FXRSTOR y memoria XMM. Test FXSAVE comprueba menos de 180 page walks donde la aproximación por byte requería >1000 |

## Pruebas reproducibles

- Mundo X `reverse-windows-c09r1-extstate.yml`: prueba nueva con valores exactos de XMM/80bits, alineación, FXSAVE, FXRSTOR en 64 bits y en modo heredado, MXCSR, rollback, OSFXSR/#NM, CPUID honesto, más suite completa histórica. El test del estado expandido verifica el tamaño MMU, las instrucciones enteras y los bytes guardados.
- REVERSE Windows `CMakeLists.txt`: **16/16 CTest PASS** (15 anteriores y nueva prueba SIMD/FXSTATE). Mantiene pruebas PCI IDE/ATA, QSSD, GPU y preflight de máquina sin ISO.
- GitHub Actions conserva la evidencia de CPU y el informe `ready_for_windows10_install=false`. Una suite exitosa NO constituye instalación de Windows.

## Razones por las que aún no se activa SSE2 y x87 CPUID

Este subciclo implementa un **subconjunto real**, NO la ISA completa. Faltan instrucciones de doble precisión como ADDSD/ADDPD/MULPD, redondeos/excepciones MXCSR, conversiones y más instrucciones SIMD; en x87 faltan aritmética 80 bits, condiciones NaN/overflow, excepciones y estado completo de FIP/FDP/FOP. No se declara FXSAVE/FXRSTOR conforme para todos los estados x87/MMX.

**CPUID SSE2 y x87 permanecen a 0**. Esto es deliberado: anunciarlos habilitaría código del instalador que todavía puede ejecutar instrucciones no disponibles. Por lo tanto la máquina permanece **NO LISTA** para instalar Windows 10.

## Fases pendientes antes de entregar al usuario

1. Completar la ISA SSE2 de flotantes y enteros, flags MXCSR y FPU/x87 80 bits exacto en la misma CPU original; pruebas cruzadas con referencias x86 conocidas.
2. Verificar transiciones contextuales y excepciones SIMD/#NM en kernel sintético, y activar CPUID sólo con cobertura suficiente.
3. Validar la placa base, temporizadores/ACPI y adaptador de vídeo con huéspedes de prueba; terminar aplicación Android ARM64 + REgraphics y control teclado/touch/ISO SAF.
4. Entregar la máquina para que el **usuario** seleccione su ISO de Windows 10 español e instale desde su dispositivo. No usar la ISO en CI ni generar APK inexistente.

Intel SDM: https://cdrdv2-public.intel.com/843827/253665-sdm-vol-1-dec-24.pdf
Intel FXSAVE: https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf