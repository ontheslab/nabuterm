@echo off

SET Z88DK_DIR=c:\z88dk\
SET ZCCCFG=%Z88DK_DIR%lib\config\
SET PATH=%Z88DK_DIR%bin;%PATH%

echo.
echo ****************************************************************************
echo  Building NABU BBS Terminal
echo ****************************************************************************

zcc +nabu -vn --list -m -create-app -compiler=sdcc -O3 --opt-code-speed nterm.c -o NABUTERM

echo ****************************************************************************
echo  Done.  Output: NABUTERM.nabu
echo ****************************************************************************

pause
