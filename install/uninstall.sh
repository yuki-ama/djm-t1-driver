#!/bin/bash
#
# uninstall.sh — Uninstall DJM-T1 bridge daemon and AudioServerPlugin
#
# Usage: sudo ./uninstall.sh
#

set -e

INSTALL_BIN="/usr/local/bin/djmt1-bridge"
INSTALL_PLUGIN="/Library/Audio/Plug-Ins/HAL/DJMT1AudioPlugin.driver"
INSTALL_PLIST="/Library/LaunchDaemons/com.ultraxperience.djmt1-bridge.plist"
SERVICE_LABEL="com.ultraxperience.djmt1-bridge"

# Check root
if [ "$EUID" -ne 0 ]; then
    echo "Error: must run as root (use sudo)"
    exit 1
fi

echo "=== DJM-T1 Audio Driver Uninstallation ==="
echo ""

# Stop daemon
if launchctl print "system/$SERVICE_LABEL" &>/dev/null; then
    echo "Stopping bridge daemon..."
    launchctl bootout "system/$SERVICE_LABEL" 2>/dev/null || true
fi

# Kill any remaining bridge process
pkill -f djmt1-bridge 2>/dev/null || true

# Remove files
echo "Removing bridge binary..."
rm -f "$INSTALL_BIN"

echo "Removing audio plugin..."
rm -rf "$INSTALL_PLUGIN"

echo "Removing launchd plist..."
rm -f "$INSTALL_PLIST"

# Remove log
rm -f /var/log/djmt1-bridge.log

# Restart coreaudiod
echo "Restarting coreaudiod..."
launchctl kickstart -k system/com.apple.audio.coreaudiod

echo ""
echo "=== Uninstallation Complete ==="
