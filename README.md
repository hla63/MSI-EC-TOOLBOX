# MSI-EC-TOOLBOX

Hackintosh kext + menu bar pour **MSI Modern 15 A10M** (1551EMS1 / CONF_G1_5).  
Testé : macOS 14 Sonoma — Lilu 1.7.x, VirtualSMC 1.3.x.

---

## Structure du projet

Les **kexts debug** de Lilu et VirtualSMC ainsi que MacKernelSDK sont a placer **directement dans `MSI-EC-TOOLBOX/`**, au même niveau que les sous-projets.

- `Lilu.kext` et `VirtualSMC.kext` : télécharger le zip **DEBUG** depuis les Releases GitHub.`).
- `MacKernelSDK`

```
MSI-EC-TOOLBOX/
├── Lilu.kext               ← zip DEBUG Lilu (bundle, headers dedans)
├── VirtualSMC.kext         ← zip DEBUG VirtualSMC (bundle, headers dedans)
├── MacKernelSDK/           ← git clone acidanthera/MacKernelSDK
├── MSIECToolbox/           ← Kext principal (Lilu plugin + IOKit UserClient)
│   ├── MSIECToolbox.xcodeproj/
│   └── Sources/
│       ├── MSIECToolbox.h / .cpp          Plugin Lilu, hook EC, fallback raw I/O
│       ├── MSIECToolboxDriver.h / .cpp    IOService minimal (provider UserClient)
│       ├── MSIECToolboxUserClient.h / .cpp Dispatch table IOKit (10 sélecteurs)
│       ├── MSIECToolboxShared.h           Registres EC, structs, constantes partagées
│       └── Info.plist
├── SMCMSIFan/              ← Plugin VirtualSMC (RPM fan → clés SMC)
│   ├── SMCMSIFan.xcodeproj/
│   └── Sources/
│       ├── SMCMSIFan.h / .cpp             Polling EC 0xCC–0xCD → F0Ac / FNum
│       ├── SMCMSIFanKeys.h                Clés SMC (F0Ac, F0Mn, F0Mx, FNum)
│       └── Info.plist
├── LaunchAgent/            ← App barre de menu (Swift, macOS userspace)
│   ├── MSIECToolboxAgent.swift
│   └── com.msi.MSIECToolboxAgent.plist    LaunchAgent plist (démarrage session)
└── ACPI/
    └── SSDT-MSI-KEY_FIX.dsl              Remapping touches PS2 → ADB (VoodooPS2)
```

---

## Dépendances

| Dépendance | Source | Utilisation |
|---|---|---|
| [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) | `git clone` | Headers kernel IOKit/KPI pour macOS 11+ |
| [Lilu **DEBUG**](https://github.com/acidanthera/Lilu/releases) | zip Release → `Lilu.kext` | Headers dans `Contents/Resources/Headers/` |
| [VirtualSMC **DEBUG**](https://github.com/acidanthera/VirtualSMC/releases) | zip Release → `VirtualSMC.kext` | Headers dans `Contents/Resources/Headers/VirtualSMCSDK/` |
| [VoodooPS2Controller](https://github.com/acidanthera/VoodooPS2) | — | SSDT remapping touches |
| Xcode | 15.0+ | Compilation kext + LaunchAgent |
| [iASL / iasl](https://acpica.org/downloads) | — | Compilation SSDT `.dsl` → `.aml` |


---

## Dépendances

| Dépendance | Version min | Utilisation |
|---|---|---|
| [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) | — | Headers kernel IOKit/KPI pour macOS 11+ |
| [Lilu](https://github.com/acidanthera/Lilu) | 1.6.0 | Base plugin kext + kext debug |
| [VirtualSMC](https://github.com/acidanthera/VirtualSMC) | 1.3.0 | Plugin SMC + kext debug (SMCMSIFan) |
| [VoodooPS2Controller](https://github.com/acidanthera/VoodooPS2) | — | SSDT remapping touches |
| Xcode | 15.0+ | Compilation kext + LaunchAgent |
| [iASL / iasl](https://acpica.org/downloads) | — | Compilation SSDT `.dsl` → `.aml` |

### Installation des dépendances

```bash
cd MSI-EC-TOOLBOX

# MacKernelSDK — toujours via git clone
git clone https://github.com/acidanthera/MacKernelSDK

# Lilu — télécharger le zip DEBUG depuis les Releases GitHub
# https://github.com/acidanthera/Lilu/releases → Lilu-X.X.X-DEBUG.zip
# Extraire dans MSI-EC-TOOLBOX/ → crée Headers/ et Lilu.kext/

# VirtualSMC — télécharger le zip DEBUG depuis les Releases GitHub
# https://github.com/acidanthera/VirtualSMC/releases → VirtualSMC-X.X.X-DEBUG.zip
# Extraire dans MSI-EC-TOOLBOX/ → crée VirtualSMCSDK/ et VirtualSMC.kext/
```

## Compilation des kext

### MSIECToolbox.kext

```bash
# Ouvrir le projet
open MSIECToolbox/MSIECToolbox.xcodeproj

# Ou compiler en ligne de commande (Release)
xcodebuild -project MSIECToolbox/MSIECToolbox.xcodeproj \
           -target MSIECToolbox \
           -configuration Release \
           build
# Résultat : build/Release/MSIECToolbox.kext
```

### SMCMSIFan.kext

```bash
open SMCMSIFan/SMCMSIFan.xcodeproj

xcodebuild -project SMCMSIFan/SMCMSIFan.xcodeproj \
           -target SMCMSIFan \
           -configuration Release \
           build
# Résultat : build/Release/SMCMSIFan.kext
```

### Ordre de chargement dans OpenCore `config.plist`

```
Lilu.kext
VirtualSMC.kext
MSIECToolbox.kext
SMCMSIFan.kext       ← dépend de VirtualSMC
```

---

## Compilation et installation du LaunchAgent

### 1. Compiler

```bash
cd LaunchAgent

swiftc MSIECToolboxAgent.swift \
  -o MSIECToolboxAgent \
  -framework Foundation \
  -framework AppKit \
  -framework CoreAudio \
  -framework IOKit \
  -framework CoreGraphics \
  -framework UserNotifications \
  -O
```

**Note :** Xcode n'est pas nécessaire pour compiler le LaunchAgent — `swiftc` seul suffit.  
L'app utilise `.accessory` (pas d'icône dans le Dock) et tourne en arrière-plan.


### 2. Installer (dans LaunchAgent)

```bash
chmod +x build_and_install.sh
sudo ./build_and_install.sh
```

### Autorisation Accessibilité (obligatoire pour CGEventTap)

Le LaunchAgent intercepte les touches via `CGEvent.tapCreate`. macOS requiert une autorisation explicite :

```
Réglages Système › Confidentialité et sécurité › Accessibilité
→ Ajouter MSIECToolboxAgent ✅
```

Sans cette autorisation, l'interception clavier est inactive mais les fonctions menu bar (fan mode, shift, cooler boost) restent fonctionnelles.

---

## Registres EC — MSI Modern 15 A10M (1551EMS1 / CONF_G1_5)

Tous ces registres sont accessibles via raw port I/O ACPI : commande `0x66`, données `0x62`.  
Source : msi-ec BeardOverflow (CONF_G1_5) + dump EC RW-Everything personnel.

### Modes et performance

| Registre | Valeurs | Description |
|---|---|---|
| `0xF4` | `0x0D`=auto / `0x1D`=silent / `0x8D`=advanced | Mode fan |
| `0x98` bit 7 | `0x80`=ON / `0x00`=OFF (masque bit 7) | Cooler Boost (fans 100%) |

> **Cooler Boost :** toujours lire `0x98`, masquer le bit 7, réécrire. Les autres bits
> contiennent des valeurs firmware persistantes (`0x02`/`0x03`/`0x05` observés dans les dumps).

### Lecture temps réel

| Registre | Contenu | Remarques |
|---|---|---|
| `0x68` | Température CPU (°C direct) | |
| `0x71` | Vitesse fan CPU (%, 0–150) | 0 = fan arrêté (< 50°C) |
| `0x80` | Température GPU (°C direct) | 0 si iGPU inactif |
| `0x89` | Vitesse fan GPU (%) | Valeur résiduelle sur A10M (iGPU seul), non fiable |
| `0xCC–0xCD` | RPM fan CPU big-endian | Formule ISW ci-dessous |
| `0xCA–0xCB` | RPM fan GPU big-endian | 0 sur A10M |

**Formule RPM (ISW)** :
```
val = (0xCC << 8) | 0xCD
si val == 0 : fan arrêté
RPM = ((325 - val) * 16) + 1480
```

### Courbe fan CPU — mode Advanced (0x6A–0x78)

La courbe n'est active que si `0xF4 = 0x8D` (mode advanced).  
Écriture atomique obligatoire : tous les 13 registres ou aucun.

| Registres | Rôle | Valeurs firmware par défaut |
|---|---|---|
| `0x6A`–`0x6F` | Seuils température (6 points) | 50, 58, 65, 70, 90, 95 °C |
| `0x72`–`0x77` | Vitesses fan (6 points) | 0, 58, 65, 72, 80, 85 % |
| `0x78` | Point 6 — 100% fixe | 0x64 (ne pas modifier) |

**Contraintes de validation avant écriture :**
- Températures strictement croissantes : `T[n] < T[n+1]`
- Vitesses croissantes ou égales : `V[n] ≤ V[n+1]`
- Plages : températures 0–100°C, vitesses 0–100%

### Courbe fan GPU (0x82–0x90)

Présente dans l'EC mais sans effet mesurable sur A10M (iGPU seul, 1 fan physique).

| Registres | Valeurs firmware |
|---|---|
| `0x82`–`0x87` (seuils temp) | 50, 60, 70, 82, 90, 93 °C |
| `0x8A`–`0x8F` (vitesses) | 45, 50, 65, 72, 80, 85 % |
| `0x90` (point fixe) | 0x64 = 100% |

### Charge batterie (0xEF)

| Valeur | Comportement |
|---|---|
| `0x64` (100) | Charge complète (défaut) |
| `0xBC` (188) | Stop à 60% — mode Super Battery MSI Center (confirmé dump) |
| `0x50` (80)  | Stop à 80% — à valider par dump avant utilisation |

> Encodage non linéaire — valider chaque seuil avec un dump EC avant d'exposer dans l'UI.

---

## SSDT-MSI-KEY_FIX.dsl

### Rôle

Ce patch ACPI hotpatch remmappe les scancodes PS2 des touches Fn du MSI Modern 15 vers des keycodes ADB que macOS comprend. Il est traité par **VoodooPS2Controller** au boot.

Sans ce SSDT, les touches Fn MSI ne génèrent pas d'événements utilisables sous macOS.

### Compilation

```bash
# Installer iASL (inclus dans MaciASL ou via brew)
brew install acpica

# Compiler le SSDT
iasl -ve SSDT-MSI-KEY_FIX.dsl
# Génère : SSDT-MSI-KEY_FIX.aml
```

### Installation

Copier `SSDT-MSI-KEY_FIX.aml` dans `EFI/OC/ACPI/` et l'ajouter dans `config.plist` :

```xml
<dict>
    <key>Path</key>
    <string>SSDT-MSI-KEY_FIX.aml</string>
    <key>Enabled</key>
    <true/>
    <key>Comment</key>
    <string>MSI Modern 15 — remapping touches Fn PS2→ADB</string>
</dict>
```

### Mapping des touches

| Scancode PS2 | Touche physique | ADB keycode | CGEventTap (agent) |
|---|---|---|---|
| `e071` | F5 mute mic | `0x4F` (ADB F14) | keycode 79 → toggle mic mute |
| `e072` | F12 rotation | `0x6F` (ADB) | keycode 111 → rotation écran 180° |
| `e06e` | F6 caméra | `0x76` (ADB) | keycode 118 → toggle caméra |
| `e077` | Volume − | `0x6B` (ADB) | géré nativement macOS |
| `e078` | Volume + | `0x71` (ADB) | géré nativement macOS |
| `e037` | Snapshot | `0x64` (PS2→PS2) | remappé vers Screenshot |

> Le LaunchAgent intercepte les keycodes 79, 111 et 118 via `CGEvent.tapCreate`
> au niveau session (`cgSessionEventTap`). L'autorisation **Accessibilité** est
> nécessaire pour que ce tap soit actif.

---

## Fonctionnement général

```
Boot OpenCore
 └─ Lilu.kext chargé
     └─ MSIECToolbox.kext chargé
         └─ pluginStart() → hook IOACPIPlatformDevice::writeECField
         └─ MSIECToolboxDriver (IOService) publié → UserClient disponible
     └─ SMCMSIFan.kext chargé
         └─ F0Ac / FNum / F0Mn / F0Mx publiés dans VirtualSMC
         └─ readCpuRPM() appelé à chaque lecture SMC

Ouverture de session
 └─ LaunchAgent démarré via com.msi.MSIECToolboxAgent.plist
     └─ IOKit UserClient connecté à MSIECToolboxDriver
     └─ CoreAudio listener → changements mute → kext → EC
     └─ CGEventTap → F5/F6/F12 → actions directes
     └─ Poll 500ms via getSystemState (sélecteur 9)
         └─ Mise à jour menu bar (temp, fan%, modes)
```

---

## Bootargs disponibles

| Bootarg | Effet |
|---|---|
| `-msiec.off` | Désactive MSIECToolbox |
| `-msiec.dbg` | Active les logs DEBUG dans Console.app |
| `-msiec.beta` | Force le chargement sur macOS non supporté |
| `-smcmsifan.off` | Désactive SMCMSIFan |
| `-smcmsifan.dbg` | Active les logs DEBUG SMCMSIFan |
