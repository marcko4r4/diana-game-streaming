@echo off
setlocal EnableExtensions
title Actualizar controlador USB/IP para DualSense (Apollo)

:: ============================================================================
:: Actualiza el controlador usbip-win2 incluido con Apollo, necesario para la
:: haptica y los gatillos adaptativos del DualSense por USB.
::
:: Uso:
::   actualizar_driver_dualsense.bat                  (detecta Apollo solo)
::   actualizar_driver_dualsense.bat "D:\ruta\Apollo" (instalacion en otra ruta)
::
:: Requiere Windows 10 o superior y permisos de administrador. Si no los tiene,
:: el script pide elevarlos.
:: ============================================================================

echo ========================================================
echo  Actualizando controlador USB/IP para DualSense
echo ========================================================
echo.

:: --- Argumentos -------------------------------------------------------------
set "ELEVATED=0"
if /i "%~1"=="--elevated" (
    set "ELEVATED=1"
    shift
)
set "APOLLO_DIR=%~1"

:: --- Permisos de administrador ---------------------------------------------
net session >nul 2>&1
if errorlevel 1 (
    if "%ELEVATED%"=="1" (
        echo [ERROR] No se obtuvieron permisos de administrador.
        echo Clic derecho en el archivo y elige "Ejecutar como administrador".
        goto :fin_error
    )
    echo Se necesitan permisos de administrador. Aceptando la ventana de Windows
    echo se volvera a ejecutar el script con esos permisos...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '--elevated' -Verb RunAs" >nul 2>&1
    if errorlevel 1 (
        echo [ERROR] No se pudo solicitar la elevacion de permisos.
        goto :fin_error
    )
    exit /b 0
)

:: --- Requisitos del sistema -------------------------------------------------
where pnputil >nul 2>&1
if errorlevel 1 (
    echo [ERROR] No se encontro pnputil. Este script requiere Windows 10 o superior.
    goto :fin_error
)

:: --- Localizar la carpeta de Apollo ----------------------------------------
if defined APOLLO_DIR if not exist "%APOLLO_DIR%\drivers\usbip-win2\usbip2_ude.inf" goto :ruta_invalida
goto :buscar_apollo

:ruta_invalida
echo [AVISO] No se encontro el controlador en la ruta indicada:
echo         %APOLLO_DIR%
set "APOLLO_DIR="

:buscar_apollo

if not defined APOLLO_DIR (
    for %%D in ("%ProgramFiles%\Apollo" "%ProgramW6432%\Apollo" "%ProgramFiles(x86)%\Apollo") do (
        if not defined APOLLO_DIR if exist "%%~D\drivers\usbip-win2\usbip2_ude.inf" set "APOLLO_DIR=%%~D"
    )
)

if not defined APOLLO_DIR (
    echo No se encontro Apollo en las rutas habituales.
    set /p "APOLLO_DIR=Escribe la carpeta de instalacion de Apollo (vacio para cancelar): "
)

if not defined APOLLO_DIR (
    echo [ERROR] Operacion cancelada. Instala Apollo o indica su carpeta.
    goto :fin_error
)

:: Quitar comillas y barra final de la ruta
set "APOLLO_DIR=%APOLLO_DIR:"=%"
if "%APOLLO_DIR:~-1%"=="\" set "APOLLO_DIR=%APOLLO_DIR:~0,-1%"

set "DRV_DIR=%APOLLO_DIR%\drivers\usbip-win2"
set "INF_FILTER=%DRV_DIR%\usbip2_filter.inf"
set "INF_UDE=%DRV_DIR%\usbip2_ude.inf"

if not exist "%INF_FILTER%" goto :falta_inf
if not exist "%INF_UDE%" goto :falta_inf
goto :inf_ok

:falta_inf
echo [ERROR] No se encontraron los archivos del controlador en:
echo         %DRV_DIR%
echo Verifica que Apollo este instalado y actualizado.
goto :fin_error

:inf_ok
echo Apollo encontrado en: %APOLLO_DIR%
echo.

:: --- Servicio de Apollo -----------------------------------------------------
set "SERVICE=ApolloService"
set "SERVICE_WAS_RUNNING=0"
powershell -NoProfile -Command "if ((Get-Service -Name '%SERVICE%' -ErrorAction SilentlyContinue).Status -eq 'Running') { exit 0 } else { exit 1 }" >nul 2>&1
if not errorlevel 1 set "SERVICE_WAS_RUNNING=1"

if "%SERVICE_WAS_RUNNING%"=="1" (
    echo 1. Deteniendo el servicio %SERVICE% temporalmente...
    powershell -NoProfile -Command "Stop-Service -Name '%SERVICE%' -Force -ErrorAction SilentlyContinue"
) else (
    echo 1. El servicio %SERVICE% no esta en ejecucion. No se modificara.
)
echo.

:: --- Instalar los controladores nuevos -------------------------------------
set "FALLO=0"

echo 2. Instalando usbip2_filter.inf...
pnputil /add-driver "%INF_FILTER%" /install
if errorlevel 1 if not errorlevel 3010 set "FALLO=1"
if "%ERRORLEVEL%"=="3010" set "REINICIO=1"
echo.

echo 3. Instalando usbip2_ude.inf...
pnputil /add-driver "%INF_UDE%" /install
if errorlevel 1 if not errorlevel 3010 set "FALLO=1"
if "%ERRORLEVEL%"=="3010" set "REINICIO=1"
echo.

:: --- Eliminar versiones antiguas -------------------------------------------
:: Solo se tocan los paquetes usbip2_ude / usbip2_filter duplicados. Se conserva
:: la version mas reciente, sea cual sea su nombre oemNN.inf en este equipo.
if "%FALLO%"=="0" (
    echo 4. Buscando versiones antiguas del controlador...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $g = Get-WindowsDriver -Online -All -ErrorAction Stop | Where-Object { $_.OriginalFileName -match 'usbip2_(ude|filter)\.inf$' } | Group-Object { [IO.Path]::GetFileName($_.OriginalFileName) }; $n = 0; foreach ($x in $g) { $x.Group | Sort-Object @{Expression={$_.Date}}, @{Expression={[version]$_.Version}} -Descending | Select-Object -Skip 1 | ForEach-Object { Write-Host ('   Eliminando ' + $_.Driver + ' (version ' + $_.Version + ')'); pnputil /delete-driver $_.Driver /uninstall /force | Out-Null; $n++ } }; if ($n -eq 0) { Write-Host '   No hay versiones antiguas que eliminar.' } } catch { Write-Host '   No se pudo consultar el almacen de controladores; se omite la limpieza.' }"
) else (
    echo 4. Se omite la limpieza de versiones antiguas porque la instalacion fallo.
)
echo.

:: --- Reiniciar el servicio si estaba activo --------------------------------
if "%SERVICE_WAS_RUNNING%"=="1" (
    echo 5. Iniciando el servicio %SERVICE%...
    powershell -NoProfile -Command "Start-Service -Name '%SERVICE%'"
    echo.
)

echo ========================================================
if "%FALLO%"=="1" (
    echo  Hubo errores durante la instalacion. Revisa los mensajes anteriores.
    echo ========================================================
    goto :fin_error
)
echo  Proceso completado.
echo  Controlador instalado actualmente:
powershell -NoProfile -Command "Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -match 'usbip2_(ude|filter)\.inf$' } | ForEach-Object { '   ' + [IO.Path]::GetFileName($_.OriginalFileName) + '  version ' + $_.Version + '  (' + $_.Driver + ')' }"
if defined REINICIO (
    echo.
    echo  Windows pide reiniciar el equipo para terminar la instalacion.
)
echo ========================================================
echo.
pause
exit /b 0

:fin_error
echo.
pause
exit /b 1
