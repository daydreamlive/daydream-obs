# Daydream OBS Plugin

## Development (macOS)

### Prerequisites

- CMake 3.28+
- Xcode

### Setup

```bash
cmake --preset macos
```

### Build

```bash
cmake --build build_macos --config Debug
```

### Install (symlink)

```bash
ln -sf "$(pwd)/build_macos/Debug/daydream-obs.plugin" \
  ~/Library/Application\ Support/obs-studio/plugins/
```

### Iterate

1. Edit code
2. `cmake --build build_macos --config Debug`
3. Restart OBS
