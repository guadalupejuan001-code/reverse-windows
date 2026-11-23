# Observación ARM64 prolongada de Prime

La referencia es `433b2d7`. El ensayo usa dos CPU del mismo runner ARM64 Linux, entrada de memoria de 749 MiB con umbral de 128 MiB y un QSSD nuevo. Cada revisión arranca por separado con el medio oficial Windows 10 Enterprise Evaluation 22H2 x64. Se conservan los relojes y las interrupciones del producto.

La meta es alcanzar 11.000 millones de pasos agregados. A partir de 9.500 millones se guardan puntos separados por 50 millones. Se informa también de los pasos de la vCPU de arranque y de cada vCPU secundaria. Los estados, INIT/SIPI, despachos en hilos, espera de bus y tiempos de reloj IRQ permiten comprobar si el ensayo alcanzó una fase comparable a la del Galaxy. El medio del Galaxy tiene un tamaño distinto.

`late-smp-targets.json` permite fijar un candidato publicado. Con `candidate: null` solo se ejecuta la referencia. La rama de CI `ci/prime-r110-arm64-late-smp` ejecuta el mismo flujo para ambos productos cuando se registra un candidato. El informe ofrece porcentajes de cambio cuando coinciden los modos, el progreso de lectura BIOS y la disponibilidad de vCPU observada.

El flujo publica un artefacto provisional al cruzar 10.500 millones y el artefacto final conserva la comparación, el estado al detener la medición y la última pantalla capturada.
