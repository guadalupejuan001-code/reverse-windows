# Éter ARM64 e interrupciones en Prime 0.3.77

## Encargo ejecutado

«Rastrear qué limita a Éter ARM64 durante el arranque, corregir una ruta que use la app y medirla antes y después con la misma carga. Comprobar que el invitado conserva sus resultados y recibe las interrupciones que tiene habilitadas».

## Ruta de la app

La sesión Android habilita la traducción ARM64, el planificador SMP y el reloj de la placa durante el arranque desde ISO. En `PcMotherboardC06::run_secondary`, un vector pendiente cuya prioridad permite la entrega conserva el límite de una instrucción. Un vector bloqueado por TPR o por una interrupción en servicio puede esperar en IRR mientras Éter ejecuta un bloque. El cambio de prioridad y el fin de interrupción vuelven a pasar por la placa. Un PIC desconectado conserva sus solicitudes pendientes sin detener los lotes paralelos de lectura.

Los procesadores secundarios también llaman a `Cpu::try_rep_batch` con el presupuesto de la placa y la misma exclusión del bus que las operaciones escalares. Éter comprueba instrucciones, permisos, límites de página y solapamientos. El diagnóstico incluye los elementos y bytes procesados en `ether_next.secondary_rep_elements` y `secondary_rep_bytes`.

Si el intérprete de vectores descarta un bucle, la vCPU ejecuta sus instrucciones y vuelve a evaluar esa dirección en su siguiente turno. Así evita decodificar repetidamente un bucle que ya ha rechazado durante el turno actual.

## Cómo comprobar el resultado

Con los componentes fijados por el proyecto y una copia de r101 en la ruta indicada, ejecutar desde la raíz del proyecto

```sh
git worktree add --detach ../prime-r101 9e6baec677e7a90f7b593e2a5651cc7623300dc9
bash tests/compare_smp_irq.sh ../prime-r101 build
```

La prueba alterna siete ejecuciones de cada versión. Un invitado x64 arranca una segunda vCPU, ejecuta aritmética con y sin una interrupción pendiente o copia 128 KiB mediante `REP MOVSB`. Para controlar el número de solicitudes, el invitado se envía una IPI al vector `0xD1`. El manejador comprueba en qué instrucción se levantó la máscara. Cada prueba verifica el estado final y la copia compara todos los bytes. Las pruebas de la placa ejercitan además el RTC por PIC e IOAPIC, incluyendo una segunda IRQ8 entre la lectura de C y el EOI de la primera.

En un diagnóstico detallado de la ISO, `rtc.irq8_delivered` cuenta las entregas asociadas a IRQ8. Cada entrada de `irq.lapic_vectors` incluye el vector, las entregas y los EOI. Las sumas `entry_to_eoi_ticks` y `entry_to_eoi_steps` cubren únicamente los intervalos cerrados por un EOI, incluidos los manejadores anidados. Los ticks pertenecen al reloj de la placa. Los pasos pertenecen a la vCPU que atendió el vector y llegan hasta antes de retirar la instrucción de EOI.
