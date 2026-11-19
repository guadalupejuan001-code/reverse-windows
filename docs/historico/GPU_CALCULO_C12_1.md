# Cálculo de GPU Virtual C12.1 en REVERSE Windows

La comprobación de hardware ejecuta C12.1 desde el mismo lockfile que usa la máquina virtual. Verifica una suma sobre 4.294.967.296 índices lógicos y una operación reversible de 1.024 bits. El ejecutor conserva las rutas de texto VGA y de estado gráfico de QSSD a través del commit de integración de GPU Virtual.

## Reproducir la medición

Con Mundo X y GPU Virtual en las revisiones del lockfile:

```sh
mkdir -p build
g++ -std=c++17 -O3 -Wall -Wextra -Wpedantic -Werror -pthread \
  -I deps/GPU-VIRTUAL/include -isystem deps/mundo-X/native/include \
  tests/gpu_calculo_benchmark.cpp -o build/gpu-calculo-benchmark
./build/gpu-calculo-benchmark
```

La exportación compara la reconstrucción tradicional palabra por palabra con la reutilización de un período probado. Ambas rutas restauran el mismo estado en memoria y producen 16 MiB. El programa compara todos los bytes y comprueba cada palabra con una fórmula independiente. La escritura a disco queda fuera de los tiempos.

La prueba XI compara 2.048 operandos de 1.024 bits con un recorrido de puertas, con la transformación reutilizada y con el XOR directo por palabras. Mide también la preparación de la transformación. En un conjunto separado, el runtime XI de Mundo X verifica la salida mediante 2.064 probabilidades Z deterministas. La ruta optimizada compone X y CNOT antes de aplicar la transformación. Los circuitos se ejecutan en CPU con la semántica de la base computacional.

En Linux x86-64 con GCC 14.2, las medianas de cinco pasadas fueron:

| Operación | Referencia | Ruta medida |
| --- | ---: | ---: |
| Reconstruir 16 MiB de salida periódica | 105,341 ms | 1,079 ms |
| Transformar el lote reversible | 37,832 ms, recorrido de puertas | 0,489 ms, compilación y ejecución |
| Aplicar al lote una transformación ya compilada | 0,029 ms, XOR directo | 0,053 ms, transformación reutilizada |

La suma de 4.294.967.296 valores de la carga periódica evalúa 64 valores. La suma exacta de los índices de ese dominio usa la fórmula afín y devuelve 9.223.372.034.707.292.160. Son resultados del ejecutor de software. El controlador WDDM y la ejecución en Mali desde Direct3D quedan pendientes.
