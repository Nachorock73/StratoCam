# Arquitectura

Android captura mediante Camera2 y codifica H.264 mediante MediaCodec. Windows recibe access units Annex-B por USB/ADB y mantiene consumidores separados para decodificación y grabación.

La preview utiliza Media Foundation, superficies D3D11/NV12 y un renderer D3D11. OpenH264 sirve como fallback. OBS y la cámara virtual reciben frames decodificados. La rama de grabación guarda el H.264 original mediante FFmpeg en MP4 fragmentado; el audio opcional proviene de WASAPI y se codifica a AAC-LC.

El control usa JSON Lines en el puerto local 27183; el video usa el 27184. ADB crea los forwards hacia los sockets Android enlazados a loopback. La aplicación no expone estos servicios a la red local.

La webcam se registra como StratoCam mediante Media Foundation para todos los usuarios. Su registro COM, archivo compartido y permisos se configuran por el instalador elevado. La desinstalación retira esa integración. Los identificadores internos históricos se mantienen por compatibilidad y no son nombres visibles del producto.

Las capacidades y límites de exposición, enfoque, WB y zoom se consultan a Android. La versión actual selecciona cameraId 0. La disponibilidad de resoluciones y controles depende del dispositivo.
