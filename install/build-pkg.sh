#!/bin/bash
#
# build-pkg.sh — Build macOS installer package (.pkg) for DJM-T1 audio driver
#
# Usage: ./build-pkg.sh [--sign "Developer ID Installer: ..."] [--notarize]
#
# Output: install/DJMT1AudioDriver.pkg
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/pkg-build"
PKG_VERSION="1.0.0"
PKG_ID="com.ultraxperience.djmt1"
PKG_OUTPUT="$SCRIPT_DIR/DJMT1AudioDriver.pkg"
SIGN_IDENTITY=""
DO_NOTARIZE=false
SERVICE_LABEL="com.ultraxperience.djmt1-bridge"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --sign)
            SIGN_IDENTITY="$2"
            shift 2
            ;;
        --notarize)
            DO_NOTARIZE=true
            shift
            ;;
        *)
            echo "Usage: $0 [--sign \"Developer ID Installer: ...\"] [--notarize]"
            exit 1
            ;;
    esac
done

echo "=== Building DJM-T1 Installer Package ==="
echo "Version: $PKG_VERSION"

# Clean
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/payload" "$BUILD_DIR/scripts"

# ── Step 1: Build components ──
echo ""
echo "Building bridge..."
make -C "$PROJECT_DIR/bridge" clean all

echo ""
echo "Building plugin..."
make -C "$PROJECT_DIR/plugin" clean all

# ── Step 2: Create payload ──
echo ""
echo "Creating payload..."

# Bridge binary
mkdir -p "$BUILD_DIR/payload/usr/local/bin"
cp "$PROJECT_DIR/bridge/djmt1-bridge" "$BUILD_DIR/payload/usr/local/bin/"
chmod 755 "$BUILD_DIR/payload/usr/local/bin/djmt1-bridge"

# Audio plugin
mkdir -p "$BUILD_DIR/payload/Library/Audio/Plug-Ins/HAL"
cp -R "$PROJECT_DIR/plugin/build/DJMT1AudioPlugin.driver" \
    "$BUILD_DIR/payload/Library/Audio/Plug-Ins/HAL/"

# LaunchDaemon plist
mkdir -p "$BUILD_DIR/payload/Library/LaunchDaemons"
cp "$SCRIPT_DIR/com.ultraxperience.djmt1-bridge.plist" \
    "$BUILD_DIR/payload/Library/LaunchDaemons/"

# ── Step 3: Create installer scripts ──
cat > "$BUILD_DIR/scripts/preinstall" << 'SCRIPT'
#!/bin/bash
# Stop existing daemon
if launchctl print system/com.ultraxperience.djmt1-bridge &>/dev/null; then
    launchctl bootout system/com.ultraxperience.djmt1-bridge 2>/dev/null || true
fi
pkill -f djmt1-bridge 2>/dev/null || true
exit 0
SCRIPT
chmod 755 "$BUILD_DIR/scripts/preinstall"

cat > "$BUILD_DIR/scripts/postinstall" << 'SCRIPT'
#!/bin/bash
# Set ownership
chown root:wheel /usr/local/bin/djmt1-bridge
chown -R root:wheel "/Library/Audio/Plug-Ins/HAL/DJMT1AudioPlugin.driver"
chown root:wheel /Library/LaunchDaemons/com.ultraxperience.djmt1-bridge.plist
# Load daemon
launchctl bootstrap system /Library/LaunchDaemons/com.ultraxperience.djmt1-bridge.plist
# Restart coreaudiod to load the audio plugin
launchctl kickstart -k system/com.apple.audio.coreaudiod
exit 0
SCRIPT
chmod 755 "$BUILD_DIR/scripts/postinstall"

# ── Step 4: Build .pkg ──
echo ""
echo "Building package..."

pkgbuild \
    --root "$BUILD_DIR/payload" \
    --scripts "$BUILD_DIR/scripts" \
    --identifier "$PKG_ID" \
    --version "$PKG_VERSION" \
    --install-location "/" \
    "$PKG_OUTPUT"

# ── Step 5: Sign (optional) ──
if [ -n "$SIGN_IDENTITY" ]; then
    echo ""
    echo "Signing package..."
    SIGNED_PKG="${PKG_OUTPUT%.pkg}-signed.pkg"
    productsign --sign "$SIGN_IDENTITY" "$PKG_OUTPUT" "$SIGNED_PKG"
    mv "$SIGNED_PKG" "$PKG_OUTPUT"
    echo "Package signed with: $SIGN_IDENTITY"
fi

# ── Step 6: Notarize (optional) ──
if [ "$DO_NOTARIZE" = true ]; then
    if [ -z "$SIGN_IDENTITY" ]; then
        echo "Error: --notarize requires --sign"
        exit 1
    fi

    echo ""
    echo "Submitting for notarization..."
    xcrun notarytool submit "$PKG_OUTPUT" \
        --keychain-profile "notarytool-profile" \
        --wait

    echo "Stapling notarization ticket..."
    xcrun stapler staple "$PKG_OUTPUT"

    echo "Notarization complete."
fi

# Clean build artifacts
rm -rf "$BUILD_DIR"

echo ""
echo "=== Package Built ==="
echo "Output: $PKG_OUTPUT"
echo ""
echo "Install with: sudo installer -pkg $PKG_OUTPUT -target /"
echo ""
if [ -z "$SIGN_IDENTITY" ]; then
    echo "Note: Package is unsigned. For distribution, use:"
    echo "  $0 --sign \"Developer ID Installer: Ultra Xperience Inc. (4GAG263K82)\""
fi
