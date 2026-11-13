# Protocole PhoneCam

Le téléphone est **serveur TCP sur le port 38500**. Le PC s'y connecte :

- **Wi-Fi** : directement sur l'IP du téléphone (découverte par balise UDP, voir plus bas) ;
- **USB** : `adb forward tcp:38500 tcp:38500`, puis connexion à `127.0.0.1:38500`.

Un seul client à la fois : une nouvelle connexion remplace l'ancienne.

## Trame

Chaque message : `u8 type` + `u32 longueur` (big-endian) + `longueur` octets de charge utile.

### Téléphone → PC

| Type | Nom    | Charge utile |
|------|--------|--------------|
| `H`  | Hello  | Texte UTF-8, une ligne par élément (voir « Capacités ») |
| `F`  | Format | Texte `clé=valeur` séparés par des espaces : `codec width height fps bitrate camera facing orientation highspeed` |
| `C`  | Config codec | SPS/PPS (H.264) ou VPS/SPS/PPS (HEVC), Annex-B |
| `V`  | Vidéo  | `u8 flags` (bit 0 = image clé) + `i64 pts µs` + données Annex-B |
| `E`  | Erreur | Texte UTF-8 |

### PC → téléphone

| Type | Nom    | Charge utile |
|------|--------|--------------|
| `S`  | Start  | `camera=<id> width=<w> height=<h> fps=<f> codec=<avc|hevc> bitrate=<bps>` (relance la capture si déjà active) |
| `K`  | Keyframe | vide : demande une image clé |
| `X`  | Stop   | vide : arrête la capture sans fermer la connexion |

### Capacités (message `H`)

```
device <nom du modèle>
codec avc
codec hevc
camera <id> <back|front|external> <orientation capteur en degrés>
mode <id caméra> <largeur> <hauteur> <fps max> <0|1 haute vitesse> <codec>
```

Une ligne `mode` par couple (taille, codec) : `fps max` tient compte à la fois de la caméra et de l'encodeur matériel.

## Flux réseau pour OBS (optionnel)

Désactivé par défaut ; réactivé au démarrage seulement s'il l'était la fois précédente.
Le téléphone (bouton « OBS ») et l'application PC (« Diffuser pour OBS sur le réseau ») servent
le même flux sur le **port TCP 38510** : `GET /` renvoie `HTTP/1.0 200`, `Content-Type: video/mp2t`,
puis un **MPEG-TS** continu (un programme, PID vidéo 0x100, H.264 type 0x1B ou HEVC type 0x24,
PTS à 90 kHz, PAT/PMT et PCR devant chaque image clé). Aucun réencodage : ce sont les images du
téléphone. Chaque lecteur commence à une image clé ; un lecteur trop lent est resynchronisé.

Côté téléphone, la caméra démarre quand un lecteur OBS se connecte même sans PC, et s'arrête quand
plus personne (ni PC ni OBS) ne regarde.

## Découverte Wi-Fi

Le téléphone diffuse chaque seconde un datagramme UDP en broadcast sur le **port 38501** :

```
PHONECAM name=<modèle> port=38500
```

Le PC écoute ce port et liste les téléphones vus dans les 5 dernières secondes. Si le téléphone et le PC
sont sur des VLAN différents, le broadcast ne passe pas : saisir l'IP à la main.
