@echo off
rem Compila o trainer (x86, 32 bits - o Max Payne 1 e um processo de 32 bits).
rem Requer o Zig como compilador C. Jeito mais facil: pip install ziglang
rem Saida: bin\MaxPayneExternal.exe
setlocal
cd /d "%~dp0"
if not exist bin mkdir bin

where zig >nul 2>nul
if %errorlevel%==0 (
    set "CC=zig cc"
) else (
    set "CC=python -m ziglang cc"
)

%CC% -target x86-windows-gnu -O2 -Wall src\MaxPayneExternal.c -o bin\MaxPayneExternal.exe -lgdi32 -luser32
if errorlevel 1 (
    echo.
    echo Falha na compilacao. Instale o Zig ^(pip install ziglang^) ou ajuste o compilador neste arquivo.
    exit /b 1
)
del /q bin\*.pdb 2>nul
echo Pronto: bin\MaxPayneExternal.exe
