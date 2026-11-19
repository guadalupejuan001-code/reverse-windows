# Prime r103 · vCPU, Éter y VRAM

La consigna ejecutada fue «Arranca CPU, GPU y respaldo de VRAM en el mismo reinicio de placa. Comprueba los 16 procesadores, conserva la entrega de interrupciones y mide el trabajo útil de los procesadores activos. Conecta la VRAM a la sesión Éter y verifica los datos antes y después de cada transferencia».

## Rutas del arranque

| Componente | Ruta de producción | Qué comprueba la prueba |
| --- | --- | --- |
| Topología x86 | C06 publica CPUID y MADT. El BSP empieza en el reinicio de placa y cada AP entra por su vector SIPI. | 8 núcleos con 2 hilos, identificadores APIC distintos y 15 incrementos atómicos de un contador en RAM compartida. La sesión detallada distingue los estados de espera por SIPI, inicio pendiente, ejecución y HLT. |
| Planificación e IRQ | El planificador C06 comprueba el LAPIC de destino, el temporizador local y los dispositivos compartidos antes de conceder un lote. Un AP detenido con IF habilitado conserva sus fuentes de despertar. | REP en un AP mientras la IRQ va al BSP. IPI y temporizador despiertan un AP y el invitado emite EOI. Se compara el mismo presupuesto de instrucciones con 15 AP dormidos. |
| Éter ARM64 y vectores | El primario utiliza el ejecutor de Mundo X. Los secundarios usan bloques ARM64, REP, escrituras agrupadas y vectores según el código invitado, el presupuesto y las interrupciones. | Registros, flags, memoria, pasos retirados, interrupciones y comparación con el intérprete en la matriz x86-64 y ARM64. |
| InfiniteCore | El puerto cuántico de la CPU, su ventana física Éter y los comandos cuánticos PCI C08 comparten la misma instancia de InfiniteSession. | Escrituras cruzadas, invalidación por época, puertas H y CX, correlaciones de Bell, rechazo atómico de operandos en superposición y recuperación del respaldo. |
| RAM y almacenamiento | Las páginas x86 usan MemoryFabric. BIOS, ATA y ATAPI llegan a sus controladores existentes. RAM transitoria, disco invitado y VRAM tienen respaldos distintos. | El suite nativo cubre RAM dispersa, presión, paginación, disco, CD-ROM y arranque de ISO sintética. |
| Pantalla de arranque | VBE lee páginas modificadas, GPU-VIRTUAL planifica rectángulos, qSSD publica las revisiones y Android presenta la imagen. | Comparación de píxeles. La telemetría VBE cuenta los bytes de la copia de pantalla que mantiene el capturador. C08 informa por separado de sus propios dibujos y memoria. |
| GPU PCI, DMA y qSSD | C08 abre y valida el volumen de VRAM en el reinicio de la placa, antes de la primera instrucción del BSP. C13 y ReGraphics conservan la geometría y el renderizado 3D. | Cabecera inicial de 64 B, cero páginas C08 útiles al encender, datos persistentes tras reabrir el volumen, DMA, fences, MSI y dibujos por el backend gráfico. |

## VRAM cuántica

El espacio de direcciones tiene 120 000 000 000 bytes lógicos. La dirección de byte Éter es `1000000000000 + offset`. Los primeros 256 bytes coinciden con la ventana física Éter del procesador. Las dos vistas comparten el reloj virtual de placa a 4 500 000 000 000 Hz. Los ciclos se calculan a partir del avance de la máquina. El tiempo de las transferencias se mide por separado en el anfitrión.

El invitado descubre `RWGPU_CAP_QUANTUM_VRAM` y el tamaño máximo de 256 B por transferencia. Puede almacenar y cargar bytes en base computacional, aplicar H a un bit o CX a dos bits, y guardar o restaurar un tramo de bytes en la VRAM persistente a la misma dirección. Una lectura clásica de un tramo con un bit en superposición devuelve estado 6. La prueba verifica que el par entrelazado, el destino y el diario sobreviven intactos. Los estados cuánticos en ejecución están sujetos a los límites de InfiniteCore de la máquina. El diario qSSD guarda los bytes materializados explícitamente.

La caché de observaciones usa la época de la sesión compartida. Cambiar el estado desde la CPU invalida la lectura anterior. En la VRAM de bytes, cada página indica qué intervalo contiene datos válidos. `resident_vram_bytes` mide las páginas ocupadas de las cachés C08, `cached_payload_bytes` cuenta los bytes válidos de la caché de transferencias y `cache_reserved_bytes` mide la capacidad reservada por el proceso. `qssd_backing_bytes` es el tamaño físico del volumen de respaldo. Android muestra también la memoria de captura VBE en su propia fila.

El transporte WDM incluye `RWGPU_IOCTL_QUANTUM`. Con ese controlador de desarrollo instalado en el invitado, `prime_gpu_scene.exe --quantum` comprueba el último byte y recupera su valor. El acceso de arranque por PCI también se prueba con instrucciones MOV de un invitado x86 independiente del controlador de Windows.

## Reproducción

La acción nativa publica `prime-r103-measurements-aarch64` y `prime-r103-measurements-x86_64`. El script `tests/compare_r103.sh` compara r102 y r103 en siete pasadas alternadas sobre el mismo anfitrión. Verifica la misma suma de comprobación en 200 000 lecturas de VRAM, el presupuesto retirado en el primer turno y el tiempo que requiere completar el mismo trabajo en el BSP. La prueba `reverse_windows_prime_gpu_guest_test` recorre la ruta PCI y compara lecturas cuánticas con y sin caché en el mismo ejecutable.
