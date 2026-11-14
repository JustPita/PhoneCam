@echo off
rem Signature Authenticode d'un fichier (exe ou dll), appelée après chaque compilation.
rem Sans certificat configuré, ne fait rien : la compilation fonctionne toujours.
rem
rem Certificat de SIGNATURE DE CODE obligatoire (pas un certificat de site web / TLS) :
rem   - dans le magasin Windows (Certum SimplySign, jeton USB, SignPath…) :
rem       set PHONECAM_SIGN_THUMBPRINT=<empreinte SHA-1 du certificat>
rem   - ou fichier PFX :
rem       set PHONECAM_SIGN_PFX=C:\chemin\certificat.pfx
rem       set PHONECAM_SIGN_PFX_PASSWORD=<mot de passe>
rem Horodatage (la signature reste valable après expiration du certificat) :
rem   PHONECAM_SIGN_TIMESTAMP, par défaut http://timestamp.digicert.com
setlocal
set "FILE=%~1"
if "%FILE%"=="" exit /b 0
if not defined PHONECAM_SIGN_THUMBPRINT if not defined PHONECAM_SIGN_PFX (
  echo [signature] aucun certificat configure : %~nx1 non signe
  exit /b 0
)
if not defined PHONECAM_SIGN_TIMESTAMP set "PHONECAM_SIGN_TIMESTAMP=http://timestamp.digicert.com"

rem signtool du SDK Windows le plus récent
set "SIGNTOOL="
for /f "delims=" %%d in ('dir /b /ad /o-n "%ProgramFiles(x86)%\Windows Kits\10\bin\10.*" 2^>nul') do (
  if not defined SIGNTOOL if exist "%ProgramFiles(x86)%\Windows Kits\10\bin\%%d\x64\signtool.exe" set "SIGNTOOL=%ProgramFiles(x86)%\Windows Kits\10\bin\%%d\x64\signtool.exe"
)
if not defined SIGNTOOL (
  echo [signature] signtool introuvable ^(SDK Windows^)
  exit /b 1
)

if defined PHONECAM_SIGN_PFX (
  "%SIGNTOOL%" sign /fd SHA256 /td SHA256 /tr "%PHONECAM_SIGN_TIMESTAMP%" /d "PhoneCam" /f "%PHONECAM_SIGN_PFX%" /p "%PHONECAM_SIGN_PFX_PASSWORD%" "%FILE%" || exit /b 1
) else (
  "%SIGNTOOL%" sign /fd SHA256 /td SHA256 /tr "%PHONECAM_SIGN_TIMESTAMP%" /d "PhoneCam" /sha1 %PHONECAM_SIGN_THUMBPRINT% "%FILE%" || exit /b 1
)
echo [signature] %~nx1 signe
