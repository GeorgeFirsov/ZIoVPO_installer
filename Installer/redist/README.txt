Place dependency installers here before building the installer.
Recommended file name for Microsoft Visual C++ Runtime:
VC_redist.x64.exe

If this file exists, Installer\build_installer.ps1 will copy it into Installer\payload\redist,
and installer.iss will add it to ZIoVPO_Setup.exe and run it silently during installation.
