# C04 — Excepciones del invitado x86/x64, IDT e IRET reales

Fecha: 2026-11-12.

## Fuente de autoridad e integración

- La CPU canónica es la original `cosmic_pc::Cpu` y `PcMachine` de Mundo X.
- PR CPU: https://github.com/guadalupejuan001-code/mundo-X/pull/86 (borrador, no fusionada).
- SHA de Mundo X fijado en `config/dependencies.lock.json` y workflow: `59fca713b878820a2de934c6da045ae989007143`.
- REVERSE Windows no implementa un segundo intérprete de CPU ni intercepta errores para simular un escritorio.
- La prueba fuente `pc-android/native/tests/x86_idt_c04_test.cpp` se compila directamente desde la dependencia exacta dentro de `CMakeLists.txt`.
- Las dependencias GPU Virtual, QSSD y REVERSE-PC permanecen fijadas en sus SHA previos; el backend QSSD C03 no se reemplaza ni se fusiona.

## Qué hace C04 realmente

1. El intérprete distingue excepciones arquitectónicas (#DE=0, #UD=6, #GP=13, #PF=14) de errores del anfitrión y opcodes sin implementar.
2. Los fallos arquitectónicos de un huésped protegido de 32 bits o largo de 64 bits intentan entregar el vector a una puerta válida de la IDT del huésped; la instrucción fallida conserva su RIP de reinicio.
3. Se validan los límites de IDT, tipo/atributos, presencia, privilegio de puerta para INT software, segmento de código destino y campos de descriptor.
4. El huésped recibe un marco de pila de Ring0 del tamaño adecuado; #PF y #GP llevan código de error. En modo 64 bits se apilan también SS y RSP.
5. IRETD/IRETQ restauran instrucciones, segmentos y flags del mismo nivel Ring0; se preserva la semántica de IF para puerta de interrupción frente a puerta de trampa.
6. Si falla la entrega del primer vector, se intenta #DF. Si también falla se informa triple fault; no hay respuesta BIOS falsa.
7. Rollback conserva RAM/registros/estado ante excepciones parciales. El contador de instrucciones refleja pasos de intérprete, no instrucciones retiradas por CPU física.

## Pruebas y resultados

### Mundo X

- https://github.com/guadalupejuan001-code/mundo-X/actions/runs/43321581317
- `REVERSE_WINDOWS_C04_IDT_PASS checks=40 windows10_booted=false ring3=false tss=false`.
- `PASS x86 long-mode: 45274 assertions`.
- `WIN10_CPU_PROFILE_PASS checks=38`.
- `WIN10_GROUP6_0F00_PASS checks=130`.

La prueba C04 inyecta instrucciones auténticas: fallo de página #PF; ISR invitado edita su PTE en RAM; IRETQ vuelve a la instrucción MOV original y completa la lectura. Verifica CR2, código de error, orden de la pila de 64 bits, RFLAGS, SS/RSP y restauración de IF. Otros huéspedes prueban #GP, #UD, #DE, #DF y retorno en modo protegido32.

### REVERSE Windows

- https://github.com/guadalupejuan001-code/reverse-windows/actions/runs/43321907949
- `5/5 Test Passed` (x86, x64, disco RAW, disco QSSD y prueba C04 IDT).
- Workflow también reproduce ISO sintética El Torito protegida 16 bits y valida evidencia del huésped x64.
- Ni la ISO original ni una instalación de Windows se cargaron a CI.

## Límites y lo que **no** se afirma

- No hay kernel real de Windows 10 arrancado ni Windows Setup instalado.
- Sólo se demuestra **Ring 0 y el mismo CPL**; C05 debe implementar Ring3, DPL/RPL completos, TSS/IST y cambio de pila.
- Faltan excepciones protegidas de 16 bits, compatibilidad IA-32e, IDT cruzando anillos, APIC, IRQ externas, delivery exhaustivo de excepciones Intel y semántica avanzada de errores anidados.
- No se han implementado SSE2, x87, funciones completas ACPI/APIC ni JIT.
- Falta conectar los dispositivos de C06/C07 para que el kernel de Windows use el disco QSSD, porque BIOS INT13 solo cubre prearranque.

## Puerta de salida C04

**PASS específico, condicionado**: las excepciones y retornos del mismo anillo Ring0 funcionan en huéspedes sintéticos reales y el resto de REVERSE Windows mantiene 5/5 CTest. C04 es un incremento verificable y no equivale a conformidad completa del procesador con Windows.

**Próximo C05:** privilegios / Ring3, retorno a Ring0 y pila TSS, con pruebas de #PF de usuario que incluyan bit U/S, y luego integración de un kernel de prueba con cambios de privilegio. No fusionar la PR automáticamente.
