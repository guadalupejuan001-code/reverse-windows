# Prime r110: RAM de estados y reparto de CPU/IRQ integrados

Versión Android `0.3.85`, código `115`, paquete Prime
`com.reversewindows.android.prime.smp.r110`.

## Contenido acumulado

- RAM Éter de estados r109: `063b4546be106adc135f95a2c258fc01a6b4c5dd`.
- Reparto entre vCPU listas y despertares IRQ de #99: `6f22f5c0fc9ae25c7faf93f8d232705483e8637e`.
- Presión, conservación de caché y diagnóstico de #100, ya incluida en r109:
  `82718583fe628dc06355dba5cd0fca3582223128`.

El commit de integración conserva ambas historias. No reemplaza las revisiones
de los componentes de r109: Mundo X sigue fijado en
`13e82d7020a7cb800888996a3b36851868b14950`, con los mismos pins de GPU Virtual,
qSSD y REgraphics que figuran en `config/dependencies.lock.json`.

## Funcionamiento

`EtherStateRam` permanece seleccionado desde la construcción de `MemoryFabric`.
El reparto C06 entrega el presupuesto a las vCPU listas, devuelve el trabajo
no consumido y conserva los despertares pendientes. El ejecutor de la vCPU
destinataria entrega la interrupción. La autoridad vectorial y la regulación
de presión Android permanecen conectadas.

## Comprobaciones exigidas al mismo build

- Ascendencia de los tres SHA de origen, registrada en `config/integration-manifest.json`.
- Comparación de los dos manifiestos empaquetados con sus fuentes.
- Versión, identificador, ABI, firma, SHA del núcleo y hash de la APK.
- CTest actual de RAM de estados, proveedor cuántico, integración x86, presión,
  SMP, placa y BIOS/ISO/QSSD; los targets usan el grafo CMake de producción.
- Los cinco modos CPU/IRQ de `smp_guest_test` en Android x86_64:
  `--test-ready-budget`, `--test-idle-irq-dispatch`, `--test-parked-timer`,
  `--test-parked-timer-boundary` y `--test-parked-timer-transition`.
- El propio ejecutable de reparto exige y registra
  `C06_READY_WAKE_BACKEND ether-state-ram`.
- La sesión de la APK identifica `ether-state-ram` y la autoprueba de RAM
  ejecuta las cuatro instrucciones x86 que leen 41 y escriben 42.

Los resultados se consultan en los workflows del SHA exacto. La matriz nativa
incluye x86_64 y aarch64; la prueba de aplicación usa un emulador Android x86_64.
La APK entregable es ARM64. Esto no acredita una instalación completa de Windows
ni medidas de rendimiento en el teléfono.

## Identidad y datos

El nombre del APK y del artefacto incluye el SHA corto de su código. El archivo
`build.txt` del artefacto registra el SHA completo y el certificado de firma.

El paquete r110 permite una instalación independiente de r108/r109 mientras se
utiliza la firma de depuración de CI. Los discos privados de las aplicaciones
anteriores no se transfieren automáticamente. Conservarlos antes de planificar
una migración; no desinstalar la versión que los contiene para probar r110.
