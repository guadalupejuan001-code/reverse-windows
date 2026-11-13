# REVERSE Windows C09R-1 etapa 4 — fidelidad SSE2, MXCSR DAZ/FTZ y x87

**Fecha:** 12–13 noviembre 2026. Rama derivada de REVERSE Windows PR #18; integra los cambios de Mundo X PR de etapa 4. **No se ha ejecutado Windows ni la ISO en español del usuario.**

## Integración correcta de tu tecnología

```text
REVERSE Windows (UN solo PcMachine de Mundo X sobre anfitrión ARM64)
  ├─ Cpu x86/x64 canónica — XMM, MXCSR, x87, FXSAVE/FXRSTOR, Ring3/0, IDT/MMU
  │    ├─ C09R1-4: DAZ/FTZ, SSE2 compare/packed convert, x87 PC/RC y stack
  │    └─ Ether/XI: la misma InfiniteSession + MemoryFabric; sin duplicar RAM
  ├─ placa PCI IDE / IRQ14 / BIOS INT13 — una sola BlockDisk
  │    └─ QSSD (backend .rqvd, StateBus, bloques exactos, commit durable)
  └─ PcVideo framebuffer REAL producido por instrucciones del huésped
       └─ GPU Virtual selecciona cambios; qSSD GraphicsWorkMemory registra
          revisión; REgraphics/Android salida final PENDIENTE
```

No se introduce un núcleo SSE alternativo en REVERSE Windows ni se sustituye Mundo X por un modelo externo. Este repositorio únicamente fija el commit original y ejecuta los **cuatro tests desde el checkout exacto de Mundo X**, junto con toda la suite previa.

## CPU: estado realmente implementado

- **SSE2/MXCSR:** MXCSR admite DAZ (bit6) y FTZ (bit15), máscaras y flags; test SSE2 contra hardware x86 compara resultados/status de denormales, FTZ enmascarado, unión de IE/ZE de dos lanes y NaN señalizante.
- **SSE2 adicional:** CMPPD/CMPSD predicados, SHUFPD, MOVMSKPD, ANDPD/ANDNPD/ORPD/XORPD, MOVLPD/MOVHPD y conversiones CVTDQ2PD/CVTPD2DQ/CVTTPD2DQ. Pruebas de NaN, signed32, redondeo, DAZ, entero indefinido, IE/PE y preservación de registros.
- **x87 PC/RC:** FCW permite aritmética básica con precisión de 24, 53 o 64 bits y RC de nearest, down, up y zero, cuantizando el resultado a la mantisa elegida. Seis oráculos físicos x86 aportan los diez bytes de ST0 y el FSW esperado; esos goldens se comparan también en ARM64.
- **x87 stack:** overflow de FLD1 en pila llena y underflow de FSTP ST0 vacía, con máscara/unmask, IE/SF/C1/TOP/ES/B y el indefinido80 cuando procede. Se conserva la ruta diferida FWAIT→#MF, FXSTATE, #NM y contexto Ring3↔Ring0 de fases previas.
- CPUID x87/FXSR/SSE2 sigue **false**: faltan más opcodes, excepciones y conformidad en distintos modos; esta entrega no autoriza instalar Windows 10.

## Evidencia

Pruebas originales en Mundo X:
- `x86_c09r1_stage4_mxcsr_test.cpp`: DAZ/FTZ oráculos hardware, status y packed flag union.
- `x86_c09r1_stage4_x87_test.cpp`: PC/RC seis casos, stack overflow/underflow mask/unmask, goldens FXSAVE.
- `x86_c09r1_stage4_sse2_misc_test.cpp`: compare/shuffle/bitwise/mov SSE2.
- `x86_c09r1_stage4_packed_cvt_test.cpp`: conversiones empaquetadas y excepciones DAZ/IE/PE, oráculo SSE2.

Los cuatro tests se compilan desde la dependencia Mundo X **commit `242f8369ebed754926e43bd6a07b2a452645de39`**, fijada en `config/dependencies.lock.json` y GitHub Actions. Dependencias GPU Virtual/QSSD/REVERSE-PC preservadas sin cambios.

- Native **ARM64**: GitHub Actions `ubuntu-24.04-arm` validó 4 tests C09R1 etapa4 y los tests de FPU/SSE2 de etapas anteriores. `uname -m=aarch64`; los casos PC/RC/DAZ/FTZ usan goldens capturados de hardware x86 real, no ejecuta x86 de forma nativa. https://github.com/guadalupejuan001-code/mundo-X/actions/runs/43368598615
- Native **x86**: GitHub Actions stage4 ejecuta los oráculos directamente con SSE2 y FXSAVE y luego la suite nativa completa de Mundo X: https://github.com/guadalupejuan001-code/mundo-X/actions/runs/43368598649
- REVERSE Windows CTest: **26 pruebas** previstas: las 22 anteriores más cuatro C09R1 etapa 4. Las nuevas pruebas están ligadas directamente al mismo `PcMachine`; QSSD, PCI ATA, GPU Virtual/Ether permanecen en regresión. Los logs específicos se adjuntan como artefactos.

La cantidad exacta de comprobaciones depende del anfitrión: en ARM64 se ejecutan los tests del emulador y los goldens; en x86 además se comparan instrucciones del propio procesador. No se confunden esos resultados con pruebas de Microsoft Windows.

## Límites críticos (no cerrados)

1. Todavía faltan muchas instrucciones SSE2/SSE, MMX, operaciones de redondeo, FTZ/DAZ, NaN y exceptions combinadas en todos los formatos. El hecho de superar los oráculos listados **no** constituye conformidad SSE2 global.
2. x87 sigue dependiendo parcialmente de `long double` del anfitrión para aritmética; el PC/RC de las pruebas es verificable, pero faltan conversiones y excepciones de pila para más instrucciones, transcendentes, PC y FSTP de todos los modos, legacy FSAVE/FRSTOR.
3. Cambios de contexto Ring3/Ring0 probados solo en casos específicos; faltan todos los escenarios MMU, TSS/IST y privilegio que puede usar el kernel de Windows.
4. REgraphics/Android visualización final, firmware ACPI/APIC/temporizadores y empaquetado APK están pendientes de pruebas integradas, sin afirmar CPU Windows x64 lista.
5. No se ha probado el Samsung Android físicamente; un runner ARM64 de GitHub no sustituye a un benchmark en el dispositivo.
6. No se creó ni ejecutó una ISO Windows, ni se utilizó la ISO del usuario; Windows 10 x64 en español sigue siendo el objetivo de instalación **solo del usuario**.

**Decisión de cierre:** C09R-1 etapa 4 mejora fidelidad con evidencia de CPU x86/ARM64, pero **NO habilita CPUID x87/SSE2/FXSR** ni cierra los requisitos de una máquina instalable. PR en borrador, sin merge automático.
