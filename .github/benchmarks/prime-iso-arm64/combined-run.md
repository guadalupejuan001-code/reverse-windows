# Comparación física de CPU, RAM y QSSD de Prime

El trabajo se ejecuta sobre `ubuntu-24.04-arm`. La referencia `9091290` y el candidato se compilan con sus revisiones publicadas y cada `dependencies.lock.json`. Ambos se ejecutan de forma consecutiva con afinidad a la misma CPU del mismo runner, con discos de laboratorio nuevos y la misma entrada de memoria disponible de 510 MiB y umbral de 128 MiB.

El medio común es Windows 10 Enterprise Evaluation 22H2 x64 de Microsoft con 5.550.497.792 bytes. El lector verifica cada rango HTTPS. La meta es cruzar 9.500 millones de pasos huésped retirados. El artefacto incluye la comparación de 400–700 millones, 750–1.000 millones, 8.200–8.600 millones y 8.650–9.500 millones. Los tres últimos subintervalos tienen puntos de control en 9.000, 9.250 y 9.500 millones. Las tasas usan el número de pasos efectivamente retirados entre las primeras ventanas que cruzan los puntos de control. IRQ, cambios de modo, reinicios y actividad de medios ayudan a comprobar qué fase recorrió cada versión.

Los contadores de CPU incluyen pasos y bloques Éter, datos que pasan a ejecución escalar, lotes de RAM y el contador de `ADD` a memoria cuando la revisión lo proporciona. Los contadores del diario de QSSD del disco del sistema se conservan por separado de los de RAM respaldada y su códec. El trabajo independiente `candidate-native-oracles` preserva los resultados JUnit de CPU, RAM, IRQ y QSSD.

La revisión a medir se fija en `combined-targets.json`. El campo `candidate` requiere `product_sha` y `mundo_x_sha` completos de una revisión publicada. Un cambio en ese manifiesto en la rama CI `ci/prime-r110-arm64-combined-pair` inicia la comparación sin depender de un flujo en `main`. Los artefactos son `prime-r110-arm64-matched-iso-9-5b` y `prime-r110-arm64-candidate-native-oracles`.

La ejecución candidata sigue en curso durante la publicación de `prime-r110-arm64-matched-iso-early-progress`. Los controles provisionales usan sus puntos de control hasta 1.000 millones de pasos y la referencia de la misma CPU. El artefacto final incorpora el estado de salida y la verificación completa del transporte del ISO candidato.
