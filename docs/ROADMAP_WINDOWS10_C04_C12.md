# REVERSE Windows — plan maestro C04–C12

Fecha: 2026-11-12. Objetivo: instalar y ejecutar **Windows 10 x64 real** desde ISO local en una PC x86-64 emulada sobre Android ARM64. No sustituir la CPU canónica PcMachine de Mundo X, no montar pantallas falsas, no afirmar virtualización x86 asistida por hardware ARM, no fusionar PR automáticamente.

## Línea base comprobada

- C00 PR #1: `PcMachine` de Mundo X ejecuta huéspedes x86/x64 de prueba.
- C01 PR #2: HDD RAW `.rvdisk`, BIOS INT13, escrituras persistentes y arranque de MBR de prueba.
- C03 PR #6: disco disperso `.rqvd` respaldado por `qssd::StateBus`; transacciones, deduplicación exacta, recuperación de cola incompleta, pruebas x86 de disco y CI aprobadas. Fuente de partida `b3930d4f6370862112c2067a0d428507284c8de7` (rama experimental, NO fusionada).
- Trabajo `.riso` / ISO Lab tiene ramas separadas; no se considera parte del disco de arranque hasta tener lector montado, pruebas y una integración explícita.

**Defectos prioritarios revisados en Mundo X:** `x86_cpu.hpp` registra `Fault` tras una excepción pero no entrega la excepción al manejador invitado en modo protegido/largo; `interrupt()` rechaza IDT protegida; `step()` rechaza ring != 0; `LOCK` no está implementado; traductor actual soporta una parte de IA-32e de 4 niveles (no paginación legacy). Cualquier corrección se hace primero en su repositorio de autoridad, se valida y SOLO después se actualiza el pin SHA en REVERSE Windows.

## Ciclos y puertas de validación

| Ciclo | Responsabilidad / repositorio | Implementación mínima | Prueba de cierre (no basta compilar) |
|---|---|---|---|
| C04 | Excepciones de CPU / Mundo X | Distinguir falla del intérprete vs excepción x86; IDT para modo protegido/largo, puertas, marcos, código de error, `CR2`, `IRET`, tratamiento de fallas dobles; preservar rollback transaccional. | Huésped x86 provoca #PF/#GP/#DE/#UD; entra al manejador INVITADO y reanuda correctamente tras reparar condición; pruebas negativas. |
| C05 | Privilegios y MMU / Mundo X | CPL0/CPL3, DPL/RPL, TSS y cambio de pila, retorno, transiciones kernel/usuario, mecanismos SYSCALL/SYSRET o SYSENTER si las trazas lo exigen; NX, accesos y páginas; instrucciones requeridas demostradas. | Código de prueba ejecuta Ring3 -> excepción/sistema -> Ring0 -> regreso a Ring3; acceso prohibido produce #PF con código correcto; suite existente x86/x64 intacta. |
| C06 | Placa base virtual / REVERSE Windows, adaptadores Mundo X | I/O bus de dispositivos y MMIO real; PCI config 0xCF8/0xCFC, PIC/PIT/RTC y APIC/ACPI conforme a necesidades del invitado; CPUID y temporizadores coherentes; un solo vCPU al principio. | Huésped enumera PCI y ACPI, programa interrupción y temporizador y recibe el vector correcto sin bucles/falsos ACK. |
| C07 | Disco reconocible / REVERSE Windows | Controladora PCI IDE de compatibilidad con PIO inicial (IDENTIFY, READ, WRITE, status/error, IRQ, FLUSH); delegar sectores al MISMO `BlockDisk` QSSD. AHCI únicamente si las pruebas del huésped lo exigen; DMA después de contrato correcto de RAM. | Test de huésped por puertos IDE/PCI identifica geometría, escribe, lee, reinicia y comprueba bytes QSSD; rechazo de comandos inválidos y regresión INT13. |
| C08 | Durabilidad y escala de QSSD / REVERSE Windows + Virtual-qSSD- | Índice de recuperación/compactación sin perder referencias, operaciones largas, crash injection, cachés acotadas, validación y checkpoints coherentes; política de sincronización explícita. | Escribir/reabrir repetidamente, cortar antes/después de commit, validar 100% sectores y conservar datos tras reinicios; benchmark RAW vs QSSD de latencia, RAM, bytes físicos y amplificación. |
| C09 | ISO y Windows PE / REVERSE Windows + Mundo X | Arranque El Torito BIOS de ISO Windows auténtica solo local; instrumentación CS:IP, opcode, CPL, excepción, MMIO, puertos e IRQ; corregir primer fallo comprobado; boot de Windows PE como siguiente hito. | Trazas genuinas superan checkpoint histórico 0050:0854/0F00 y muestran avance verificable del cargador; después kernel WinPE vivo, no imagen simulada. |
| C10 | Instalación/reinicio reales / REVERSE Windows | Windows Setup reconoce HDD, particiona MBR, copia archivos, reinicia desde QSSD y alcanza sesión real; recuperación consistente de máquina CPU+RAM+dispositivos+disco. | Logs y capturas de framebuffer originados por invitado real; reinicio y arranque desde disco conservando archivos, entrada funcional. |
| C11 | Producto Android / REVERSE-PC + REVERSE Windows + GPU-VIRTUAL/REgraphics | NDK + UI Android, acceso local seguro a ISO/disco, entrada teclado/touch, framebuffer invitado genuino y presentación eficiente, controles de RAM, pausa/reanudación; sin escritorio falso. | APK abre e inicia misma máquina en dispositivo real; entrada/salida comprobada; prueba de estabilidad y consumo térmico/memoria. |
| C12 | Optimización medible / Mundo X, GPU-VIRTUAL, QSSD, REgraphics | Perfilado en ARM64; optimizar hot paths CPU y storage, evaluar JIT x86→ARM64 solo tras pruebas equivalentes; cachés, transferencias, gráficos y almacenamiento con evidencia. | Benchmarks pareados del mismo huésped y dispositivo, corrección byte/instrucción preservada; publicar velocidad, memoria, consumo y limitaciones; no afirmar aceleración no medida. |

**Ruta principal:** C04 -> C05 -> C06 -> C07 -> C09 -> C10 -> C11. C08 debe cerrarse antes de escrituras de instalación prolongadas en C10. C12 solo después de obtener línea base. Las dependencias se prueban antes de cambiar orden por conveniencia.

## Decisiones y límites explícitos

1. Priorizar BIOS legado + MBR para el primer objetivo Windows 10 x64; Windows admite este modo. UEFI/GPT quedará como fase posterior. Para una instalación inicial usar una imagen lógica de ~64 GiB, no intentar usar 16 TiB con MBR. El modo UEFI exige particionado GPT; BIOS/MBR y UEFI/GPT no se confunden.
2. Una vCPU x86 es suficiente para validar primero el hardware; workers Ether/XI NO equivalen a SMP x86. Agregar SMP y APIC local por núcleo después de que una sola vCPU funcione.
3. `QSSD` es el backend de bloques y autoridad de revisiones. Un enlace de referencias o una simulación de qubits no demuestra teletransportación ni aceleración cuántica física. Toda lectura invitada debe devolver bytes exactos.
4. La presentación GPU Virtual + REgraphics toma píxeles del framebuffer de la máquina real. Una animación o escritorio dibujado por el host no cuenta como arranque.
5. JIT/dynamic binary translation se investiga por rendimiento; no reemplaza una CPU semánticamente correcta. Debe invalidar bloques automodificables y manejar excepciones precisas.
6. No subir ISO, licencia, imagen de disco, datos privados o checkpoints de teléfono a GitHub. CI usa huéspedes de prueba libres/sintéticos; la ISO real se prueba localmente.
7. Ninguna mejora de rendimiento se anuncia sin baseline RAW e igualdad de salida. Guardar métricas de guest steps, wall time, host RAM, bytes materializados, I/O y errores.
8. Cada ciclo crea rama/PR draft, documentos y pruebas; una PR se encadena sobre la anterior aprobada o SHA conocido, sin merge automático ni cambios silenciosos en módulos. Cada nuevo SHA requerido se fija explícitamente.

## Protocolo repetible por ciclo

**D0 Auditoría:** comprobar heads, dependencias, fallos existentes y presupuesto de recursos.
**D1 Contrato:** escribir qué hará el componente, sus entradas, salidas y errores; criterio observable de PASS.
**D2 Implementación:** cambios mínimos donde vive la autoridad, integrar al mismo `PcMachine`/bus/disco, no por un módulo aislado sin uso.
**D3 Validación:** unitarias + integración + un huésped real de prueba; ejecutar CI; resolver errores y validar regresión.
**D4 Evidencia:** commit SHA, PR, logs, trazas, limitaciones y comparación antes/después.
**D5 Puerta:** PASS sólo con D0-D4; si falla, no avanzar ni vender una capacidad inexistente.

## Primera tarea concreta C04

1. Leer en Mundo X `pc-android/native/x86_cpu.hpp` las rutas `step`, `interrupt`, `page_fault`, `rollback`, `IRET`, carga de IDT/segmentos y propagación de flags; leer `pc_machine.hpp` para los hooks BIOS.
2. Diseñar las excepciones síncronas como eventos entregados al invitado, distintos de host errors y opcodes no soportados. Un fallo de página debe detener la instrucción, conservar CR2/código de error y entrar en IDT. No simular éxito.
3. Construir un huésped mínimo que provoque #PF por ausencia de página, que el manejador corrija la tabla, ejecute IRET y que la instrucción fallida se complete exactamente una vez. Agregar #GP/#DE y marcos x64.
4. Correr los tests originales de Mundo X y las pruebas de REVERSE Windows en un pin actualizado; entregar trazas antes de implementar el siguiente dispositivo.

**No está implementado en este documento:** este PR solamente fija el plan auditable; ningún ciclo C04–C12 se declara concluido.
