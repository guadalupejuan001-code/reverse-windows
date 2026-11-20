# REVERSE Windows — C09R-1 etapa 3: SSE2 binario64, MXCSR y x87 exacto

Fecha: 12–13 de noviembre de 2026. Rama encadenada sobre REVERSE Windows PR #17, sin fusionar automáticamente.

## Meta y límites

Dejar la **única CPU canónica de Mundo X** preparada por etapas para que el usuario pueda instalar **su propia ISO de Windows 10 x64 en español**, exclusivamente desde Android cuando el hardware sea apto.

**No se ejecutó Windows ni la ISO personal.** En este ciclo solo se ejecutaron instrucciones x86/x64 escritas para pruebas y las regresiones sintéticas anteriores. No existe todavía evidencia de instalación de Windows 10, drivers de Windows ni pantalla de escritorio.

## Integración sin duplicados

```text
REVERSE Windows (único PcMachine de Mundo X)
  ├── CPU original: XMM/MXCSR + x87 + FXSAVE/FXRSTOR + SSE2 parcial C09R-1
  ├── MemoryFabric RAM dispersa + Ether/XI estados y ventanas originales
  ├── PCI/ATA PIO + IRQ14 + BIOS INT13 ---- único BlockDisk ---- QSSD .rqvd StateBus
  └── PcVideo framebuffer real -- GPU-VIRTUAL cálculo de cambios exactos
                                -- qSSD GraphicsWorkMemory versión del plan
                                -- REgraphics/Android salida final PENDIENTE
```

La dependencia Mundo X está fijada por SHA exacto en `config/dependencies.lock.json` y en GitHub Actions. Los SHA de GPU Virtual, QSSD StateBus y REVERSE-PC no se modifican. La CPU no se reemplaza por una implementación SIMD alternativa de REVERSE Windows. Los tests nuevos se compilan directamente desde los archivos de Mundo X.

## Funciones realmente probadas

### SSE2 y MXCSR

- Aritmética FP64 escalar: `ADDSD/SUBSD/MULSD/DIVSD/SQRTSD/MINSD/MAXSD`, `MOVSD` con conservación de bits superiores; aritmética empaquetada PD de dos elementos.
- Conversión signo entero32/64 y double: `CVTSI2SD`, `CVTSD2SI`, `CVTTSD2SI`. Casos nearest-even, redondeo hacia arriba/abajo y truncado; infinito produce entero indefinido e IE.
- `COMISD/UCOMISD`: resultados de EFLAGS CF/ZF/PF, desactivación OF/SF/AF, tratamiento probado de QNaN.
- Estado y excepciones MXCSR: indicadores sticky IE/DE/ZE/OE/UE/PE, máscaras y CR4.OSXMMEXCPT. Desenmascarar división por cero y activar OSXMMEXCPT lleva a **#XM vector 19**; desactivarlo lleva a **#UD vector 6**, conservando el bit ZE incluso si se revierte la instrucción. Manejadores de huésped e IRETQ probados, sin entregar un kernel ficticio.
- `HostRound` toma el entorno flotante del anfitrión solo durante una operación y lo restaura. Esto **no demuestra conformidad de todas las condiciones IEEE en Android ARM64**; hace falta una matriz de resultados con oráculos y casos excepcionales adicionales.

### x87 extendido y contexto

- `FLD m32real/m64real` construye los 80 bits exactos a partir de la codificación IEEE32/IEEE64, incluidos subnormales y ceros con signo, sin pasar primero por `long double`.
- x87 añade FADD/FMUL/FSUB/FDIV ST0,STi para el modo de precisión extendida y redondeo nearest soportados; las precisiones/modos que todavía no son correctos se rechazan.
- División por cero con excepción enmascarada actualiza FSW.ZE y produce resultado +inf; desenmascarada deja pendiente ES/B/ZE sin modificar ST0. `FWAIT` genera **#MF vector 16** con CR0.NE=1; manejador de código x86 ejecuta FNCLEX + IRETQ y FWAIT finaliza.
- Se conserva FXSAVE/FXRSTOR de 512 bytes, XMM0–15, MXCSR, ST0..7 en orden lógico y las etiquetas físicas, dos contextos A/B y #NM/CLTS/IRETQ de etapa 2.

### Ring3 / Ring0 real

Un programa x86-64 de Ring 3 ejecuta ADDSD, INT80, recibe una conmutación de pila mediante TSS.RSP0 y el manejador del kernel ejecuta FXSAVE64, PXOR y FNINIT, FXRSTOR64 e IRETQ. El programa de Ring 3 continúa con MULSD y sale al kernel mediante INT81/HLT. Los registros XMM, x87 y MXCSR conservan el estado comprobado; el ensayo **no equivale a una sesión Windows ni a un planificador completo**.

## Evidencias de CI

| Prueba fuente original de Mundo X | Resultado |
|---|---|
| `x86_c09r1_stage3_fp_test.cpp` | **54 comprobaciones**; FP64, conversiones, MXCSR, #XM/#UD, FXSAVE |
| `x86_c09r1_stage3_ring3_test.cpp` | **13 comprobaciones**; Ring3→Ring0→FXSAVE→Ring3 |
| `x86_c09r1_stage3_x87_precision_test.cpp` | **231 comprobaciones**; 20 valores IEEE32/64 comparados con `flds/fldl/fxsave` del hardware x86, incluidos subnormales |
| `x86_c09r1_stage3_x87_mf_test.cpp` | **12 comprobaciones**; x87 ZE enmascarado y #MF/FNCLEX/IRETQ |
| Etapa 2 anterior | 196 FPU/FXSAVE + 166 SSE2 packed + 56 XMM |
| REVERSE Windows integrado | **22/22 CTest PASS**: incluye C04–C09R-1, QSSD, PCI ATA, IRQ14 y GPU Virtual |

El workflow de REVERSE Windows archiva por separado los logs de los cuatro ensayos nuevos, además de las evidencias de la máquina y sus componentes.

**Verificación independiente:** las pruebas usan instrucciones reales de una CPU x86 del runner para contrastar algunos resultados numéricos y el formato extendido de ST0. No dependen de Windows, Qiskit ni modelos de IA externos.

## Por qué CPUID sigue sin declarar SSE2/x87 completos

- Todavía faltan instrucciones SSE2 y SSE/MMX, más formatos y conversiones vectoriales, control completo de denormales/FTZ/DAZ, precisión y excepciones de NaN de todos los opcodes.
- El soporte x87 no incluye todas las operaciones de transcedentales, redondeos/casos FPU, FSAVE/FRSTOR legacy, pila con errores completos y todos los flags. Algunas operaciones aritméticas usan `long double` del anfitrión; no existe todavía prueba de exactitud para ARM64.
- Los cambios de contexto de prueba cubren un camino real; no equivalen a cientos de combinaciones del kernel Windows ni al soporte completo de TSS/IST/ACPI y FPU.
- **CPU CPUID: SSE2=false; x87=false; FXSR=false.** No se encienden indicadores porque un subconjunto funcione.
- Faltan las fases de MMU/firmware/REgraphics/Android y compilación APK con selector SAF local. Windows 10 español lo instalará únicamente el usuario cuando la máquina sea lista.

## Siguiente puerta técnica

C09R-1 etapa 4 debe cerrar los opcodes de SSE2 y x87 faltantes, comprobar NaN, denormales, distintos modos MXCSR y x87 PC/RC, completar excepciones de precisión con oráculos independientes y pruebas ARM64; después decidir qué CPUID puede anunciarse. No fusionar automáticamente ni declarar Windows 10 listo.
