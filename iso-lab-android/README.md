# REVERSE ISO Lab — APK experimental Android v0.1

**Objetivo:** descomponer una ISO normal en bloques, descubrir repeticiones/relaciones simples, producir un contenedor compacto autodescriptivo `.riso` y **reconstruir los bytes originales** sin necesidad de cargar todo el fichero a memoria.

Esta aplicación es **software de laboratorio funcional de base convencional**, no una integración ya lograda con Ether o QSSD. El algoritmo es un patrón de comparación reproducible para futuros adaptadores nativos.

## Flujo en el teléfono

1. Abrir **REVERSE ISO Lab** > **Seleccionar ISO**. La selección usa el gestor de documentos de Android, no importa/copia el fichero entero a la APK.
2. **Crear archivo compacto .riso** y elegir un archivo de destino diferente. El resultado informa tamaño original, tamaño compacto, reducción efectiva, patrones, duplicados, ceros, SHA-256.
3. **Seleccionar archivo .riso** > **Reconstruir archivo .iso** y elegir ubicación. Se reconstruye por bloques y se compara SHA-256 y número exacto de bytes contra el manifiesto original. Si no coinciden, falla y trata de eliminar la salida incompleta.

El **emulador de PC no ejecuta todavía los .riso directamente**. Puede utilizar la ISO reconstruida; acceso aleatorio sobre el contenedor compacto es trabajo posterior. No se modifica la ISO original ni se garantiza que cualquier ISO se comprima.

## Formato REISO01, versión 1

Archivo autodescriptivo, solo uso local:

- Cabecera: 8 bytes ASCII `REISO01\n`, uint32 big-endian tamaño bloque fijo = 65,536.
- Registros secuenciales:
  - `1 RAW` / `2 DEFLATE`: uint32 longitud original, uint32 longitud payload, 32 bytes SHA-256 del bloque, datos; asigna ID de bloque único.
  - `3 REF`: uint32 ID del bloque único anterior (hash SHA-256 + longitud).
  - `4 ZERO`: uint32 longitud de bloque de ceros.
  - `5 PATTERN`: uint32 longitud reconstruida, uint32 longitud de patrón (1–64), bytes del patrón repetido.
  - `127 END`: uint64 bytes originales, uint64 cantidad de bloques, uint32 bloques únicos, 32 bytes SHA-256 de **toda** la ISO.
- Todos los enteros se guardan en orden de bytes de red. No hay cifrado.
- Reconstrucción secuencial con referencias a posiciones anteriores del archivo comprimido, usando lectura posicionada del archivo seleccionado. Requiere proveedor Android con acceso aleatorio para .riso. El acceso al archivo original solo requiere lectura secuencial.
- Tamaño de bloque único en RAM: 64 KiB más pequeños buffers comprimidos; una tabla de IDs/hash crece según el número de bloques únicos. El contenedor no impone una reducción predefinida.
- Duplicados se identifican con SHA-256 y longitud, lo cual ofrece una probabilidad de colisión criptográficamente despreciable, pero no es una prueba matemática de igualdad. Al reconstruir se comprueba SHA-256 completo; las pruebas locales comparan byte por byte el origen y salida.

## Limitaciones de la versión provisional

- No hace una extracción de archivos WIM ni desmonta el sistema de archivos dentro de Windows; **descompone bytes de la ISO**, sin modificarlos.
- La ISO de Windows suele contener `install.wim` o `install.esd` ya comprimido. La reducción puede ser limitada o incluso negativa en datos no comprimibles.
- Los bloques únicos también contienen una cabecera. Los patrones/duplicados pueden ahorrar mucho solo cuando la ISO tiene estructura adecuada.
- Trabajar con ficheros grandes exige tiempo y **espacio de almacenamiento real**. La compactación es sin pérdida; no es posible reducir arbitrariamente datos de alta entropía.
- La versión prototipo procesa mientras la Activity está en primer plano; si Android la mata, reanudar procesos interrumpidos no está implementado. Se puede cancelar, pero una salida parcial no es válida.
- Algunos proveedores de nube no ofrecen acceso aleatorio para reconstruir referencias; usar un fichero local del teléfono.
- No se suben ISO, claves ni archivos del usuario a GitHub Actions. No usa permisos de red.

## Compilar

Requisitos Java 17, Gradle 8.9, Android SDK 35.

```sh
gradle -p iso-lab-android --no-daemon :app:assembleDebug
```

Archivo: `iso-lab-android/app/build/outputs/apk/debug/app-debug.apk`.

Pruebas Java en host sin Android:

```sh
mkdir -p /tmp/reiso-test
javac -encoding UTF-8 -d /tmp/reiso-test \
  iso-lab-android/app/src/main/java/com/reversewindows/isolab/IsoArchive.java \
  iso-lab-android/host-tests/com/reversewindows/isolab/IsoArchiveTest.java
java -cp /tmp/reiso-test com.reversewindows.isolab.IsoArchiveTest
```

## Desarrollo futuro

Usar esta referencia medible para ensayar una verdadera representación de reglas de Ether y la deduplicación/versionado de qSSD; añadir acceso aleatorio a los bloques para montar un `.riso` como unidad óptica sin reconstruir un archivo .iso entero, siempre con comparación SHA-256 y de bytes contra el original.
