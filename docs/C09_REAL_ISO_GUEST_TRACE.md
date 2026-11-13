# C09 — Diagnóstico del arranque real de REVERSE Windows (ISO, CPU y periféricos)

Fecha: 12 de noviembre de 2026 (GitHub Actions reporta UTC 13 de noviembre). Este ciclo se desarrolla sobre **C08 PR #12**, con el plan de **PR #7**, sin fusionar automáticamente.

## Alcance y estado honesto

**Instrumentación y ensayos C09: implementados y validados con medios de prueba sintéticos.** El hito de arrancar **Windows PE desde una ISO original de Windows 10 permanece pendiente**: no se encontró ni se utilizó una ISO auténtica del usuario en estas ejecuciones. No se ha instalado Windows ni demostrado que su cargador supere una instrucción problemática real.

La máquina emulada conserva exactamente:
- `cosmic_pc::PcMachine` original de Mundo X (un único motor de CPU y sus RAM/paginación), con las extensiones C04–C07;
- `PcMotherboardC06` (PIC, APIC parcial, PCI y ACPI parcial), controlador IDE ATA PIO C07 e IRQ14;
- `BlockDisk` común entre BIOS DL=80h, ATA y QSSD original con checkpoints/compactación C08.

No se introdujo una CPU paralela, una pantalla falsa, otro motor de Windows ni una ISO recompuesta. No hay ejecución JIT ni software virtualizado asistido por hardware ARM que pueda afirmar Windows 10 funcional.

## Auditoría técnica del camino de arranque

1. **ISO/El Torito**: `BootMedia` lee la ISO local de forma estrictamente de solo lectura. Valida descriptores CD001, firma/catálogo, checksum y entrada x86 inicial sin emulación. Interpreta el tamaño de carga inicial en sectores de 512 B y las lecturas posteriores de CD como sectores de 2048 B. No soporta perfiles alternativos de El Torito sin extender explícitamente las pruebas.
2. **Firmware y BIOS**: `PcMachine::disk_bios` responde INT 13h AH=42h para la unidad óptica E0 y lee sectores posteriores reales del archivo ISO. El `BiosHardDisk` separa DL80 para QSSD sin duplicar el soporte de unidad óptica.
3. **CPU**: se registran CS:IP/RIP antes y después de cada instrucción, bytes realmente decodificados, opcode, modo, CPL, flags, pila, CR2/CR3, walks de la MMU, excepción entregada e información de fallo. La traza de instrucción se genera desde la CPU canónica, **no mediante lectura aproximada de memoria física que ignoraría paginación**.
4. **Dispositivos**: mediante wrappers que delegan en los hooks anteriores se observan puertos IN/OUT, operaciones MMIO, llamadas BIOS, vectores de IRQ externos realmente entregados, estados ATA/IRQ14 y commits QSSD. Un callback de observación que falle no debe alterar las instrucciones invitadas.
5. **Finalización y bloqueos**: se conserva un evento final incluso si se alcanzó el límite de eventos. La traza de un fallo guarda el opcode y la razón terminal aun con cuota agotada. `scripts/c09_diagnose_trace.py` resume el primer bloqueo **observado**, sin inferir que el medio sea Microsoft Windows.

## Ejecución local sobre una ISO auténtica

Este ejemplo **no se ejecutó en CI porque la ISO original no está disponible**. Sólo debe usarse con una ISO de Windows 10 obtenida legalmente y almacenada localmente; ninguna ISO se sube a GitHub.

```bash
# Sólo cuando no exista todavía disco.rqvd:
reverse_windows_boot_probe --iso /ruta/Windows10.iso \
  --qssd-disk /ruta/disco.rqvd --create-qssd-gib 64 \
  --ram-mib 2048 --resident-mib 256 --steps 2000000 \
  --trace /ruta/c09-traza.jsonl --trace-max-events 50000 \
  --evidence /ruta/c09-estado.json > /ruta/c09-salida.json

# Si ya existe disco.rqvd, OMITIR --create-qssd-gib.
python3 scripts/c09_diagnose_trace.py /ruta/c09-traza.jsonl \
  --output /ruta/c09-diagnostico.json
```

**Seguridad:** el ejecutable rehúsa crear una traza si el archivo de destino ya existe o coincide con la ISO o el volumen. La ISO se abre para lectura; la máquina puede escribir únicamente en el QSSD/RAW elegido. No se activa ningún proceso en segundo plano.

La ejecución con ISO auténtica puede terminar con código 2 por un opcode, MMU o dispositivo no soportado. Este resultado **es evidencia útil para corregir el siguiente bloqueo**, no un arranque exitoso.

## Formato de evidencia C09

`--trace` escribe JSON Lines (`reverse-windows-c09-trace-v1`). Eventos:

- `start`: CPU, modo, CS:RIP inicial, procedencia y cuota.
- `el-torito-boot-plan`: entrada x86, segmento inicial, origen de la imagen, tamaño en sectores y unidad.
- `instruction`: secuencia exacta desde `Cpu::step`; CS:IP/RIP/opcode/bytes/mode/CPL/MMU/excepción/flags/CR2/CR3.
- `mode-transition`: cambios reales a protected16/protected32/long64. Los primeros 256 eventos críticos se conservan aunque se agote la cuota de detalle; el resto se contabiliza como descartado para impedir archivos ilimitados.
- `bios-interrupt`: vector, AH y DL (corregido en C09), resultado Carry y lecturas del medio.
- `io-in`, `io-out`, `mmio-read`, `mmio-write`: puerto/dirección/ancho/valor y si el dispositivo aceptó la operación.
- `hardware-irq`: vector realmente entregado por la CPU y CS:RIP origen/destino.
- `terminal-fault`: bytes y razón real del primer fallo terminal, fuera de la cuota si es necesario.
- `finish`: estado de máquina, instrucciones observadas, eventos descartados, walks, interrupciones, contadores ATA/QSSD y `windows_boot_verified:false`.

`scripts/c09_diagnose_trace.py` produce `reverse-windows-c09-diagnostic-v1` y siempre marca `windows_10_boot_verified:false`, `winpe_boot_verified:false`, `guest_image_identity_verified:false`. Una etiqueta `reached_cs_0050_ip_0854:true` **solo indica esa dirección en la CPU**, no la identidad del código.

## Pruebas y resultados

### CPU Mundo X

- Programa x86 real en modo real: `MOV AX,0x1234` y `HLT` generan dos eventos exactos.
- Opcode `F0` no soportado produce `fault` y conserva el CS:IP original.
- Programa x64 entrega #UD (UD2) a la IDT, permite reanudación mediante IRETQ y registra el vector y bytes `0f0b`.
- Interrupción de hardware real-mode IVT vector `0x20` se registra sin modificar el estado.
- Un callback que lanza excepciones no interfiere con el huésped.
- Se ejecutó la suite nativa completa de Mundo X junto a C04/C05/C06/C07.

### REVERSE Windows

**ISO sintética generada por nosotros:** `tests/c09_synthetic_iso.py` usa plataforma x86, entrada de El Torito `2000:0000` y lectura posterior por INT 13h AH42. El código x86 real consulta PCI config CF8/CFC, encuentra el IDE C07, emite ATA IDENTIFY, cambia a protected16 y ejecuta **`0F 00 D8` en `0050:0854`**. Concluye mediante HLT. NO es la ISO ni el cargador de Windows 10.

**Traza de hardware:** otro huésped en modo protegido programa PIC+ATA, escribe por PIO al QSSD y recibe IRQ14 del esclavo con vector `0x2e`; registra lectura y escritura en LAPIC MMIO y vuelve mediante IRETD. El sector se recupera idéntico tras reabrir QSSD.

**Casos negativos:**
- Prefijo LOCK (`F0`) no implementado, en una imagen RAW propia, produce un error final visible con opcode exacto.
- Una cuota de trazas mínima descarta detalles no críticos, pero retiene cambios de modo y el estado final.
- Un catálogo El Torito con checksum alterado se rechaza, sin ejecutar el huésped.
- La traza nunca puede sobrescribir una ISO o una imagen de disco existente.
- El diagnóstico identifica correctamente un fallo terminal y la diferencia respecto a un HLT de prueba.

Las **13 pruebas CTest** incluyen 11 pruebas C00–C08, una CPU C09 y una nueva prueba de trazado del hardware C09; el pipeline además ejecuta la ISO sintética y todos los casos negativos.

## Limitaciones y siguientes acciones

1. No hay un Windows 10 ISO auténtico disponible en este ciclo, de modo que **no se puede señalar la primera instrucción que bloquea el instalador real**. El histórico `0050:0854` sigue sin revalidarse con esa ISO. Superarlo en una ISO sintética sólo confirma la funcionalidad del intérprete.
2. La CPU todavía no anuncia SSE2; x87, varios estados privilegiados, dispositivos ACPI/APIC, temporizadores y rutas del cargador de Windows siguen incompletos.
3. La traza es de depuración: reduce rendimiento y genera archivos que pueden ocupar decenas de MB. No representa mediciones de FPS, exactitud de temporizadores ni eficiencia energética Android.
4. El código de BIOS y los dispositivos conservan limitaciones de C06/C07. No se implementó UEFI ni un soporte universal de ISO El Torito.
5. Una prueba exitosa de HLT no equivale a Windows PE vivo. Hasta tener una ISO original y una traza concreta del primer bloqueo, **C09 no cumple su puerta final de Windows PE**.

El próximo paso técnico concreto es obtener una traza local de una ISO de Windows 10, clasificar el primer bloqueo real con el diagnóstico y diseñar el parche mínimo en su repositorio de autoridad, manteniendo regresiones y la evidencia.
