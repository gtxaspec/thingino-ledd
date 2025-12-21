# LED Daemon Design

## Overview

The daemon monitors `/run/ledd/` for control files and manages GPIO pin blinking. Each control file triggers independent blinking on a specific GPIO pin. The daemon uses inotify for efficient file system monitoring and POSIX threads for concurrent GPIO control.

## Data Structures

```c
typedef struct {
	int gpio_pin;                    // GPIO pin number
	int initial_state;               // GPIO state before blinking
	double blink_interval_ms;        // Interval in milliseconds
	volatile int is_blinking;        // Blinking control flag
	pthread_t blink_thread;          // Thread handle
	pthread_mutex_t state_lock;      // Thread synchronization
} gpio_state_t;
```

## Control File Format

- **Location**: `/run/ledd/`
- **Filename**: PIN number (e.g., `17`, `27`)
- **Content**: Single number representing blinking interval in milliseconds
- **Valid range**: 1-60000 ms

## Blinking Behavior

- GPIO toggles between ON and OFF at the specified interval
- Each half-cycle duration = interval / 2
- Example: 500ms interval = 250ms ON + 250ms OFF
- Initial GPIO state is stored before blinking starts
- GPIO is restored to initial state when blinking stops

## File Monitoring

- Uses Linux inotify API for event-driven monitoring
- Watches for IN_CREATE and IN_DELETE events
- Non-blocking I/O with select() for responsiveness
- No polling overhead

## Concurrency

- Main thread monitors inotify events
- Worker thread created for each active GPIO pin
- Each thread manages independent blinking timer
- Thread-safe state management with mutexes:
  - `gpio_list_lock`: Protects active GPIO list
  - `state_lock` (per GPIO): Protects individual GPIO state

## Error Handling

**Invalid GPIO numbers**: Filename is not a valid number
- Action: Logged as warning, file ignored

**Invalid intervals**: Non-numeric or out-of-range (must be 1-60000 ms)
- Action: Logged as warning, GPIO not started

**Permission errors**: Cannot access GPIO sysfs files
- Action: Logged as error, operation fails gracefully

**Missing files**: File disappears during read
- Action: Logged as warning, operation skipped

**Maximum GPIO limit**: Exceeds 32 simultaneous pins
- Action: Logged as error, new GPIO rejected

## GPIO Control

- GPIO is automatically exported when blinking starts
- Direction is set to "output"
- GPIO is unexported when blinking stops
- Already-exported GPIOs are handled gracefully

## Signal Handling

- SIGTERM and SIGINT trigger graceful shutdown
- All GPIO pins are stopped and restored
- All threads are joined before exit

## Logging

All events logged to kernel log buffer with identifier "ledd":

```bash
dmesg | grep ledd
logread | grep ledd
```

Log levels:
- LOG_INFO: Normal operations
- LOG_WARNING: Non-critical issues
- LOG_ERR: Critical errors

## Limitations

- Maximum 32 simultaneous GPIO pins (configurable via MAX_GPIO_PINS)
- Maximum interval: 60 seconds (60000 ms)
- Minimum practical interval: ~10ms (system overhead)

