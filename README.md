# StratoCam

Usa la cámara de tu teléfono Android en Windows mediante USB. StratoCam ofrece preview en vivo, controles de cámara, grabación local y una webcam virtual que otras aplicaciones pueden seleccionar como **StratoCam (Windows Virtual Camera)**.

## Descargar

[**Descargar StratoCam para Windows y Android — v0.5.1**](https://github.com/Nachorock73/StratoCam/releases/tag/v0.5.1)

- **Windows:** `StratoCam-Setup-v0.5.1.exe`
- **Android:** `StratoCam-Android-v0.5.1.apk`
- **Integridad de las descargas:** `SHA256SUMS.txt`

No necesitas compilar el proyecto, instalar herramientas de desarrollo ni ejecutar comandos.

## Instalar y conectar

1. Ejecuta el instalador de Windows y acepta la autorización de administrador. El instalador incluye ADB, las dependencias y el registro automático de la webcam **StratoCam**. La integración con OBS es opcional.
2. En el teléfono, abre la APK descargada e instálala. Android puede pedir autorización para instalar aplicaciones desde el navegador o gestor de archivos.
3. Abre StratoCam en Android y permite el acceso a la cámara.
4. Activa **Opciones de desarrollador → Depuración USB** en Android y conecta el teléfono con un cable USB de datos.
5. Acepta en el teléfono la autorización de depuración USB del PC. Puedes marcar «Permitir siempre desde este equipo».
6. Abre StratoCam en Windows. La aplicación detecta el dispositivo autorizado e inicia el video. Si conectas el teléfono después de abrirla, reintenta la detección automáticamente.

La autorización de Windows se pide durante la instalación. Los permisos de cámara y depuración USB se aceptan en Android; son permisos del sistema que la aplicación no puede concederse sola.

## Usar como webcam

Mantén StratoCam abierto con el video activo. En la aplicación de videollamada, navegador o capturadora selecciona **StratoCam (Windows Virtual Camera)**. Windows añade automáticamente el sufijo de cámara virtual al nombre base **StratoCam**.

Si instalaste StratoCam mientras otra aplicación tenía abierta su lista de cámaras, reinicia esa aplicación para actualizar la lista. Sin video activo, la webcam entrega una imagen negra.

## Controles y grabación

- **Start / Stop:** inicia o detiene el video.
- **Exposure, Focus, White Balance y Zoom:** modos automáticos y controles manuales según lo que permita la cámara.
- **Click-to-focus:** haz clic en la imagen con enfoque automático activo. Usa **Resume continuous AF** para volver al enfoque continuo.
- **Orientación y escala:** Auto, Portrait, Landscape, rotaciones, Fit, Fill y tamaño real.
- **Record:** guarda el H.264 original del teléfono en MP4 fragmentado, sin volver a codificar el video.
- **Audio Input:** selecciona un micrófono o interfaz de audio de Windows, o **No audio**. El audio se guarda como AAC-LC a 48 kHz.
- **Browse:** elige dónde guardar. La carpeta predeterminada es `Videos\StratoCam`.
- **Audio Sync Offset:** ajuste manual opcional de sincronización en milisegundos.

Elige resolución, orientación y entrada de audio antes de comenzar a grabar. Finaliza con **Stop REC**.

## OBS

Selecciona la integración OBS durante la instalación. Reinicia OBS Studio 32.2.2 x64 y añade la fuente **StratoCam**. También puedes usar la webcam StratoCam como dispositivo de captura de video.

## Requisitos y estado actual

- Windows 11 x64, build 22000 o posterior para la cámara virtual.
- Android 6.0 o posterior con Camera2, codificador H.264 y depuración USB disponibles.
- Un teléfono conectado por USB de datos. Algunos fabricantes requieren su controlador USB para Windows.
- 1080p30 y 4K30 dependen de las capacidades de la cámara y del teléfono; los controles manuales también dependen del hardware.
- Decodificación por hardware con Media Foundation/D3D11 y fallback OpenH264.

**Versión 0.5.1, en desarrollo.** Existen limitaciones de compatibilidad y una incidencia visual pendiente al maximizar la ventana. La aplicación utiliza actualmente la cámara `0`; no incorpora todavía un selector universal de lentes. El instalador Windows no tiene firma Authenticode y Windows puede mostrar una advertencia de editor desconocido. La APK sí está firmada.

No incluye audio desde Android, HEVC ni modos de alta velocidad.

## Si algo no conecta

- Desbloquea el teléfono, comprueba depuración USB y acepta la autorización del PC.
- Usa un cable de datos y prueba otro puerto USB. En algunos teléfonos ayuda seleccionar transferencia de archivos.
- Comprueba los permisos de cámara/micrófono para aplicaciones de escritorio en Windows.
- Para reparar la webcam, vuelve a ejecutar el instalador. No hace falta ejecutar scripts de registro.
- Si un dispositivo de audio no aparece, conéctalo antes de abrir StratoCam.

## Privacidad

La conexión de cámara funciona por USB/ADB con sockets locales. No requiere cuentas, nube ni conexión Wi-Fi, y no incorpora analítica. Las grabaciones se guardan en la carpeta elegida por el usuario.

## Código fuente

Consulta [la arquitectura](docs/ARCHITECTURE.md), [la compilación desde código](docs/BUILDING.md) y [la firma Android](docs/RELEASE_SIGNING.md). Estas instrucciones son para desarrollo; las descargas ya incluyen lo necesario para usar la aplicación.

Código de la aplicación bajo [Apache-2.0](LICENSE). El módulo OBS tiene su aviso GPL-2.0-or-later. Consulta [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) para dependencias y licencias.
