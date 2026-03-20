@echo off

SET Z88DK_DIR=c:\z88dk\
SET ZCCCFG=%Z88DK_DIR%lib\config\
SET PATH=%Z88DK_DIR%bin;%PATH%

echo.
echo ****************************************************************************
echo  Building NABU BBS Terminal
echo ****************************************************************************

echo.
echo  [1/2] G2 colour build (stock TMS9918A) -^> NABUTERM.nabu
echo.
zcc +nabu -vn --list -m -create-app -compiler=sdcc -O3 --opt-code-speed nterm.c -o NABUTERM

echo.
echo  [2/2] 80-col F18A build               -^> NABUTERM80.nabu
echo.
zcc +nabu -vn --list -m -create-app -compiler=sdcc -O3 --opt-code-speed -DVDP_80COL nterm.c -o NABUTERM80

echo.
echo ****************************************************************************
echo  Done.  NABUTERM.nabu (G2 colour, stock)   NABUTERM80.nabu (F18A 80-col)
echo ****************************************************************************

pause
