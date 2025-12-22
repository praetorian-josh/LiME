# LiME Control Tools

## lime_ioctl

Userspace control tool for LiME when compiled with `CONFIG_LIME_STEALTH=y`.

### Build

```bash
# For desktop Linux
make

# For Android (static binary)
make android

# Or manually:
aarch64-linux-android-gcc -static -o lime_ioctl lime_ioctl.c
```

### Usage

```bash
# Full memory dump
./lime_ioctl -1 /data/local/tmp/memory.lime

# Process-specific dump
./lime_ioctl 1234 /data/local/tmp/process.lime

# With options
./lime_ioctl -f raw -d sha256 -D -1 /data/dump.raw

# Over TCP
./lime_ioctl 5678 tcp:4444
```

### Arguments

- `pid`: Process ID to dump, or `-1` for full physical memory
- `output_path`: File path or `tcp:port` for network transfer

### Options

- `-f, --format FORMAT`: Output format (`raw`, `lime`, `padded`)
- `-d, --digest ALGO`: Hash algorithm (`md5`, `sha1`, `sha256`)
- `-D, --dio`: Enable Direct I/O
- `-l, --localhost-only`: Bind TCP to localhost only
- `-h, --help`: Show help message

### Device

Communicates with `/dev/lwis-sensor-imx461` (disguised as camera sensor)

### Detection Characteristics

**Stealthy:**
- No `/sys/kernel/lime/` directory
- Device name blends with existing lwis camera devices
- ioctl operations look like normal driver communication

**Visible:**
- `/dev/lwis-sensor-imx461` appears in `/dev/` (but looks legitimate)
- Binary `lime_ioctl` on filesystem (can be deleted after use)

### Push to Android

```bash
adb push lime_ioctl /data/local/tmp/
adb shell chmod +x /data/local/tmp/lime_ioctl
adb shell /data/local/tmp/lime_ioctl -1 /data/local/tmp/memory.lime
adb pull /data/local/tmp/memory.lime
```
