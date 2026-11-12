# REVERSE Windows

Emulador de PC x86/x64 para arrancar Windows 10 como sistema invitado real.

**Estado:** reconstrucción técnica iniciada el 12 de noviembre de 2026. No se afirma que Windows 10 ya arranque ni que exista un escritorio compatible.

Reglas del proyecto:

- Sin interfaces que simulen un escritorio Windows o produzcan imágenes de arranque falsas.
- Sin copiar APK, versiones o implementaciones antiguas de laboratorios anteriores.
- El núcleo de CPU debe ejecutar instrucciones x86/x64 auténticas, con errores explícitos para operaciones no soportadas.
- Dependencias actuales fijadas por SHA y verificadas en CI; los módulos experimentales permanecen identificados como tales.
- Cada avance se demuestra con pruebas del huésped real y registros reproducibles; compilar en un host Windows no equivale a arrancar Windows como invitado.
- Nunca publicar ni distribuir imágenes ISO privadas o con derechos de terceros. Las ISO del usuario permanecen en su dispositivo.

Los cambios de arquitectura y el primer ejecutor de pruebas se desarrollan en ramas y PR independientes antes de su integración a `main`.
