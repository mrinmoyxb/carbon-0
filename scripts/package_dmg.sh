#!/bin/bash
set -e

echo "Starting macOS DMG Packaging..."

# 1. Compile binaries
echo "Compiling binaries..."
make clean
make

# 2. Setup App Bundle directories
APP_DIR="build/watt.app"
echo "Creating App Bundle structure at $APP_DIR..."
rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/Contents/MacOS"
mkdir -p "$APP_DIR/Contents/Resources"

# 3. Copy binaries and assets
echo "Copying executables and web assets..."
cp build/watt "$APP_DIR/Contents/MacOS/"
cp build/token_proxy "$APP_DIR/Contents/MacOS/"
cp -R ui "$APP_DIR/Contents/Resources/"

# 4. Create Info.plist
echo "Generating Info.plist..."
cat << 'EOF' > "$APP_DIR/Contents/Info.plist"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>English</string>
    <key>CFBundleExecutable</key>
    <string>watt</string>
    <key>CFBundleIdentifier</key>
    <string>com.carb0n.watt</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>watt</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>0.2.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>10.13</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF

# 5. Create DMG file
STAGE_DIR="build/dmg_stage"
echo "Preparing DMG staging area..."
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"
cp -R "$APP_DIR" "$STAGE_DIR/"
ln -s /Applications "$STAGE_DIR/Applications"

echo "Creating Disk Image (DMG)..."
hdiutil create -volname "carb-0n" -srcfolder "$STAGE_DIR" -ov -format UDZO build/carb-0n.dmg

# 6. Cleanup stage
rm -rf "$STAGE_DIR"
echo "DMG successfully created at build/carb-0n.dmg!"
