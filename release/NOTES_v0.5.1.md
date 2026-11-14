# StratoCam v0.5.1

## Descargar e instalar

1. En Windows 11 x64, ejecuta `StratoCam-Setup-v0.5.1.exe` y acepta el aviso de administrador.
2. En Android, instala `StratoCam-Android-v0.5.1.apk` y permite el acceso a la cámara.
3. Activa depuración USB, conecta el teléfono y acepta la autorización del PC.
4. Abre StratoCam en ambos dispositivos. Para usarlo como webcam, selecciona **StratoCam (Windows Virtual Camera)** en la aplicación de destino.

La webcam se instala y registra automáticamente con el instalador Windows. No se requieren comandos. OBS Studio 32.2.2 x64 es una integración opcional del instalador.

## Cambios

- Registro obligatorio de la webcam durante la instalación, con comprobación de errores.
- Nombre de dispositivo StratoCam y migración de registros anteriores.
- Reintentos de detección USB cuando el teléfono tarda en estar disponible.
- Documentación centrada en instalación y uso.

Incluye preview, controles de cámara, 1080p30/4K30 según hardware, grabación H.264 original y audio opcional de Windows AAC-LC 48 kHz.

Versión en desarrollo: permanece una incidencia visual al maximizar y la compatibilidad depende del dispositivo. El instalador Windows no tiene firma Authenticode. La APK Android está firmada.

Verifica las descargas con `SHA256SUMS.txt`. Las licencias redistribuidas están incluidas en el instalador y en el archivo adjunto de licencias.
