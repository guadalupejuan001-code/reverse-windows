# Instalar Windows 10 de verdad — contrato C01

Objetivo: instalar Windows 10 como sistema **invitado** dentro de REVERSE Windows, no reproducir su interfaz.

## Hardware que ha de ver el instalador

1. Medio óptico: `--iso Windows10.iso` monta la ISO original **solo en lectura**, drive BIOS `E0h`.
2. Disco independiente: `--disk win10.rvdisk` monta una imagen de sectores de 512 bytes en `DL=80h`. El invitado puede solicitar servicios BIOS INT 13h AH 02/03 (CHS) y 42/43 (EDD lectura/escritura), con capacidad EDD y geometría CHS limitada.
3. Imagen persistente: `--create-disk-gib N` crea el archivo **solo si no existe**. Reabrirlo conserva las escrituras. Una imagen de 64 GiB ofrece espacio lógico, pero **los datos escritos consumen almacenamiento real**. No se modifica la ISO del usuario.
4. El adaptador se conecta a `PcMachine::cpu.interrupt_hook` y delega las solicitudes restantes al BIOS x86 original. La CPU y la RAM siguen perteneciendo al intérprete canónico de Mundo X, sin segundo procesador.

Ejemplo de diagnóstico **en una máquina anfitriona con CMake y la ISO local del usuario**:

```sh
cmake -S . -B build -DREVERSE_MUNDO_X_ROOT="$PWD/deps/mundo-X" -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure

./build/reverse_windows_boot_probe \
  --iso /ruta/local/Windows10.iso \
  --disk /ruta/local/Windows10.rvdisk \
  --create-disk-gib 64 \
  --ram-mib 2048 --resident-mib 1024 --steps 100000000 \
  --evidence /ruta/local/trace-boot.json
```

Reutilizar el disco después: **omitir** `--create-disk-gib`. No subir ISO, disco o registros con información privada a GitHub.

Para **arrancar desde el HDD** después de instalar un sistema con un sector de arranque válido:

```sh
./build/reverse_windows_boot_probe --boot-hdd \
  --disk /ruta/local/Windows10.rvdisk \
  --ram-mib 2048 --resident-mib 1024 --steps 100000000 \
  --evidence /ruta/local/hdd-reboot.json
```

La regresión C01 utiliza un MBR de prueba con bytes x86 auténticos, no un Windows instalado. La vía HDD es real pero Windows Setup todavía necesita más dispositivos e instrucciones. Los comandos de host todavía no equivalen a una APK Android.

## Lo que queda y no se debe fingir

- **BIOS HDD ≠ controlador de almacenamiento de Windows.** Después de arrancar, Windows necesita un dispositivo reconocible mediante PCI/ACPI, un controlador de disco compatible y soporte de interrupciones y DMA. Esto todavía no está implementado en REVERSE Windows.
- El intérprete x86/x64 conserva limitaciones importantes (SSE2/x87, ring 3, IDT/interrupciones protegidas, APIC/ACPI). Windows 10 completo no ha arrancado.
- El C01 no instala Windows de forma automática. Su prueba real usa un pequeño programa x86 que llama INT 13h, escribe el sector 7 y lo recupera desde el archivo persistente; es una prueba de hardware virtual concreto, no de Windows.
- QSSD no sustituye el disco: los snapshots consistentes de la máquina completa siguen siendo trabajo posterior. GPU Virtual/REgraphics tampoco sustituyen los controladores de vídeo que Windows reconozca.

## Gates

- D0: escrituras y lecturas BIOS reales por guest x86, verificadas contra fichero persistente y sin tocar ISO.
- D1: bootloader de ISO auténtica descubre la unidad virtual; nueva traza de fallos.
- D2: Windows PE / Setup reconoce controlador de almacenamiento nativo, particiona y formatea el disco.
- D3: instalador copia archivos, reinicia desde HDD y entra en el sistema Windows real.
- D4: sesión, entrada y gráficos reales verificados. Ningún logo o bitmap por sí solo cuenta.
