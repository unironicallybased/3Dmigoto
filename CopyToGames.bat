@echo off
if "%username%" NEQ "bo3b" exit

if %2=="amd64" GOTO Copyx64

:Copyx86
@echo on
echo 
xcopy "%1*.*" "T:\SteamLibrary\SteamApps\common\Assassin's Creed 3\"  /F /Y
echo 
xcopy "%1*.*" "T:\Bootleg\Assassin's Creed IV - Black Flag\"  /F /Y
echo 
xcopy "%1*.*" "T:\SteamLibrary\SteamApps\common\BioShock Infinite\Binaries\Win32\" /F /Y
echo 
xcopy "%1*.*" "T:\Bootleg\Saints Row IV\" /F /Y
echo 
xcopy "%1*.*" "T:\SteamLibrary\SteamApps\common\Saints Row the Third\" /F /Y
echo 
xcopy "%1*.*" "T:\SteamLibrary\SteamApps\common\F.E.A.R. 3\" /F /Y
echo 
xcopy "%1*.*" "C:\Program Files (x86)\Steam\SteamApps\common\Alien Isolation\" /F /Y
echo
xcopy "%1*.*" "T:\Bootleg\Ori and the Blind Forest\" /F /Y
echo
xcopy "%1*.*" "C:\Program Files (x86)\Steam\SteamApps\common\DefenseGrid2\" /F /Y
echo
xcopy "%1*.*" "T:\Bootleg\DiRT Rally\" /F /Y

exit


:Copyx64
@echo on
echo 
xcopy "%1*.*" "T:\Games\Watch_Dogs\bin"  /F /Y
xcopy "%1*.*" "T:\SteamLibrary\SteamApps\common\Call of Duty Ghosts\"  /F /Y
xcopy "%1*.*" "T:\SteamLibrary\SteamApps\common\ShadowOfMordor\x64\" /F /Y
xcopy "%1*.*" "T:\Bootleg\Project CARS\" /F /Y
xcopy "%1*.*" "T:\Games\The Witcher 3 Wild Hunt\bin\x64\" /F /Y
xcopy "%1*.*" "T:\SteamLibrary\SteamApps\common\Metal Gear Solid Ground Zeroes\" /F /Y
