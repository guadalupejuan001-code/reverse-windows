# REVERSE Windows para Android

La app conecta el selector de archivos de Android, el arranque BIOS de Mundo X y el disco persistente QSSD en el mismo `PcMachine`. Las revisiones de los componentes se fijan en `config/dependencies.lock.json`.

## Probar en el teléfono

1. Instala el APK ARM64. Usa **Probar BIOS** y pulsa **Enter** cuando el huésped pida una tecla.
2. Apaga la prueba. Pulsa **Crear QSSD de 64 GiB**. El disco tiene 64 GiB lógicos y usa almacenamiento físico conforme recibe escrituras.
3. Selecciona tu ISO local y pulsa **Arrancar desde ISO**. Android abre el archivo en modo de lectura y lo presenta como CD/DVD.
4. Si Windows Setup llega a mostrar el disco, elige ese QSSD para crear las particiones. Después de que Setup termine de copiar y prepare el reinicio, arranca desde **QSSD** si el sistema vuelve al CD.
5. Si la máquina se detiene, usa **Copiar diagnóstico**. El informe incluye el modo de CPU, el punto de ejecución, el motivo del fallo y el estado del disco.

Desinstalar la app borra su disco privado. Para conservar la instalación, actualiza con un APK firmado con la misma clave.

## Actualización 0.1.1

La CPU conserva el descriptor de pila heredado del modo real hasta que el cargador vuelve a cargar `SS`. La prueba reproduce el `PUSH 0x50` en `2000:083F` del diagnóstico en Samsung y comprueba que el invitado continúa. Los cambios de selector posteriores siguen sujetos a las comprobaciones de privilegio.

El Ether Next de Mundo X añade reducción de lotes XI y caché de transformaciones. En ARM64, el decodificador usa NEON en formas SSE2 enteras de registro admitidas. El intérprete conserva el fetch, el avance de RIP y las interrupciones. `Copiar diagnóstico` incluye el número de instrucciones invitadas que realmente pasaron por NEON. En modo protegido de 16 bits este contador seguirá en cero. Las capacidades CPUID conservan el perfil de la CPU anterior.

## Qué se comprobó

- Un huésped El Torito lee su ISO, recibe una tecla por BIOS, muestra salida VGA, escribe un MBR en QSSD y arranca desde ese MBR al reabrir el disco. Se comprueba que la ISO conserve los mismos bytes.
- Un huésped x86 en modo protegido usa ATAPI PIO en el IDE secundario, recibe IRQ15, lee dos sectores y obtiene `DATA PROTECT` ante una orden de escritura.
- Un huésped en modo protegido recibe teclado PS/2 por IRQ1. La pantalla de Android presenta texto VGA y píxeles VBE de la máquina. GPU Virtual calcula las actualizaciones del modo texto.

## Límites

Se verificó el flujo con huéspedes sintéticos. Windows 10 todavía tiene pendiente su primer arranque con la ISO del usuario. La CPU conserva la cobertura parcial de SSE2/x87 y CPUID de la PR #19. IDE/ATAPI y PS/2 tienen cobertura parcial. El controlador gráfico de Windows y el reinicio del instalador desde QSSD se deben verificar con Windows real.

## Compilar

Con los componentes fijados, JDK 17 y el SDK de Android, ejecuta `python3 scripts/verify_deps.py` desde la raíz y luego `./gradlew :app:assembleDebug` desde `android/`. El workflow `.github/workflows/android-boot.yml` prepara las versiones del SDK, verifica la firma y ABI y publica el APK ARM64.
