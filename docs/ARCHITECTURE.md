# REVERSE Windows — arquitectura de ejecución actual

Fecha de auditoría: 2026-11-12. Estado: C00 en construcción; Windows 10 no ha completado el arranque.

## Regla de ingeniería

No se importa una APK anterior, un escritorio ficticio de Windows ni un segundo intérprete x86. No se sustituyen los mecanismos x86 que exige el invitado por una API cuántica sin equivalencia probada.

REVERSE Windows es un emulador de PC x86/x64. Android continúa como anfitrión. REVERSE-PC sirve como escritorio central; Mundo X contiene el intérprete y el runtime de cómputo; GPU Virtual, QSSD y REgraphics son componentes que se integran a través de datos y revisiones comprobadas.

## Autoridades

1. Invitado PC: una sola autoridad de registros x86, RAM, MMU, BIOS/firmware, interrupciones, reloj y controladores. El ejecutor es PcMachine de Mundo X C03 fijado por SHA, no el viejo APK Win10 Lab.
2. Runtime Ether/XI: autoridad original de sus operaciones programables, consultada mediante puertos explícitos desde x86. No se le atribuye compatibilidad automática con opcodes x86.
3. QSSD: autoridad de versiones y recuperación para snapshots completos y consistentes. El adaptador que incluya CPU/RAM/disco/dispositivos de Windows aún debe implementarse.
4. GPU Virtual: planea trabajo gráfico a partir de bytes de video reales del invitado, conservando identidad y revisiones.
5. REgraphics y la GPU física: presentan el resultado sin inventar píxeles ni declarar aceleración que no se haya medido.

El port de VGA texto en REVERSE-PC PR239 no demuestra que la presentación VBE32, el escritorio Windows o el rendimiento del hardware físico funcionen.

## Dependencias

config/dependencies.lock.json fija los SHA exactos de:
- Mundo X C03 PR85, motor x86/x64, InfiniteCore y workers.
- GPU Virtual C12 con el candidato PR8 para planificación física.
- QSSD C06 y candidato PR8 para revisiones gráficas sobre StateBus.
- REVERSE-PC C03 PR239 para integración y presentación.
- reverseAndroid no se enlaza; se consulta solo como precedente de recuperación autenticada ARM64.

Candidatos en PR no fusionadas son experimentales, no una release aprobada. La validación prohíbe cambiar silenciosamente a main o a código más antiguo.

## Ciclos

C00: fijar SHA, verificar los cuatro repositorios y compilar una prueba de invitado x86/x64 real.
C01: probar la ISO privada del usuario localmente, continuar desde el primer fallo auténtico.
C02: integrar snapshots QSSD de CPU, RAM, MMU, dispositivos, temporizador y disco, restaurando el mismo siguiente paso.
C03: cumplir las necesidades de CPU/chipset por trazas: SSE2/x87, CPL3, IDT, interrupciones, APIC, ACPI y temporizadores.
C04: añadir un disco virtual de escritura separado de la ISO; completar BIOS y UEFI donde lo requiera Windows.
C05: integrar framebuffer real, GPU Virtual y REgraphics, comprobar equivalencia de píxeles y tiempos.
C06: verificar inicio de kernel, instalador, reinicios y escritorio real con entradas del invitado.

El arranque Windows 10 no se declara por ejecutar una demo x64, compilar en Windows ni mostrar un logo.
