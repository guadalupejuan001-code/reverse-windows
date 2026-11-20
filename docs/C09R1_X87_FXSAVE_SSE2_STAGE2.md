# C09R-1 Etapa 2 — x87, FXSAVE/FXRSTOR y ampliación SSE2 en Mundo X

**Fecha:** 2026-11-12 (GitHub Actions opera en UTC). Esta etapa está encadenada sobre C09R-1 PR #16 de REVERSE Windows y C09R-1 PR #93 de Mundo X. **No se ha utilizado la ISO personal ni arrancado Windows**.

## Fuente canónica y contrato de integración

- El único ejecutor de instrucciones es `cosmic_pc::Cpu` dentro de `PcMachine` original de Mundo X.
- Los cambios en FPU/XMM/MXCSR están en Mundo X. REVERSE Windows **solo fija el SHA exacto y compila sus tests originales**; no hay un segundo motor SSE2, CPU, ni módulo aislado que reemplace el procesador.
- Ether/XI conserva la memoria lógica de la misma máquina; el estado arquitectónico XMM/FPU permanece en el `Cpu::Snapshot` por corrección y se revierte ante fallos, no se cambia por memoria GPU.
- GPU-VIRTUAL sigue seleccionando diferencias exactas del VGA real; qSSD GraphicsWorkMemory registra la revisión del mismo plan. QSSD .rqvd y BIOS/PCI IDE/IRQ14 siguen compartiendo un único `BlockDisk`.

## Funciones implementadas y probadas en huéspedes x86 propios

1. **Estado:** ocho registros x87 físicos de 80 bits, tags físicos, TOP/status/control y FIP/FDP; 16 XMM de 128 bits, MXCSR con máscara de bits reservados. Reset y rollback transaccional incluyen todos estos campos. No se afirma aún soporte MMX ni todos los modos FPU.
2. **x87 parcial:** FNINIT/FNCLEX/FNOP, FLD1/FLDZ, FLD ST(i), FXCH ST(i), FSTP ST(i), FLD/FSTP m32/m64, FADD ST0,ST(i), FLDCW, FNSTCW, FNSTSW y FWAIT con limitaciones explícitas. Las operaciones de redondeo/exception que no se pueden reproducir con precisión suficiente quedan como `UnsupportedState`.
3. **FXSAVE/FXRSTOR:** área de 512 bytes alineada; estado x87, control/status, FTW abreviado físico, datos ST0..7 **lógicos**, XMM0..7 (XMM0..15 en 64 bits), MXCSR y máscara. Los últimos 48 bytes de software (464–511) permanecen intactos. FXSAVE64 y FXRSTOR64 admiten punteros FIP/FDP de 64 bits.
4. **Excepciones:** se rechazan fuentes o destinos inválidos, alineación incorrecta, MXCSR reservado y CR0.EM/TS; se probó entrega #NM vector7 a una IDT x64 con CLTS e IRETQ que retoma exactamente la instrucción FXSAVE. El journal cancela escrituras parciales y restaura el estado.
5. **SSE2 de enteros parcialmente implementado:** MOVDQA, MOVDQU, MOVAPD/UPD, MOVD/MOVQ, PXOR/PAND/PANDN/POR, PADD/PSUB B/W/D/Q, PCMPEQ/PCMPGT B/W/D, PUNPCK bajo/alto, PSLL/PSRL W/D/Q, PSLLDQ/PSRLDQ, PSHUFD, PMOVMSKB y LDMXCSR/STMXCSR.
6. **Oracle x86 independiente:** en un runner x86 real se ejecutó `fninit; fld1; fldz; fxsave` y se comparó byte a byte la cabecera FPU y ST0/ST1 del emulador. Esta comparación detectó y corrigió el orden **lógico** de los valores FXSAVE frente al orden **físico** de las etiquetas.
7. **Contexto:** prueba independiente de dos contextos FPU/XMM distintos, A y B, que preservan etiquetas, MXCSR y registros al guardarlos y restaurarlos. No representa todavía scheduling de dos procesos reales Windows.

## Evidencia y regresiones

- Mundo X: `x86_c09r1_fxstate_test.cpp` tiene **196 comprobaciones**, incluyendo hardware oracle, restauración de contextos y #NM/CLTS/IRETQ.
- Mundo X: `x86_c09r1_sse2_packed_test.cpp` tiene **166 comprobaciones** de operaciones SIMD, comparación, shifts y rollback de memoria.
- `x86_c09r1_xmm_subset_test.cpp` de la etapa anterior conserva sus 56 comprobaciones.
- REVERSE Windows ejecuta los tres tests compilados **desde el SHA fijado de Mundo X**, junto a C00–C09R, ATA/IRQ14, QSSD y GPU-VIRTUAL: **18/18 CTest aprobadas** en el commit de integración.
- La suite original de Mundo X y sus demostraciones también se ejecutan mediante `.github/workflows/reverse-windows-c09r1-fxstate.yml`. No se sube Windows.
- Comparación con oráculo de hardware se limita a la semántica evaluada (cabecera y ST0/ST1), no prueba todas las instrucciones FPU/x87.

## Límites que impiden anunciar SSE2 y x87 en CPUID

- La implementación x87 aún depende de `long double` del anfitrión para algunas operaciones; faltan redondeos y excepciones exactos de precisión extendida, stack faults con estado completo, FNSTENV/FSAVE/FRSTOR, varias conversiones y opcodes.
- SSE2 aún carece de múltiples operaciones escalares y de punto flotante de doble precisión, conversión y excepciones en MXCSR. La presencia de XMM y de operaciones SIMD de enteros no equivale a compatibilidad SSE2 global.
- Faltan pruebas de cambio de contexto completo entre tareas reales de usuario y kernel, más semántica de estado/exception para MMX/FPU y CR0/CR4.
- **Por eso los bits CPUID x87, FXSR, SSE y SSE2 permanecen desactivados**, y la máquina se sigue etiquetando `ready_for_windows10_install=false`.
- No hay prueba de Windows 10 español, de su ISO, de BasicDisplay/WDDM ni benchmark de ejecución ARM64. Tampoco hay APK de Windows creada en esta etapa.

**No fusionar PR automáticamente**. Esta etapa mejora la CPU, pero todavía no cierra C09R-1. El siguiente subciclo debe completar SSE2 escalar/doble y x87, resolver las excepciones restantes, y solo después habilitar las capacidades anunciadas.
