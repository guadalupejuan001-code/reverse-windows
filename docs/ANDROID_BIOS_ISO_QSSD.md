# REVERSE Windows para Android

La app conecta el selector de archivos de Android, el arranque BIOS de Mundo X y el disco persistente QSSD en el mismo `PcMachine`. Las revisiones de los componentes se fijan en `config/dependencies.lock.json`.

## Probar en el teléfono

1. Instala el APK ARM64. Usa **Probar BIOS** y pulsa **Enter** cuando el huésped pida una tecla.
2. Apaga la prueba. Pulsa **Crear QSSD de 64 GiB**. El disco tiene 64 GiB lógicos y usa almacenamiento físico conforme recibe escrituras.
3. Selecciona tu ISO local y pulsa **Arrancar desde ISO**. Android abre el archivo en modo de lectura y lo presenta como CD/DVD.
4. Si Windows Setup llega a mostrar el disco, elige ese QSSD para crear las particiones. Después de que Setup termine de copiar y prepare el reinicio, arranca desde **QSSD** si el sistema vuelve al CD.
5. Si la máquina se detiene, usa **Copiar diagnóstico**. El informe incluye el modo de CPU, el punto de ejecución, el motivo del fallo y el estado del disco.

Desinstalar la app borra su disco privado. Para conservar la instalación, actualiza con un APK firmado con la misma clave.

## C03 · De BIOS al QSSD

La prueba arranca un CD El Torito de solo lectura. El huésped valida la unidad de arranque del BIOS, cambia a modo protegido, encuentra el controlador PCI IDE y lee `IDENTIFY DEVICE` tras recibir IRQ14. Comprueba los **64 GiB** y escribe la confirmación en VGA. Después de Enter, escribe un MBR de prueba por BIOS, compara sus 512 bytes por ATA y arranca desde el mismo QSSD. El MBR vuelve a arrancar al abrir el disco en una sesión nueva.

**Comprobar equipo virtual** ahora prueba el archivo y la capacidad ATA por separado. El registro de la sesión cuenta las respuestas IDENTIFY que el invitado leyó enteras. Ese contador permite saber si la sesión alcanzó ATA aunque la pantalla siga en negro. La detección por Windows requiere la prueba con su instalador.

## C02 · Conformidad de CPU

MMX comparte los registros físicos de x87. `EMMS` marca vacías sus etiquetas. Las pruebas cubren operaciones empaquetadas, transferencias MMX/XMM y conversiones. SSE/SSE2 incorpora `RCP`, `RSQRT`, `MASKMOVDQU` y `MOVNTI`, además de comprobar la prioridad de excepciones con subnormales y el redondeo cerca del umbral FTZ.

La aritmética x87 usa un formato de 80 bits independiente de `long double`, con precisión de 24, 53 y 64 bits y los cuatro modos de redondeo. Las pruebas cubren conversiones, etiquetas, entornos x87 y entrega de `#MF` en modo real, protegido y compatibilidad IA-32e. La placa entrega IRQ13 mediante el PIC cuando el huésped selecciona la ruta FERR heredada. `FXSAVE` y `FXRSTOR` se prueban con ambos formatos de puntero en modo largo, conservación de XMM/MXCSR, y guardado y recuperación de imágenes repartidas entre páginas.

Las pruebas se ejecutan en anfitriones x86_64 y ARM64. El primero compara resultados con instrucciones x86 del procesador físico. Ambos ejecutan los casos de referencia y la misma integración de `PcMachine`. CPUID anuncia MMX, FXSR, SSE y SSE2. El bit x87 sigue desactivado mientras falten ocho instrucciones trascendentales. La instalación de Windows 10 sigue sin verificar.

## C01 · Cobertura de CPU SSE/SSE2

La CPU integra aritmética SSE de precisión simple, comparaciones, movimientos, conversiones SSE2 entre datos de 32 y 64 bits, y operaciones enteras empaquetadas. Las pruebas comprueban resultados y excepciones contra x86 y ejecutan los mismos casos en ARM64. `FXSAVE` y `FXRSTOR` preservan el acceso a x87 con `CR4.OSFXSR=0`. La integración prueba también el recorrido sintético BIOS, ISO, teclado, QSSD y reinicio del huésped.

El perfil CPUID de C01 dejó las capacidades SSE, SSE2, FXSR y x87 desactivadas mientras se completaban las pruebas de conformidad.

## Actualización 0.1.4

**Comprobar equipo virtual** ejecuta una prueba de RAM, VGA y detección PCI IDE, consulta CPUID y revisa la entrada BIOS de la ISO y el QSSD en modo de solo lectura. El panel muestra la entrega al cargador, cambios de modo de CPU, lecturas, escrituras confirmadas y el motivo de un fallo aunque VGA esté en negro. La ausencia de SSE2, x87 y FXSR en el perfil anunciado se muestra como comprobación pendiente.

La CPU ejecuta `LOCK OR` y las variantes de RAM documentadas de la familia atómica x86. La petición de reinicio del huésped sincroniza el QSSD, comprueba si contiene un sector de arranque y selecciona QSSD o la ISO montada. La prueba integrada escribe un MBR, solicita el reinicio desde el propio huésped y arranca desde el mismo QSSD. WinPE visible con un QSSD escribible y el reinicio de Windows se deben comprobar con el medio real.

## Actualización 0.1.3

La ISO en el Samsung superó `0050:089D` y llegó a modo protegido de 32 bits. Se detuvo en `0020:446056` al leer el registro `IA32_TSC` (`RDMSR`, `ECX=0x10`). La CPU expone por `RDMSR` el mismo contador virtual que por `RDTSC` y permite ajustarlo por `WRMSR` desde CPL0. El contador sigue midiendo pasos del intérprete y no tiempo de pared. Los registros MSR pendientes se identifican por su índice en el diagnóstico.

## Actualización 0.1.2

La prueba con la ISO en el Samsung superó `2000:083F` y se detuvo al ejecutar `0F 22 C0` (`MOV CR0,EAX`) en `0050:089D`. El cargador quería volver de modo protegido de 16 bits a modo real. La CPU admite esa transición y conserva los descriptores de segmento en caché. Una regresión de El Torito comprueba el regreso a modo real, las lecturas de BIOS y la persistencia del QSSD con un huésped sintético.

## Actualización 0.1.1

La CPU conserva el descriptor de pila heredado del modo real hasta que el cargador vuelve a cargar `SS`. La prueba reproduce el `PUSH 0x50` en `2000:083F` del diagnóstico en Samsung y comprueba que el invitado continúa. Los cambios de selector posteriores siguen sujetos a las comprobaciones de privilegio.

El Ether Next de Mundo X añade reducción de lotes XI y caché de transformaciones. En ARM64, el decodificador usa NEON en formas SSE2 enteras de registro admitidas. El intérprete conserva el fetch, el avance de RIP y las interrupciones. `Copiar diagnóstico` incluye el número de instrucciones invitadas que realmente pasaron por NEON. En modo protegido de 16 bits este contador seguirá en cero. Las capacidades CPUID conservan el perfil de la CPU anterior.

## Qué se comprobó

- Un huésped El Torito lee su ISO, recibe una tecla por BIOS, muestra salida VGA, escribe un MBR en QSSD y arranca desde ese MBR al reabrir el disco. Se comprueba que la ISO conserve los mismos bytes.
- Un huésped x86 en modo protegido usa ATAPI PIO en el IDE secundario, recibe IRQ15, lee dos sectores y obtiene `DATA PROTECT` ante una orden de escritura.
- Un huésped en modo protegido recibe teclado PS/2 por IRQ1. La pantalla de Android presenta texto VGA y píxeles VBE de la máquina. GPU Virtual calcula las actualizaciones del modo texto.

## Límites

Se verificó el flujo con huéspedes sintéticos. IDE/ATAPI y PS/2 tienen cobertura parcial. La pantalla del instalador, el reconocimiento del QSSD y el reinicio de Windows se deben comprobar con el medio real.

## Compilar

Con los componentes fijados, JDK 17 y el SDK de Android, ejecuta `python3 scripts/verify_deps.py` desde la raíz y luego `./gradlew :app:assembleDebug` desde `android/`. El workflow `.github/workflows/android-boot.yml` prepara las versiones del SDK, verifica la firma y ABI y publica el APK ARM64.
