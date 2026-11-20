# C06: reparto entre vCPU listas y despertar por interrupción

Base: **Prime r108**, commit `ced230890b5016ac8fb9c70a12fd000d0a91e185`.

**Alcance de esta nota:** validación local en x86-64, incluido el caso en que
una vCPU ejecuta `HLT` dentro de un despacho. Los resultados de ARM64 y
Android se registran por separado en los checks del SHA correspondiente al PR.

## Problemas reproducidos

El [planificador C06](../src/c06_motherboard.hpp) incluía al procesador de
arranque (BSP) en el reparto aunque estuviera dormido. El caso especial que
devolvía su turno solo cubría una secundaria activa. Con dos secundarias y un
presupuesto de 96 pasos, cada una ejecutaba 32 y los otros 32 quedaban sin usar.
Las nuevas pruebas reprodujeron el fallo en serie y con trabajadores.

Además, una secundaria dormida con interrupciones habilitadas reducía el
presupuesto de todo el despacho a 32 pasos por vCPU lista. Dos vCPU que
completaban 32.768 pasos necesitaban 512 despachos, frente a 4 con las
interrupciones enmascaradas en la dormida, aunque no llegara ninguna IRQ.

## Corrección del reparto y del sondeo

En modo de reloj activo, la lista de trabajo contiene las vCPU listas para
ejecutar. Un BSP dormido devuelve su parte a todas las listas. El presupuesto
se reparte por identificador de vCPU, de modo que la primera entrada de la
lista puede ser una secundaria.

Los trabajadores conservan turnos de hasta 4.096 pasos por vCPU. Cuando hay
un procesador dormido que puede aceptar una IRQ, comprueban sus eventos bajo
el bloqueo del bus en subturnos de 32 pasos. Así, sondear un temporizador no
exige terminar y volver a sincronizar todos los trabajadores después de cada
tanda. Los bloques amplios que omitirían ese sondeo ceden a la ruta acotada.
Esta cadencia se expresa en trabajo del invitado; no establece un plazo
universal en milisegundos para Android ni una entrega en 32 pasos globales:
otros trabajadores pueden tener un tramo ya iniciado fuera del bus.

El sondeo registra el evento y marca un despertar pendiente. La entrada al
manejador corresponde al ejecutor de la vCPU destinataria. La marca persiste
entre llamadas a `run()` y tiene prioridad antes de otro turno largo. Si el
despertar corta un despacho, se descuenta el trabajo realmente retirado,
conservando el presupuesto restante para atenderlo. Un nuevo `HLT` dentro del
despacho incorpora inmediatamente su vCPU al sondeo y devuelve la cuota que
no ejecutó. El tamaño de los siguientes tramos se adapta bajo el mismo bus.

El modo de firmware conserva su avance por pasos del BSP y su política de
sondeo. Los cambios de reparto se apoyan en el reloj activo para atender los
dispositivos mientras el BSP duerme.

## Qué significan los 4,5 THz

Hay tres cantidades distintas en el código:

- **Objetivo de ejecución:** `AndroidBootSession::step_engine_window()` solicita
  `duración_ns × 4500` pasos, limita el trabajo por un plazo real del anfitrión
  y registra por separado los pasos que la placa efectivamente retiró.
- **TSC y relojes del modelo:** el perfil `Experimental4_5THz` configura el TSC
  a 4.500.000.000.000 Hz y mantiene la relación de firmware de 20. El reloj
  activo acumula tiempo dentro de `run()`; excluye los intervalos entre llamadas.
- **Rendimiento medido:** requiere contar trabajo completado y tiempo real.
  Reducir 512 despachos a 4 no demuestra ejecutar 4,5 billones de instrucciones
  por segundo ni mide por sí mismo la mejora en el teléfono.

Referencias: [sesión Android](../src/android_boot_session.hpp),
[frecuencia de componentes](../src/prime_pci_clock.h) y
[pruebas SMP y TSC](../tests/smp_guest_test.cpp).

## Evidencia local disponible

Las dos suites existentes pasaron primero sobre la base r108. Las regresiones
nuevas se compilaron también contra esa fuente y reprodujeron los fallos antes
de probar la corrección. La ejecución final local con el cambio registró:

| Caso | Resultado observado en x86-64 |
| --- | --- |
| BSP dormido, una secundaria, presupuesto 96 | 96 pasos retirados, en serie y con trabajadores. |
| BSP dormido, dos secundarias, presupuesto 96 | 48 + 48, en serie y con trabajadores. |
| Dos activas y una dormida, 32.768 pasos, sin IRQ recibidas | 4 despachos tanto con IF enmascarado como habilitado en la dormida. |
| Temporizador de una vCPU dormida durante trabajo ajeno | BSP y secundaria 2 despiertan; una IRQ y un EOI, en serie y con trabajadores. |
| Despertar pendiente entre llamadas | La siguiente llamada con presupuesto 1 retira una instrucción de la vCPU despertada. |
| vCPU que ejecuta HLT dentro del despacho | El temporizador posterior despierta al BSP o a la secundaria 2; una IRQ y un EOI, en serie y con trabajadores. |
| Suite SMP completa | `SMP_PASS`, incluidos relojes, interrupciones y RAM compartida. |
| Suite motherboard completa | `REVERSE_WINDOWS_C06_MOTHERBOARD_PASS checks=593`. |

Los registros locales están en `build-c06/smp.log` y
`build-c06/motherboard.log`. Los manejadores de las pruebas de temporizador
comprueban una cota de 128 incrementos adicionales de los bucles de trabajo
entre vencimiento y observación. Las pruebas locales no ejercitan el backend
nativo ARM64. La latencia, el consumo y el rendimiento en el teléfono siguen
pendientes de medición.

## Reproducción

Desde la raíz, utiliza el grafo CMake de la versión integrada. Así las pruebas
seleccionan `EtherStateRam` y enlazan la misma autoridad cuántica que Android:

```sh
python3 scripts/verify_deps.py --checkout
cmake -S . -B build-c06 -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-c06 --target reverse_windows_smp_guest_test \
  reverse_windows_c06_motherboard_guest_test --parallel 2

build-c06/reverse_windows_smp_guest_test --test-ready-budget
build-c06/reverse_windows_smp_guest_test --test-idle-irq-dispatch
build-c06/reverse_windows_smp_guest_test --test-parked-timer
build-c06/reverse_windows_smp_guest_test --test-parked-timer-boundary
build-c06/reverse_windows_smp_guest_test --test-parked-timer-transition

ctest --test-dir build-c06 --output-on-failure \
  -R '^(reverse_windows_smp_guest|reverse_windows_c06_real_motherboard)$'
```

En r110, la prueba de reparto exige que el mismo ejecutable identifique
`C06_READY_WAKE_BACKEND ether-state-ram`. El workflow Android también ejecuta
los cinco modos anteriores en el emulador y conserva sus registros.
`verify_deps.py` obtiene las revisiones exactas de `config/dependencies.lock.json`.

La validación nativa ARM64 corresponde al job `native-real-guest` de
[c00-real-guest.yml](../.github/workflows/c00-real-guest.yml), con runner
`ubuntu-24.04-arm`. La compilación y comprobación Android corresponden a
[android-boot.yml](../.github/workflows/android-boot.yml). Se deben revisar
los resultados del SHA del PR antes de dar por terminada la validación.
