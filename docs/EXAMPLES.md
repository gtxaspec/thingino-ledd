# LED Daemon Examples

## Basic Usage

### Single LED

```bash
./ledd
echo "1000" > /run/ledd/17
sleep 5
rm /run/ledd/17
```

### Multiple LEDs

```bash
./ledd
echo "500" > /run/ledd/17
echo "1000" > /run/ledd/27
echo "250" > /run/ledd/22
sleep 10
rm /run/ledd/*
```

### Different Intervals

```bash
./ledd
echo "100" > /run/ledd/17    # Fast: 100ms
echo "500" > /run/ledd/27    # Medium: 500ms
echo "2000" > /run/ledd/22   # Slow: 2000ms
```

## Practical Scenarios

### Boot Status Indication

```bash
#!/bin/bash
./ledd

# Boot starting - fast blinking
echo "200" > /run/ledd/17
sleep 5

# Boot in progress - medium blinking
rm /run/ledd/17
echo "500" > /run/ledd/17
sleep 5

# Boot complete - slow blinking
rm /run/ledd/17
echo "1000" > /run/ledd/17
```

### Multi-Status Indication

```bash
./ledd

# Status LED - healthy
echo "1000" > /run/ledd/17

# Network LED - connected
echo "500" > /run/ledd/27

# Activity LED - active
echo "200" > /run/ledd/22
```

### Dynamic Interval Adjustment

```bash
#!/bin/bash
./ledd

echo "1000" > /run/ledd/17
sleep 5

rm /run/ledd/17
echo "500" > /run/ledd/17
sleep 5

rm /run/ledd/17
echo "200" > /run/ledd/17
```

## Error Scenarios

### Invalid GPIO Number

```bash
echo "500" > /run/ledd/invalid
# Result: Daemon logs warning, file ignored
```

### Invalid Interval

```bash
echo "invalid" > /run/ledd/17
# Result: Daemon logs warning, GPIO not started

echo "100000" > /run/ledd/17
# Result: Out of range, daemon logs warning
```

### Missing GPIO

```bash
echo "500" > /run/ledd/999
# Result: GPIO export fails, daemon logs error
```

## Troubleshooting

### Check Daemon Status

```bash
ps w | grep ledd
pgrep ledd
```

### View Logs

```bash
dmesg | grep ledd
logread | grep ledd
```

### Check Active GPIOs

```bash
ls -la /run/ledd/
cat /run/ledd/17
```

### Stop All Blinking

```bash
rm /run/ledd/*
pkill ledd
```

### Restart Daemon

```bash
pkill ledd
./ledd
```

## Scripted Control

```bash
#!/bin/bash

start_led() {
    local pin=$1
    local interval=$2
    echo "$interval" > /run/ledd/$pin
    echo "GPIO $pin: ON (${interval}ms)"
}

stop_led() {
    local pin=$1
    rm /run/ledd/$pin 2>/dev/null
    echo "GPIO $pin: OFF"
}

start_led 17 500
start_led 27 1000
sleep 5
stop_led 17
stop_led 27
```



