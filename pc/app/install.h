#pragma once
#include <string>

// PhoneCam.exe s'installe lui-même : la DLL de caméra virtuelle est embarquée en ressource.
namespace installer {

// Dossier d'installation : C:\Program Files\PhoneCam
std::wstring InstallDir();
std::wstring InstalledExe();
std::wstring SelfPath();

// Vrai si la DLL installée manque ou diffère de celle embarquée, ou si l'exe installé est plus ancien.
bool Needed();

// Relance l'exe en administrateur avec --install / --uninstall et attend la fin. Renvoie le code de sortie.
int RunElevated(const wchar_t* argument);

// Modes « --install » et « --uninstall » (déjà en administrateur). Renvoient 0 si tout va bien.
int Install();
int Uninstall();

}  // namespace installer
