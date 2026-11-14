# Prime C02 · perfil de arranque

La prueba nativa mide el mismo tramo del invitado El Torito con y sin cronómetro. El disco QSSD de 64 GiB lógicos se crea vacío en cada pasada. El invitado copia RAM con `REP`, ejecuta aritmética, consulta el controlador IDE, recibe IRQ14 y llega al prompt de teclado verificado en C01. Una segunda prueba toma muestras en `long64` con paginación activa.

```sh
python3 tests/prime_boot_profile.py build/reverse_windows_boot_probe \
  --evidence build/prime-c02-boot-profile.json
```

Ambas arquitecturas guardan la huella de la imagen sintética, el estado del invitado y tres pares de mediciones en el artefacto de CI. Para repetir un intervalo desde una ISO local se puede usar el ejecutable nativo con los mismos parámetros de entrada y una copia inicial idéntica del disco.

```sh
build/reverse_windows_boot_probe --iso ./guest.iso \
  --qssd-disk ./profile-run.rqvd --create-qssd-gib 64 \
  --ram-mib 2048 --resident-mib 256 --ram-spill-directory /tmp \
  --profile-from 32768 --profile-until 229376 --profile-every 32768 \
  --steps 500000 --evidence ./profile-run.json
```

`--profile-from`, `--profile-until` y `--profile-every` usan pasos lógicos x86, incluidos los elementos de `REP`. `--steps` limita los ticks de la placa. El ejecutable devuelve código 3 si el invitado se detiene antes de `--profile-until`. Para una pasada de control, usar `--profile-counts-only` y un disco inicial equivalente. Si el invitado lee el reloj del BIOS, recibe entrada o arranca desde un disco con otros datos, comprobar el estado antes de comparar tiempos.

| Campo | Qué mide |
| --- | --- |
| `profile_wall_ns` | Tiempo total del intervalo en la sonda nativa. La preparación de medios y la captura final de vídeo ocurren fuera del intervalo. |
| `boot_profile` | Estado inicial, actual y los ocho últimos puntos con registros, paginación, RAM, actividad de la placa y trabajo acelerado. Los puntos permiten comparar dos ejecuciones. Cargar un punto como estado de VM queda fuera de este ciclo. |
| `boot_time_samples` | Cada ventana de hasta 16 iteraciones mide consultas a la placa, intentos y ejecución por lote, CPU escalar y resto del bucle. La CPU escalar subdivide búsqueda y decodificación, otros accesos a RAM, llamadas de E/S y resto. Una medición exterior incluye el trabajo interno. `run_ns` y `compaction_ns` son acumulados del intervalo. |
| `boot_code_regions` | Tiempo escalar muestreado por región de 64 bytes, modo y CS. Cada límite inferior resta el error del contador. Una región domina a otra cuando su límite inferior supera el límite superior de la otra. Los límites acotan los pesos observados en las muestras. |
| `profile_iso_read_ns`, `profile_spill_read_us`, `profile_spill_write_us` | Tiempo acumulado de lectura de ISO y del respaldo de RAM cuando se habilita. Ya forman parte del tiempo transcurrido durante el intervalo. |

El temporizador añade coste en cada muestra. Comparar la mediana de pasadas emparejadas en la misma máquina para evaluar ese coste. Las sumas de las muestras describen el trabajo observado en las ventanas. Los tiempos publicados por CI corresponden a los invitados sintéticos y a sus hosts.
