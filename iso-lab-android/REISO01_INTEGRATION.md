# REISO01: formatos y compatibilidad de arranque

**Importante:** el archivo `.riso` NO es una ISO9660 estándar y **no debe renombrarse a `.iso`**. Es un contenedor propio con bloques reconstruibles, por lo que Windows no entiende sus bytes directamente.

REVERSE Windows puede ofrecer una **unidad de CD-ROM virtual** que lee el `.riso` a través de un decodificador por sectores y expone al huésped los bytes exactos de la ISO original. El lector C++ de la rama `feature/c02-riso-direct-mount-20261112` es la primera implementación de esa idea para el ejecutor host; la conexión a JNI/Android sigue pendiente.

La APK ISO Lab v0.2 permite seleccionar una ISO normal, empaquetarla, volver a reconstruir el archivo comprimido en memoria durante la **verificación automática del hash completo** sin guardar otra ISO, y opcionalmente restaurar una `.iso` con todos sus bytes.

Nunca se garantiza reducción para toda ISO: las regiones de Windows `install.esd` o `install.wim` ya comprimidas pueden ahorrar poco o crecer. La métrica es tamaño total del contenedor, no solo bytes del payload.

La ISO inicial y la imagen de disco duro virtual se mantienen independientes; solo el disco duro es escribible durante la instalación. No se suben archivos personales ni ISO al servidor.
