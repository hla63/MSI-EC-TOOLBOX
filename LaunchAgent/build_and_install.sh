#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_PATH="/Applications/MSIECTOOLBOX.app"
SUPPORT_DIR="/Library/Application Support/MSIECToolbox"
AGENT_DST="$SUPPORT_DIR/MSIECToolboxAgent"

echo "=== MSIECToolbox v4.1.1 — Installation ==="

# 1. Compiler et installer l'agent (toujours recompilé)
echo "→ Compilation de l'agent..."
AGENT_SRC="$SCRIPT_DIR/MSIECToolboxAgent.swift"
swiftc "$AGENT_SRC" \
    -o /tmp/MSIECToolboxAgent_bin \
    -framework Foundation \
    -framework CoreAudio \
    -framework IOKit \
    -framework CoreGraphics \
    -framework AppKit \
    -O
sudo mkdir -p "$SUPPORT_DIR"
sudo cp /tmp/MSIECToolboxAgent_bin "$AGENT_DST"
sudo chmod 755 "$AGENT_DST"
sudo xattr -d com.apple.quarantine "$AGENT_DST" 2>/dev/null || true
echo "   ✅ Agent installé dans $AGENT_DST" 

# 2. Désenregistrer l'ancien LaunchAgent launchctl si présent
OLD_PLIST="$HOME/Library/LaunchAgents/com.msi.MSIECToolboxAgent.plist"
if [ -f "$OLD_PLIST" ]; then
    echo "→ Suppression ancien LaunchAgent launchctl..."
    launchctl unload "$OLD_PLIST" 2>/dev/null || true
    rm -f "$OLD_PLIST"
    echo "   ✅ Ancien plist supprimé"
fi

# 3. Compiler le binaire installeur
echo "→ Compilation de l'installeur..."
swiftc "$SCRIPT_DIR/MSIECToolboxInstaller.swift" \
    -o /tmp/MSIECToolboxInstaller_bin \
    -framework Foundation \
    -framework ServiceManagement \
    -O
echo "   ✅ Installeur compilé"

# 4. Créer le bundle app
echo "→ Création de MSIECToolbox.app..."
sudo rm -rf "$APP_PATH"
sudo mkdir -p "$APP_PATH/Contents/MacOS"
sudo mkdir -p "$APP_PATH/Contents/Library/LaunchAgents"

sudo cp /tmp/MSIECToolboxInstaller_bin "$APP_PATH/Contents/MacOS/MSIECToolboxInstaller"
sudo cp "$SCRIPT_DIR/Info.plist"     "$APP_PATH/Contents/Info.plist"

# Plist de l'agent (dans le bundle)
sudo tee "$APP_PATH/Contents/Library/LaunchAgents/com.msi.MSIECToolboxAgent.plist" > /dev/null << 'PLISTEOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
    "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.msi.MSIECToolboxAgent</string>
    <key>ProgramArguments</key>
    <array>
        <string>/Library/Application Support/MSIECToolbox/MSIECToolboxAgent</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <dict>
        <key>Crashed</key>
        <true/>
    </dict>
    <key>ThrottleInterval</key>
    <integer>30</integer>
    <key>StandardOutPath</key>
    <string>/tmp/MSIECToolboxAgent.log</string>
    <key>StandardErrorPath</key>
    <string>/tmp/MSIECToolboxAgent.err</string>
</dict>
</plist>
PLISTEOF

echo "   ✅ Bundle créé dans $APP_PATH"

# 5. Signer le bundle (ad-hoc — pas besoin de compte développeur)
echo "→ Signature du bundle..."
sudo codesign --force --deep --sign - \
    --entitlements "$SCRIPT_DIR/entitlements.plist" \
    "$APP_PATH"
echo "   ✅ Bundle signé"

# Vérifier la signature
codesign --verify --deep "$APP_PATH" && echo "   ✅ Signature valide"

# 6. Enregistrer via SMAppService
#    On ne fait unregister+register que si le binaire agent a changé
#    (le register() déclenche une notification système à chaque appel
#    si le bundle a été recréé — on évite ça en vérifiant le checksum).
AGENT_CHECKSUM_FILE="$SUPPORT_DIR/.agent_checksum"
NEW_CHECKSUM=$(shasum -a 256 "$AGENT_DST" | awk '{print $1}')
OLD_CHECKSUM=$(cat "$AGENT_CHECKSUM_FILE" 2>/dev/null || echo "")

STATUS=$("$APP_PATH/Contents/MacOS/MSIECToolboxInstaller" status 2>/dev/null || echo "")

if [ "$NEW_CHECKSUM" != "$OLD_CHECKSUM" ] || [[ "$STATUS" != *"✅"* ]]; then
    echo "→ Désenregistrement préalable (unregister)..."
    "$APP_PATH/Contents/MacOS/MSIECToolboxInstaller" unregister 2>/dev/null || true

    echo "→ Enregistrement (register)..."
    "$APP_PATH/Contents/MacOS/MSIECToolboxInstaller" register

    # Sauvegarder le checksum pour éviter les re-registrations inutiles
    echo "$NEW_CHECKSUM" | sudo tee "$AGENT_CHECKSUM_FILE" > /dev/null
else
    echo "→ Agent inchangé et déjà enregistré — pas de re-registration"
    echo "   (évite la notification 'Éléments en arrière-plan' répétée)"
fi

echo ""
echo "=== Installation terminée ==="
echo "Si le statut indique 'En attente approbation' :"
echo "→ Réglages Système > Général > Ouverture > activer MSIECToolbox"
