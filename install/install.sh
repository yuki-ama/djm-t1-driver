#!/bin/bash
#
# install.sh — Install DJM-T1 bridge daemon and AudioServerPlugin
#
# Usage: sudo ./install.sh
#
# Installs:
#   /usr/local/bin/djmt1-bridge
#   /Library/Audio/Plug-Ins/HAL/DJMT1AudioPlugin.driver/
#   /Library/LaunchDaemons/com.ultraxperience.djmt1-bridge.plist
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BRIDGE_BIN="$PROJECT_DIR/bridge/djmt1-bridge"
PLUGIN_BUNDLE="$PROJECT_DIR/plugin/build/DJMT1AudioPlugin.driver"
LAUNCHD_PLIST="$SCRIPT_DIR/com.ultraxperience.djmt1-bridge.plist"

INSTALL_BIN="/usr/local/bin/djmt1-bridge"
INSTALL_PLUGIN="/Library/Audio/Plug-Ins/HAL/DJMT1AudioPlugin.driver"
INSTALL_PLIST="/Library/LaunchDaemons/com.ultraxperience.djmt1-bridge.plist"
SERVICE_LABEL="com.ultraxperience.djmt1-bridge"

# Check root
if [ "$EUID" -ne 0 ]; then
    echo "Error: must run as root (use sudo)"
    exit 1
fi

# Check build artifacts exist
if [ ! -f "$BRIDGE_BIN" ]; then
    echo "Error: bridge not built. Run 'make' first."
    exit 1
fi

if [ ! -d "$PLUGIN_BUNDLE" ]; then
    echo "Error: plugin not built. Run 'make' first."
    exit 1
fi

echo "=== DJM-T1 Audio Driver Installation ==="
echo ""

# Stop existing daemon if running
if launchctl print "system/$SERVICE_LABEL" &>/dev/null; then
    echo "Stopping existing bridge daemon..."
    launchctl bootout "system/$SERVICE_LABEL" 2>/dev/null || true
fi

# Install bridge binary
echo "Installing bridge daemon -> $INSTALL_BIN"
mkdir -p /usr/local/bin
cp "$BRIDGE_BIN" "$INSTALL_BIN"
chown root:wheel "$INSTALL_BIN"
chmod 755 "$INSTALL_BIN"

# Install AudioServerPlugin
echo "Installing audio plugin -> $INSTALL_PLUGIN"
mkdir -p "/Library/Audio/Plug-Ins/HAL"
rm -rf "$INSTALL_PLUGIN"
cp -R "$PLUGIN_BUNDLE" "$INSTALL_PLUGIN"
chown -R root:wheel "$INSTALL_PLUGIN"

# Install launchd plist
echo "Installing launchd plist -> $INSTALL_PLIST"
cp "$LAUNCHD_PLIST" "$INSTALL_PLIST"
chown root:wheel "$INSTALL_PLIST"
chmod 644 "$INSTALL_PLIST"

# Load daemon
echo "Loading bridge daemon..."
launchctl bootstrap system "$INSTALL_PLIST"

# Restart coreaudiod to pick up new plugin
echo "Restarting coreaudiod..."
launchctl kickstart -k system/com.apple.audio.coreaudiod

echo ""
echo "=== Installation Complete ==="
echo ""
echo "The DJM-T1 bridge daemon will start automatically when the system boots."
echo "Connect your DJM-T1 and check Audio MIDI Setup for 'PIONEER DJM-T1'."
echo ""
echo "Logs: /var/log/djmt1-bridge.log"
echo "Uninstall: sudo $SCRIPT_DIR/uninstall.sh"
