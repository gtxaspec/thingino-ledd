# thingino-ledd

Multi-LED control daemon for simultaneous independent GPIO blinking with inotify-based file monitoring.

## Build

```bash
make clean
make
```

## Run

```bash
./ledd
```

The daemon detaches and runs in the background on its own. Do not append `&`.

## Control LEDs

Create control files in `/run/ledd/` with pin number as filename:

```bash
# Blink GPIO 17 at 500ms interval
echo "500" > /run/ledd/17

# Blink GPIO 27 at 1000ms interval
echo "1000" > /run/ledd/27

# Stop blinking
rm /run/ledd/17
rm /run/ledd/27
```

## Documentation

- **[docs/DESIGN.md](docs/DESIGN.md)**: Architecture, data structures, and implementation
- **[docs/EXAMPLES.md](docs/EXAMPLES.md)**: Usage examples and troubleshooting

## Requirements

- Linux kernel with inotify support
- POSIX threads (pthread)
- GPIO sysfs interface (`/sys/class/gpio/`)
- Write access to `/sys/class/gpio/` and `/run/ledd/`

## Logs

View daemon logs:

```bash
dmesg | grep ledd
logread | grep ledd
```
