# Architecture

Android captures with Camera2 and encodes H.264 with MediaCodec. Windows receives Annex-B access units over USB/ADB and distributes them to independent decoder and recorder consumers.

Preview uses Media Foundation, D3D11/NV12 surfaces, and a D3D11 renderer. OpenH264 provides a software fallback. OBS and the virtual camera consume decoded frames. Recording preserves the original H.264 bitstream using FFmpeg and fragmented MP4; optional Windows audio comes from WASAPI and is encoded as AAC-LC.

Control uses JSON Lines on local port 27183; video uses port 27184. ADB forwards these ports to Android sockets bound to loopback. The application does not expose these services to the local network.

The webcam is registered as StratoCam through Media Foundation for all Windows users. The elevated installer configures its COM registration, shared frame file, and permissions. Uninstall removes this integration. Historical internal identifiers remain for compatibility and are not product display names.

Exposure, focus, white balance, and zoom capabilities are queried from Android. The current camera selection uses camera ID 0. Available resolutions and controls depend on device capabilities.
