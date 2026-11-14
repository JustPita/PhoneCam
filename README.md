<p align="center">
  <img src="docs/icon.png" width="96" alt="Icône PhoneCam">
</p>

# PhoneCam

Utiliser la caméra d'un téléphone **Android** comme **webcam Windows 11**, en **Wi-Fi** ou en **USB**,
à la résolution et à la fréquence d'images maximales que le téléphone sait produire.

La webcam « PhoneCam » apparaît dans toutes les applications (Teams, Discord, OBS, Zoom, navigateur…)
comme une vraie caméra, sans pilote noyau : elle s'appuie sur l'API de caméra virtuelle de Windows 11
(`MFCreateVirtualCamera`).

## Fonctionnalités

- **Qualité maximale** : l'application Android lit les capacités réelles de la caméra *et* de l'encodeur
  matériel, et le PC propose exactement ces modes (ex. 3840×2160 @ 30 i/s, 1920×1080 @ 120 i/s en haute vitesse).
- **H.264 ou HEVC** encodé par le téléphone, **décodé par le GPU** du PC (DXVA / D3D11).
- **Wi-Fi** avec découverte automatique sur le réseau local, ou **USB** via `adb` (latence minimale).
- **Lissage automatique** en Wi-Fi : un petit tampon absorbe la gigue du réseau pour une image fluide.
- Toutes les caméras du téléphone (avant, arrière…), miroir, rotation à 180°, débit réglable.
- Le téléphone continue de filmer **écran éteint** (service de premier plan).
- Côté PC : **un seul `.exe`** qui s'installe lui-même, se range dans la **zone de notification**
  et se désinstalle depuis *Paramètres → Applications*.

## Installation

### PC (Windows 11)

1. Téléchargez `PhoneCam.exe` (ou compilez-le, voir plus bas) et lancez-le.
2. Au premier lancement, il propose d'installer la caméra virtuelle : acceptez la demande
   administrateur (une seule fois). Il se copie dans `C:\Program Files\PhoneCam`, ajoute un raccourci
   au menu Démarrer et ouvre le port UDP 38501 (découverte Wi-Fi, réseaux privés uniquement).

Pour le **HEVC**, installez « Extensions vidéo HEVC » depuis le Microsoft Store ; sinon H.264 est utilisé.

### Téléphone (Android 10 ou plus)

Installez `PhoneCam.apk` (ou compilez-le), ouvrez l'application et autorisez la caméra.

## Utilisation

1. Ouvrez **PhoneCam** sur le téléphone : il attend le PC.
2. Ouvrez **PhoneCam** sur le PC, choisissez le téléphone :
   - **Wi-Fi** : il apparaît tout seul dans la liste s'il est sur le même réseau ;
     sinon (VLAN différent, par exemple) saisissez son IP, affichée sur le téléphone ;
   - **USB** : branchez-le avec le *débogage USB* activé et choisissez « USB (adb) ».
3. Cliquez sur **Connecter** : la diffusion démarre dans le meilleur mode, modifiable ensuite.
4. Dans votre application de visio, choisissez la caméra **PhoneCam**.

Fermer ou réduire la fenêtre la range dans la zone de notification : la webcam reste disponible.
Clic droit sur l'icône → *Quitter* pour l'arrêter.

### Flux pour OBS sur le réseau (optionnel)

Pour utiliser le téléphone dans OBS sur **un autre PC** (ou sans l'application PC) :

- **depuis le téléphone** : touchez **OBS** en haut de l'écran ; l'adresse s'affiche (`http://IP-du-téléphone:38510/`) ;
- **depuis le PC** : *Réglages avancés* → **Diffuser pour OBS sur le réseau** ; l'adresse s'affiche à côté.

Dans OBS : *Sources* → **Source média**, décochez *Fichier local*, collez l'adresse dans *Entrée*.
L'option reste désactivée tant que vous ne l'activez pas, puis est mémorisée.

## Architecture

```
Téléphone (Kotlin)                          PC (C++)
┌──────────────────────────┐   TCP 38500   ┌──────────────────────────────┐
│ Camera2 → MediaCodec     │──────────────▶│ PhoneCam.exe                 │
│ (H.264/HEVC matériel)    │  Wi-Fi / USB  │ réception → décodage DXVA    │
│ serveur TCP + balise UDP │               │ → lissage → mémoire partagée │
└──────────────────────────┘               └──────────────┬───────────────┘
                                                          │ Global\PhoneCamFrames
                                           ┌──────────────▼───────────────┐
                                           │ PhoneCamSource.dll           │
                                           │ source Media Foundation      │
                                           │ chargée par Frame Server     │
                                           └──────────────┬───────────────┘
                                                          ▼
                                             Teams, OBS, Discord, Chrome…
```

| Dossier | Contenu |
|---|---|
| `android/` | Application Android : capacités caméra/encodeur, capture Camera2, encodage, serveur |
| `pc/vcamsource/` | DLL de caméra virtuelle (source Media Foundation : `IMFMediaSourceEx`, `IMFMediaStream2`) |
| `pc/app/` | Application PC : réseau, décodage, lissage, interface, installation |
| `pc/common/` | Format de la mémoire partagée entre l'application et la DLL |
| `pc/tools/` | `camtest` (lit la webcam comme une application et mesure la régularité), génération de l'icône |
| `PROTOCOL.md` | Protocole entre le téléphone et le PC |

## Compilation

### Android

JDK 17 et SDK Android (plateforme 35) :

```bash
cd android
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### PC

Visual Studio Build Tools (outils C++ x64, SDK Windows 11) et CMake :

```bash
pc\build.cmd
```

Produit `pc\build\PhoneCam.exe` (avec la DLL embarquée) et `pc\build\camtest.exe`.

## Signature

### APK

La clé de signature n'est jamais dans le dépôt. Gradle la cherche dans les variables d'environnement
`PHONECAM_KEYSTORE`, `PHONECAM_KEYSTORE_PASSWORD`, `PHONECAM_KEY_ALIAS`, `PHONECAM_KEY_PASSWORD`,
puis dans `android/keystore.properties` ou `~/.phonecam-signing/keystore.properties` :

```properties
storeFile=C:/chemin/phonecam-release.jks
storePassword=...
keyAlias=phonecam
keyPassword=...
```

Sans clé, l'APK release est signé avec la clé de debug. Le workflow **Android** compile l'APK signé
grâce aux secrets du dépôt (`PHONECAM_KEYSTORE_B64` = keystore en base64, et les trois autres), et
le joint automatiquement à chaque release publiée.

### Exe

`pc\tools\sign.cmd` signe la DLL puis l'exe à chaque compilation, dès qu'un certificat de
**signature de code** est configuré (un certificat de site web/TLS ne convient pas) :

- certificat dans le magasin Windows (Certum SimplySign, jeton USB…) : `set PHONECAM_SIGN_THUMBPRINT=<empreinte SHA-1>` ;
- ou fichier PFX : `set PHONECAM_SIGN_PFX=...` et `set PHONECAM_SIGN_PFX_PASSWORD=...`.

Sans certificat, la compilation fonctionne et l'exe n'est pas signé.

Le workflow **Windows** compile l'exe sur GitHub Actions et le joint à chaque release. Une fois
SignPath configuré (secret `SIGNPATH_API_TOKEN`, variable `SIGNPATH_ORGANIZATION_ID`), il fait
signer la DLL de caméra virtuelle, recompile l'exe avec la DLL signée, puis fait signer l'exe.
Les configurations d'artefacts SignPath sont dans `.signpath/artifact-configurations/`.

## Code signing policy

*Politique de signature de code.*

Free code signing provided by [SignPath.io](https://signpath.io), certificate by
[SignPath Foundation](https://signpath.org).

- **Committers and reviewers** (auteurs et relecture) : [JustPita](https://github.com/JustPita)
- **Approvers** (approbation de chaque signature) : [JustPita](https://github.com/JustPita)
- **Build** : les binaires signés sont compilés uniquement depuis ce dépôt public par GitHub Actions
  (workflow `Windows`), sans intervention manuelle ; chaque signature est approuvée manuellement.
- **Privacy policy** : This program will not transfer any information to other networked systems
  unless specifically requested by the user or the person installing or operating it.
  *PhoneCam ne transmet rien hors de votre réseau local : la vidéo va du téléphone au PC (et à OBS
  si vous activez le flux réseau) ; aucun compte, aucune télémétrie, aucun serveur externe.*

## Diagnostic

- La fenêtre PhoneCam affiche en continu le débit, les images reçues et décodées, et la régularité
  (écart maximal, retards réseau, images perdues côté téléphone, temps de décodage, lissage).
- `camtest.exe [images]` ouvre la webcam comme une application et indique la cadence obtenue.
- La DLL écrit ses messages avec `OutputDebugString` (visibles dans DebugView, *Capture Global Win32*).

## Limites connues

- Windows 11 obligatoire (les caméras virtuelles sont apparues avec Windows 11).
- Les fréquences d'images disponibles dépendent de ce que le constructeur ouvre aux applications tierces :
  certains téléphones réservent par exemple le 4K à 60 i/s à leur propre application caméra.
- La découverte Wi-Fi utilise le broadcast : elle ne traverse pas les VLAN ni les routeurs (saisir l'IP).
- Pas de micro : seule la vidéo est transmise.

## Licence

[MIT](LICENSE)
