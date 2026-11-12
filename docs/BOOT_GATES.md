# Validaciones del arranque auténtico de Windows 10

## Historial que preservamos

Con la ISO real en el Samsung, el motor ejecutó 31.599 pasos del intérprete, realizó 21 lecturas BIOS y llegó a modo protegido de 16 bits. Se detuvo en 0050:0854 con opcode 0F 00. El grupo 6 está implementado en Mundo X PR70 y validado con una ISO sintética, pero no hay una nueva prueba documentada de la ISO real después de esa corrección.

Los pasos del intérprete no deben describirse automáticamente como instrucciones hardware retiradas: una instrucción REP puede contarse por elemento.

## Puertas

- G00: el ejecutor original PcMachine entra en long64 y ejecuta HLT con invitado de prueba, guardando JSON original de la CPU.
- G01: repetir la ISO real con lectura local, comparar avance semántico con el histórico y registrar nueva CS:IP, opcode, modo y dispositivo pendiente.
- G02: demostrar el tránsito auténtico del cargador al kernel, con registros/memoria y trazas de excepciones, no solo una imagen del logo.
- G03: instalador auténtico, entrada funcional, disco escribible persistente y controladores comprobados.
- G04: kernel e interfaz Windows reales tras reinicio y recuperación reproducible de estado.

## Condiciones de cierre

Un cambio exige pruebas de estados y del huésped auténtico, evidencia de rendimiento pareada si se anuncian ganancias, no reutilizar pantallas falsas y no modificar la ISO original. Nunca subir ISO, claves o datos del teléfono a Actions.

La medición de GitHub Actions en un runner Windows o Ubuntu no acredita el arranque de Windows como sistema invitado.
