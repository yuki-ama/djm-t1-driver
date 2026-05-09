# DJM-T1 macOS Audio Driver

Pioneer DJM-T1 DJ mixer用の macOS オーディオドライバー。
Pioneer（現AlphaTheta）が提供していた従来のkextドライバーが新しいmacOSで動作しなくなったため、ユーザースペースの bridge + AudioServerPlugin アーキテクチャで代替実装したもの。

## Architecture

```
[DJM-T1] <--USB isochronous--> [bridge daemon] <--shared memory--> [AudioServerPlugin] <--Core Audio HAL--> [DAW / DJ Software]
```

- **bridge** — libusb経由でDJM-T1とUSBアイソクロナス転送を行うデーモン（root権限で動作）
- **plugin** — Core Audio HAL AudioServerPlugin。共有メモリ経由でbridgeとオーディオデータを交換
- 6ch入出力、48kHz、24bit

## Requirements

- macOS 13.0 (Ventura) 以降
- Apple Silicon または Intel Mac
- Xcode Command Line Tools
- [Homebrew](https://brew.sh)

## Build & Install

```bash
# Dependencies
brew install libusb pkg-config

# Clone
git clone --recursive https://github.com/yuki-ama/djm-t1-driver.git
cd djm-t1-driver

# Build
make

# Install (requires root)
sudo make install
```

## Verify

1. DJM-T1をUSB接続
2. **Audio MIDI Setup** を開く
3. 「PIONEER DJM-T1」が表示されていることを確認

Bridge daemon のログ:

```bash
tail -f /var/log/djmt1-bridge.log
```

## Uninstall

```bash
sudo make uninstall
```

## Installer Package (.pkg)

署名済み `.pkg` をビルドする場合:

```bash
# 署名なし
make pkg

# 署名 + notarization
install/build-pkg.sh \
  --sign "Developer ID Installer: Ultra Xperience Inc. (4GAG263K82)" \
  --notarize
```

## Latency

| Measurement | Value |
|------------|-------|
| Round-trip latency | ~24ms |
| USB isochronous interval | 1ms |
| Ring buffer size | 256 frames (~5.3ms) |

DJ用途として実用的なレイテンシーを達成。

## Troubleshooting

### Audio MIDI Setup にデバイスが表示されない

```bash
# bridge が動作しているか確認
sudo launchctl list com.ultraxperience.djmt1-bridge

# ログを確認
tail -20 /var/log/djmt1-bridge.log

# coreaudiod を再起動
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
```

### USB permission error

DJM-T1がUSBで認識されているか確認:

```bash
system_profiler SPUSBDataType | grep -A5 "DJM"
```

### bridge が起動しない

libusb がインストールされているか確認:

```bash
pkg-config --libs libusb-1.0
```

## Project Structure

```
bridge/     USB isochronous bridge daemon (C++, libusb)
plugin/     Core Audio HAL AudioServerPlugin (C++, libASPL)
tools/      Development & testing utilities
install/    Installation scripts & launchd plist
```

## License

[MIT](LICENSE)

Third-party licenses: [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)
