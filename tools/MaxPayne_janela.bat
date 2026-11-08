@echo off
rem Abre o Max Payne em modo janela (-window).
rem Necessario para o ESP: o overlay nao aparece por cima de tela cheia exclusiva.
rem
rem Uso:  MaxPayne_janela.bat ["C:\caminho\para\Max Payne"]
rem Sem argumento, procura na pasta deste .bat e nos locais de instalacao mais comuns.
setlocal
set "MP_DIR=%~1"
if not defined MP_DIR if exist "%~dp0MaxPayne.exe" set "MP_DIR=%~dp0"
if not defined MP_DIR if exist "C:\Program Files (x86)\Steam\steamapps\common\Max Payne\MaxPayne.exe" set "MP_DIR=C:\Program Files (x86)\Steam\steamapps\common\Max Payne"
if not defined MP_DIR if exist "C:\GOG Games\Max Payne\MaxPayne.exe" set "MP_DIR=C:\GOG Games\Max Payne"
if not defined MP_DIR if exist "C:\Games\Max Payne\MaxPayne.exe" set "MP_DIR=C:\Games\Max Payne"

if not defined MP_DIR (
    echo MaxPayne.exe nao encontrado. Passe a pasta do jogo como argumento:
    echo   %~nx0 "C:\caminho\para\Max Payne"
    pause
    exit /b 1
)
if not exist "%MP_DIR%\MaxPayne.exe" (
    echo MaxPayne.exe nao encontrado em "%MP_DIR%".
    pause
    exit /b 1
)
start "" /D "%MP_DIR%" "%MP_DIR%\MaxPayne.exe" -window
