# Compilar StratoCam

Requisitos para desarrollo: Windows, Git, PowerShell y Python 3 con el launcher `py`.

```powershell
.\scripts\bootstrap-windows-deps.ps1
.\scripts\bootstrap-android-sdk.ps1 -AcceptAndroidSdkLicenses
.\scripts\bootstrap-inno-setup.ps1
.\scripts\generate-icons.ps1
.\scripts\build-windows-app.ps1
.\scripts\build-virtual-camera.ps1
.\scripts\build-obs-plugin.ps1
.\scripts\build-android-release.ps1
.\scripts\package-release.ps1 -CleanDist
```

Las dependencias se descargan a `.tools/`, sin modificar el PATH global. `version.properties` define la versión de Windows, Android y el instalador. La configuración de firma Android es local y permanece fuera de Git: consulta [RELEASE_SIGNING.md](RELEASE_SIGNING.md).

`build-windows-app.ps1` ejecuta las pruebas nativas. `build-android-release.ps1` ejecuta las pruebas unitarias release y verifica la firma de la APK cuando existe una configuración de firma completa.

Los artefactos se generan en `dist/`, con sus licencias y `SHA256SUMS.txt`. El instalador incluye ADB, bibliotecas multimedia, APK, webcam y el módulo OBS opcional. El registro de webcam se ejecuta automáticamente durante la instalación con privilegios de administrador y se comprueba su código de salida.

Los scripts de instalación manual de componentes se conservan para desarrollo y diagnóstico; no forman parte del flujo de un usuario que descarga el instalador.

El empaquetado compila también `android-shell/src/com/stratocam/shell/DisplayPower.java` mediante el JDK y D8 locales. El DEX se instala en `Support/` y Windows lo ejecuta como shell a través de ADB para controlar el panel físico. No utiliza permisos privilegiados de la APK. La sesión conserva stdin abierto y restaura el panel al liberar ese canal. El control depende de APIs del sistema que algunos fabricantes o versiones Android pueden no exponer; en ese caso se conserva el stream con pantalla encendida.
