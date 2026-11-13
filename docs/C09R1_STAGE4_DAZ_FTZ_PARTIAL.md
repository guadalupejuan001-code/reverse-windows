# C09R-1 etapa 4 — Integración SSE2, MXCSR y x87

REVERSE Windows PR #19 continúa la PR #18 e integra la CPU canónica de Mundo X PR #96 en el commit `fd20afd2aca2cec5c40b63a14ae0e7842b68df9a`. El workflow comprueba el SHA y los archivos requeridos antes de compilar.

## Pruebas integradas

| Área | Cobertura |
| --- | --- |
| DAZ/FTZ | Cero con signo, máscaras de `LDMXCSR` y `FXSAVE`, resultados subnormales y #XM por underflow desenmascarado |
| SSE2 | NaN, comparaciones, movimientos, operaciones lógicas, conversiones empaquetadas y prioridad de excepciones |
| x87 | Seis casos de precisión y redondeo PC/RC y errores en pila llena o vacía |
| Contexto y hardware | Ring 3 → Ring 0 → Ring 3, `FXSAVE/FXRSTOR`, QSSD, PCI ATA/IRQ14, GPU Virtual y Ether/XI |

CTest compila las cinco pruebas de etapa 4 desde el checkout fijado de Mundo X. El workflow las ejecuta además para archivar sus resultados y conserva las regresiones previas. Mundo X ejecuta las pruebas contra hardware x86 y en un runner ARM64 con resultados de referencia.

## Límites

CPUID SSE2/x87/FXSR continúa desactivado. Faltan instrucciones SSE2, combinaciones de excepciones y escenarios de contexto. Parte de la aritmética extendida x87 depende de `long double` y la conversión `FSTP` a memoria tiene cobertura limitada. Se usaron huéspedes sintéticos. El arranque de Windows y la ISO del usuario quedan fuera de esta campaña.
