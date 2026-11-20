# Prime r109: RAM Éter de estados

Esta rama compila REVERSE Windows 0.3.84, código de versión 114, con el paquete
Android `com.reversewindows.android.prime.smp.r109`. Conserva las mejoras de
presión de RAM y ejecución de vCPU de la base `82718583fe628dc06355dba5cd0fca3582223128`.
Las revisiones exactas de los cuatro componentes están en
`config/dependencies.lock.json` y se copian dentro del APK.

## Memoria que usa el invitado

`MUNDO_X_PC_STATE_RAM=1` selecciona `EtherStateRam` en `MemoryFabric` desde su
construcción. En este perfil no se crea un objeto `SparseRam`. Se reutilizan
sus contratos de respaldo y tipos de contadores para conservar la integración
con Android y qSSD.

El perfil Prime ofrece 64 GiB de RAM utilizable: 68.719.476.736 bytes. El espacio
de direcciones físicas puede ser mayor por el hueco reservado para PCI/MMIO.
Una página sin escribir representa ceros. Las páginas escritas usan reglas de
base, diferencias de palabras o estados de base empaquetados. Esta representación
es exacta para los bytes normales del invitado y evita reservar 64 GiB físicos
al abrir la aplicación.

Las celdas cuánticas vinculadas usan `QuantumRamAuthority`, cuyo motor procede
del núcleo actual de Mundo X y las representaciones adaptativas, reducidas y MPS
de REVERSE-PC. Vincular una celda transfiere su byte previo al motor y elimina
su valor anterior de la representación de base. Las lecturas y escrituras de
CPU y DMA consultan esa autoridad, sin una segunda memoria de bytes que decida
el resultado. Una lectura no determinista exige una medición explícita o las
operaciones inversas correspondientes.

Los bytes normales de una ISO de Windows permanecen en la representación de
base. No se convierten automáticamente en amplitudes complejas. El proveedor
cuántico está conectado a la máquina y sus operaciones se ejercitan en la
prueba de equipo; sus límites de recursos son explícitos.

## Integración y consistencia

- Las vCPU comparten `MemoryFabric`, incluida la autoridad cuántica.
- Los accesos escalares, de bloques, DMA, exploración y restauración pasan por
  el mismo backend. Los punteros de lectura directa no cubren celdas cuánticas.
- Una revisión cuántica nueva invalida las vistas de código y traducción; la
  notificación explícita invalida también la observación de vídeo.
- Las escrituras de una instrucción conservan un punto de restauración de la
  autoridad cuántica. Un fallo no debe dejar bytes, fases o correlaciones de
  una instrucción incompleta.
- El respaldo temporal qSSD y la regulación de presión de Android permanecen
  activos. Las métricas separan el contenido residente, los metadatos y el
  contenido cuántico comprometido.

Los encabezados de compatibilidad vectorial se generan únicamente para una
unidad de compilación con espacios de nombres privados. El generador valida
los SHA-256 y anclajes revisados de las fuentes. No modifica los checkouts ni
reemplaza el núcleo de CPU/GPU utilizado por los demás componentes. También
conserva el efecto de `T; T`: estas puertas no son involuciones y no se cancelan
como dos puertas X.

## Comprobaciones incluidas

`reverse_windows_quantum_ram_authority_test` comprueba el proveedor real y sus
operaciones, límites y restauración. `reverse_windows_ether_state_ram_integration_test`
comprueba la integración con la memoria y CPU x86. Los demás ensayos del
proyecto siguen compilando contra la memoria seleccionada para r109.

**Equipo → Comprobar equipo virtual** y el modo de prueba del APK ejecutan una
máquina temporal independiente de los discos del usuario:

1. Vinculan dos bytes al motor real dentro de un espacio de 64 GiB.
2. Comprueban la interferencia de `H; T; H`, rechazan una lectura sin medir y
   aplican la transformación inversa.
3. Una puerta X transforma el byte de origen de 40 a 41.
4. La CPU ejecuta `MOV AL,[origen]`, `ADD AL,1`, `MOV [destino],AL` y `HLT`.
5. El destino debe contener 42 en la autoridad cuántica, con dos bytes
   vinculados y cero sobreescrituras clásicas dentro de ese proveedor.

El diagnóstico incluye `ram_profile.memory_backend = "ether-state-ram"` y
`quantum_ram_probe`, con el resultado y el trabajo realmente ejecutado. CI
exige ese backend tanto en la prueba interna como en el arranque de una ISO
sintética. El artefacto conserva el manifiesto, firma, hash, ABI y lockfile de
las fuentes que se compilaron.

## Significado de capacidad y frecuencia

Los 64 GiB son la capacidad que presenta la máquina virtual. Los datos que
están presentes siguen ocupando recursos del anfitrión o de su respaldo; una
representación compacta no garantiza comprimir cualquier contenido arbitrario.
La CPU anfitriona ejecuta el emulador y la CPU virtual ejecuta las instrucciones
del invitado consultando esta memoria.

`ram_clock_hz = 4.500.000.000.000` conserva el reloj del modelo. No es una
medición de ancho de banda, latencia ni frecuencia física del teléfono. La
velocidad real se observa mediante pasos retirados y tiempo transcurrido.

El APK no incorpora Windows ni prueba por sí solo una instalación completa
de Windows. Esa validación requiere la ISO del usuario y un arranque real.
