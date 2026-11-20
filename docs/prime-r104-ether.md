# Prime r104 · Arranque, Éter ARM64 y VRAM compartida

La tarea de esta revisión fue ejecutar las lecturas de segmento del logo de Windows en ARM64, recuperar el turno de un procesador secundario cuando la CPU de arranque duerme, preparar el motor GLES al encender la placa y guardar el estado entrelazado de la VRAM a través de un apagado.

## Procesadores y pantalla de arranque

Éter traduce las cargas MOV de memoria con prefijos FS y GS en modo de 64 bits. Obtiene la base del segmento de cada vCPU en cada despacho. La caché de lectura y las comprobaciones de acceso usan la dirección lineal completa. El intérprete conserva la gestión de los fallos precisos.

Cuando el reloj de Android está activo y el BSP duerme con un solo AP ejecutable, la placa comprueba los dispositivos cada 32 instrucciones como máximo y entrega al AP el presupuesto disponible. INIT, SIPI, el enmascaramiento de interrupciones y la prioridad APIC siguen bajo control del invitado.

Al reiniciar la placa, C08 abre el respaldo disperso y prepara el contexto GLES, el programa de sombreado y una superficie de un píxel antes de la primera instrucción x86. El primer dibujo reutiliza estos recursos. La telemetría guarda el resultado de la preparación, su duración y el nombre que devuelve GLES. Un fallo de inicialización permite continuar con el framebuffer VBE y reintentar GLES al recibir un dibujo. El comparador VBE comprueba tramos de píxeles consecutivos, incluidos los que usa Android al presentar 1024 × 768 a 960 × 720.

## Guardar y recuperar un estado entrelazado

El dispositivo anuncia la capacidad PCI `RWGPU_CAP_QUANTUM_STATE`. El controlador WDM de desarrollo expone dos operaciones explícitas con parámetros cero. En Windows se pueden invocar con `prime_gpu_scene --quantum-checkpoint` y `prime_gpu_scene --quantum-restore`. Restaurar reemplaza toda la sesión Éter compartida con la CPU, incluso los qubits externos al intervalo de VRAM. Así se conservan las correlaciones que atraviesan ese límite y la secuencia del generador de mediciones.

El volumen compañero `.qstate.rqvd` se crea al guardar por primera vez. Contiene dos espacios de hasta 16 MiB para el formato canónico MXI. La escritura confirma el contenido antes de cambiar el manifiesto. La restauración comprueba el tamaño, el CRC y el formato antes de publicar el estado. Una VM recién encendida comienza con el estado inicial hasta que el invitado solicita restaurar. Los comandos que guardan bytes determinados continúan disponibles.

Los 120 GB son el espacio lógico direccionable de la VRAM. GPU y VRAM comparten el reloj virtual de 4,5 THz de la placa. La pantalla Vídeo distingue el framebuffer, los bytes residentes de caché, el estado Éter transferido y los dibujos completados.

## Verificación reproducible

El contrato de GPU reinicia la VM y compara el estado canónico completo, incluidos los observables XX, ZZ y Z de un par de Bell entre CPU y GPU. También interrumpe una escritura en la ranura inactiva y altera la ranura activa para comprobar el rechazo sin pérdida del estado vivo.

`tests/compare_r104.sh` compara el código de r103 y r104 con las revisiones originales de Mundo-X y con el mismo número de instrucciones y la misma salida de vídeo. CI conserva siete mediciones alternadas por versión y arquitectura en `prime-r104-measurements`. Los tiempos son de pruebas de carga controladas. La medida del arranque de una ISO comercial requiere registrar el tiempo hasta que aparezca el instalador en el dispositivo Android.
