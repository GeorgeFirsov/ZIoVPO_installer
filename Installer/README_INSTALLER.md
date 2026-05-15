# Сборка установщика

1. Установить Inno Setup 6.
2. При необходимости положить Microsoft Visual C++ Redistributable в файл:

```text
Installer\redist\VC_redist.x64.exe
```

3. Открыть Developer PowerShell for Visual Studio в корне проекта.
4. Выполнить:

```powershell
powershell -ExecutionPolicy Bypass -File .\Installer\build_installer.ps1
```

Готовый установщик появится здесь:

```text
Installer\output\ZIoVPO_Setup.exe
```

## Что делает установщик

- копирует файлы клиентского приложения;
- копирует файлы Windows-службы;
- копирует `signature_public.pem`;
- копирует `DefaultAvDb\manifest.bin` и `DefaultAvDb\data.bin`;
- при наличии `Installer\redist\VC_redist.x64.exe` устанавливает VC++ Runtime;
- регистрирует службу `ZIoVPO_service` с ручным запуском;
- не запускает службу после установки: служба стартует позже при запуске приложения;
- при удалении останавливает и удаляет службу;
- удаляет установленную папку приложения и `C:\ProgramData\ZIoVPO`.

## Важно про запуск службы

Инсталлятор регистрирует службу `ZIoVPO_service` с типом запуска `demand` (ручной запуск).
После установки и после перезагрузки Windows служба не стартует сама.
Запуск службы выполняется клиентским приложением, когда пользователь открывает `Application.exe`.
