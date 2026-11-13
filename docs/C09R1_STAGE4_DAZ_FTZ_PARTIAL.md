# C09R-1 etapa 4 — Integración provisional DAZ/FTZ, no conformidad SSE2 completa

Se continúa sobre REVERSE Windows PR #18 y Mundo X PR #96. El CPU único y original sigue siendo `cosmic_pc::Cpu` de Mundo X, fijado mediante el SHA exacto `73b18d07b62275b23641f870914ded3662a97acb`. El commit se marca **candidato sin fusionar** hasta que pasen las suites de Mundo X y REVERSE Windows.

Se agregó CTest `reverse_windows_c09r1_daz_ftz_mxcsr` que compila directamente `x86_c09r1_stage4_daz_ftz_test.cpp` del checkout canónico. Los tests existentes C04–C09R-1, QSSD, PCI ATA/IRQ14, GPU Virtual y Ether/XI se mantienen en la misma máquina y no se alteraron sus commits. No hay una CPU secundaria ni un disco paralelo.

## Cambios de CPU

- MXCSR.DAZ (bit6) aceptado en LDMXCSR y FXSAVE.MXCSR_MASK.
- Los operandos FP64 subnormales se convierten en cero con signo cuando DAZ=1, sin marcar DE; DAZ=0 conserva el subnormal y la bandera DE.
- Se conserva el tratamiento previo de FTZ para salidas FP64 subnormales.
- Se prueban rutas de guest x64, valor numérico, estado MXCSR, FXSAVE y bits reservados.

## Limitaciones

Este ciclo **no ha completado** el conjunto de instrucciones SSE2 ni la semántica de NaN, todas las excepciones MXCSR, redondeo x87 PC/RC, stack faults y validación ARM64. Sigue sin demostrarse compatibilidad Windows 10. **CPUID x87/FXSR/SSE2 permanece apagado**. No se ejecutó ni utilizó la ISO del usuario. No se generó APK. La PR permanece en borrador y no se fusionará automáticamente.

Las pruebas de CI deben comprobarse sobre el HEAD exacto antes de declarar la integración aprobada.
