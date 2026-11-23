# REVERSE Windows

Emulador de PC x86/x64 para arrancar Windows 10 como sistema invitado real.

**Versión de esta rama: Prime r110 · 0.3.86 (116).** Reúne la RAM Éter de estados de r109, la gestión de presión de RAM y la corrección de reparto de CPU y despertares IRQ de la PR #99. Incluye las correcciones de mantenimiento de REP MOVS y compactación de RAM. El arranque completo de Windows 10 todavía necesita una prueba con una ISO real. El proyecto no incluye esa imagen.

Reglas del proyecto:

- Sin interfaces que simulen un escritorio Windows o produzcan imágenes de arranque falsas.
- Sin copiar APK, versiones o implementaciones antiguas de laboratorios anteriores.
- El núcleo de CPU debe ejecutar instrucciones x86/x64 auténticas, con errores explícitos para operaciones no soportadas.
- Dependencias actuales fijadas por SHA y verificadas en CI; los módulos experimentales permanecen identificados como tales.
- Cada avance se demuestra con pruebas del huésped real y registros reproducibles; compilar en un host Windows no equivale a arrancar Windows como invitado.
- Nunca publicar ni distribuir imágenes ISO privadas o con derechos de terceros. Las ISO del usuario permanecen en su dispositivo.

Los cambios de arquitectura y el primer ejecutor de pruebas se desarrollan en ramas y PR independientes antes de su integración a `main`.

## Integración acumulativa r110

La APK incorpora ambas líneas de trabajo en un único commit de integración.
`assets/integration-manifest.json` registra las revisiones de origen y
`assets/dependencies.lock.json` fija los cuatro componentes compilados.
La CI exige que esas revisiones sean antecesoras del build, comprueba los
manifiestos dentro de la APK y ejecuta las regresiones CPU/IRQ sobre
`EtherStateRam` también en Android. [Contenido y comprobaciones](docs/prime-r110-integrated-ram-cpu-irq.md).

## Tarjeta de vídeo Prime

Prime r105 utiliza el estado compartido de Éter para seleccionar geometría y reutiliza los dibujos idénticos antes de llegar a GLES. La auditoría describe la ruta de vídeo de arranque, el controlador Windows, las capacidades actuales y las mediciones de imágenes y transferencias. [Auditoría y resultados de la GPU](docs/prime-r105-gpu.md)

En **Prime r106**, los procesadores secundarios agrupan búsquedas, desplazamientos, puestas a cero y multiplicaciones hasta el siguiente evento que pueda afectar al invitado. El panel de Android muestra los pasos por segundo en tiempo real y durante la ejecución del motor. En **Equipo → Comprobar equipo virtual** se puede verificar un par de Bell en una máquina temporal.

El dispositivo PCI acepta circuitos Clifford de 1 a 64 puertas sobre el estado cuántico compartido por CPU y VRAM. El invitado puede enviarlos mediante `RWGPU_QUANTUM_BLOCK` o mediante DMA con `RWGPU_DMA_QUANTUM_BLOCK`. Los controles y destinos se expresan como posiciones de bits. Un registro inválido rechaza el circuito completo. [Formato de los registros](src/c08_gpu_protocol.h) · [Protocolo DMA](src/c08_gpu_dma.h)

En **Prime r108**, las vCPU ARM64 ejecutan en paralelo bucles x86 que leen o escriben palabras de RAM en direcciones estables. Cada trabajador captura sus entradas, libera el bus y conserva sus escrituras hasta validar los datos, el código y las traducciones. Si hay conflicto, el motor vuelve a ejecutar desde el último estado confirmado. La placa conserva el orden de las interrupciones y el acceso a dispositivos. **Telemetría → CPU** muestra pasos fuera del bus, conflictos y espera muestreada. El APK incluye las revisiones de sus cuatro componentes en `assets/dependencies.lock.json`.

En **Prime r107**, una vCPU secundaria puede completar un bloque validado de RAM bajo el mismo turno del bus. La placa respeta la prioridad de las interrupciones y los temporizadores de los otros procesadores. **Telemetría → CPU** muestra la duración real y los pasos del último bloque muestreado. La comparación de versiones mide aparte el trabajo completo de dos vCPU, incluido el arranque de la secundaria y la entrega de la interrupción.

## Correcciones de arranque

Cada arreglo de un fallo de Windows amplía el componente de la máquina virtual que interviene en él con las técnicas de Éter, QSSD o GPU Virtual, según el componente. Junto con la corrección se verifica la mejora del componente y su comportamiento en el invitado.

## Gestión y diagnóstico de RAM

Prime r109 selecciona `EtherStateRam` al construir la máquina, antes del BIOS.
El espacio de RAM de 64 GiB usa estados de base compactos; las celdas vinculadas
al motor cuántico tienen una única autoridad vectorial. CPU, DMA y accesos de
bloques consultan la misma memoria. La prueba de equipo ejecuta instrucciones
x86 que leen 41 y escriben 42 en celdas del motor real, después de comprobar
interferencia de fase. [Arquitectura, pruebas y límites](docs/prime-r109-state-ram.md)

La preparación vuelve a comprobar su elegibilidad mientras corre el invitado y
al reanudarlo. La caché conserva páginas válidas al cambiar de tamaño. El panel
**Telemetría → RAM** y el diagnóstico exportado separan el coste de conseguir
espacio durante una escritura y el de recuperar datos del respaldo.
[Comportamiento, contadores y límites de las mediciones](docs/ram-pressure-cycles.md)

## Dependencias de compilación

Desde la raíz del repositorio, ejecuta `python3 scripts/verify_deps.py --checkout` para sincronizar las revisiones exactas del archivo de dependencias. Después puedes compilar con CMake o desde el proyecto `android/`. Ambos builds verifican la revisión de Mundo X al configurar y recompilar. El diagnóstico de Éter ARM64 incluye el SHA que se compiló y los contadores de reutilización de páginas de código.
