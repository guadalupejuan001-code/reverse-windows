# REVERSE Windows

Emulador de PC x86/x64 para arrancar Windows 10 como sistema invitado real.

**Estado:** reconstrucción técnica iniciada el 12 de noviembre de 2026. No se afirma que Windows 10 ya arranque ni que exista un escritorio compatible.

Reglas del proyecto:

- Sin interfaces que simulen un escritorio Windows o produzcan imágenes de arranque falsas.
- Sin copiar APK, versiones o implementaciones antiguas de laboratorios anteriores.
- El núcleo de CPU debe ejecutar instrucciones x86/x64 auténticas, con errores explícitos para operaciones no soportadas.
- Dependencias actuales fijadas por SHA y verificadas en CI; los módulos experimentales permanecen identificados como tales.
- Cada avance se demuestra con pruebas del huésped real y registros reproducibles; compilar en un host Windows no equivale a arrancar Windows como invitado.
- Nunca publicar ni distribuir imágenes ISO privadas o con derechos de terceros. Las ISO del usuario permanecen en su dispositivo.

Los cambios de arquitectura y el primer ejecutor de pruebas se desarrollan en ramas y PR independientes antes de su integración a `main`.

## Tarjeta de vídeo Prime

Prime r105 utiliza el estado compartido de Éter para seleccionar geometría y reutiliza los dibujos idénticos antes de llegar a GLES. La auditoría describe la ruta de vídeo de arranque, el controlador Windows, las capacidades actuales y las mediciones de imágenes y transferencias. [Auditoría y resultados de la GPU](docs/prime-r105-gpu.md)

En **Prime r106**, los procesadores secundarios agrupan búsquedas, desplazamientos, puestas a cero y multiplicaciones hasta el siguiente evento que pueda afectar al invitado. El panel de Android muestra los pasos por segundo en tiempo real y durante la ejecución del motor. En **Equipo → Comprobar equipo virtual** se puede verificar un par de Bell en una máquina temporal.

El dispositivo PCI acepta circuitos Clifford de 1 a 64 puertas sobre el estado cuántico compartido por CPU y VRAM. El invitado puede enviarlos mediante `RWGPU_QUANTUM_BLOCK` o mediante DMA con `RWGPU_DMA_QUANTUM_BLOCK`. Los controles y destinos se expresan como posiciones de bits. Un registro inválido rechaza el circuito completo. [Formato de los registros](src/c08_gpu_protocol.h) · [Protocolo DMA](src/c08_gpu_dma.h)

En **Prime r108**, las vCPU ARM64 ejecutan en paralelo bucles x86 que leen o escriben palabras de RAM en direcciones estables. Cada trabajador captura sus entradas, libera el bus y conserva sus escrituras hasta validar los datos, el código y las traducciones. Si hay conflicto, el motor vuelve a ejecutar desde el último estado confirmado. La placa conserva el orden de las interrupciones y el acceso a dispositivos. **Telemetría → CPU** muestra pasos fuera del bus, conflictos y espera muestreada. El APK incluye las revisiones de sus cuatro componentes en `assets/dependencies.lock.json`.

En **Prime r107**, una vCPU secundaria puede completar un bloque validado de RAM bajo el mismo turno del bus. La placa respeta la prioridad de las interrupciones y los temporizadores de los otros procesadores. **Telemetría → CPU** muestra la duración real y los pasos del último bloque muestreado. La comparación de versiones mide aparte el trabajo completo de dos vCPU, incluido el arranque de la secundaria y la entrega de la interrupción.

## Correcciones de arranque

Cada arreglo de un fallo de Windows amplía el componente de la máquina virtual que interviene en él con las técnicas de Éter, QSSD o GPU Virtual, según el componente. Junto con la corrección se verifica la mejora del componente y su comportamiento en el invitado.

## Dependencias de compilación

Desde la raíz del repositorio, ejecuta `python3 scripts/verify_deps.py --checkout` para sincronizar las revisiones exactas del archivo de dependencias. Después puedes compilar con CMake o desde el proyecto `android/`. Ambos builds verifican la revisión de Mundo X al configurar y recompilar. El diagnóstico de Éter ARM64 incluye el SHA que se compiló y los contadores de reutilización de páginas de código.
