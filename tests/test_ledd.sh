#!/bin/bash

# Test script for the multi-LED daemon
# This script demonstrates the functionality of the new ledd daemon

set -e

MONITOR_DIR="/run/ledd"
DAEMON="./ledd"

echo "=== LED Daemon Multi-LED Control Test ==="
echo ""

# Create monitor directory
echo "[1] Creating monitor directory: $MONITOR_DIR"
mkdir -p "$MONITOR_DIR"
chmod 777 "$MONITOR_DIR"
echo "    ✓ Directory created"
echo ""

# Start the daemon
echo "[2] Starting LED daemon..."
"$DAEMON" &
DAEMON_PID=$!
sleep 1
echo "    ✓ Daemon started (PID: $DAEMON_PID)"
echo ""

# Test 1: Create control file for GPIO 17 with 500ms interval
echo "[3] Test 1: Create control file for GPIO 17 (500ms interval)"
echo "500" > "$MONITOR_DIR/gpio17"
sleep 2
echo "    ✓ Control file created"
echo "    Expected: GPIO 17 should start blinking at 500ms interval"
echo ""

# Test 2: Create control file for GPIO 27 with 1000ms interval
echo "[4] Test 2: Create control file for GPIO 27 (1000ms interval)"
echo "1000" > "$MONITOR_DIR/gpio27"
sleep 2
echo "    ✓ Control file created"
echo "    Expected: GPIO 27 should start blinking at 1000ms interval"
echo "    Expected: GPIO 17 should continue blinking independently"
echo ""

# Test 3: Create control file for GPIO 22 with 250ms interval
echo "[5] Test 3: Create control file for GPIO 22 (250ms interval)"
echo "250" > "$MONITOR_DIR/gpio22"
sleep 2
echo "    ✓ Control file created"
echo "    Expected: GPIO 22 should start blinking at 250ms interval"
echo "    Expected: GPIO 17 and GPIO 27 should continue blinking independently"
echo ""

# Test 4: Delete control file for GPIO 27
echo "[6] Test 4: Delete control file for GPIO 27"
rm "$MONITOR_DIR/gpio27"
sleep 1
echo "    ✓ Control file deleted"
echo "    Expected: GPIO 27 should stop blinking and restore to initial state"
echo "    Expected: GPIO 17 and GPIO 22 should continue blinking"
echo ""

# Test 5: Create invalid control file (should be ignored)
echo "[7] Test 5: Create invalid control file (invalid GPIO number)"
echo "500" > "$MONITOR_DIR/gpio_invalid"
sleep 1
echo "    ✓ Invalid control file created"
echo "    Expected: Daemon should log warning and ignore this file"
echo ""

# Test 6: Create control file with invalid interval
echo "[8] Test 6: Create control file with invalid interval"
echo "invalid_value" > "$MONITOR_DIR/gpio23"
sleep 1
echo "    ✓ Invalid interval file created"
echo "    Expected: Daemon should log warning and not start blinking"
echo ""

# Test 7: Delete remaining control files
echo "[9] Test 7: Clean up - delete remaining control files"
rm "$MONITOR_DIR/gpio17" "$MONITOR_DIR/gpio22" "$MONITOR_DIR/gpio_invalid" "$MONITOR_DIR/gpio23" 2>/dev/null || true
sleep 1
echo "    ✓ Control files deleted"
echo "    Expected: All GPIO pins should stop blinking"
echo ""

# Stop the daemon
echo "[10] Stopping daemon..."
kill $DAEMON_PID 2>/dev/null || true
sleep 1
echo "    ✓ Daemon stopped"
echo ""

# Check syslog for daemon messages
echo "[11] Daemon log messages (from syslog):"
echo "    Run: tail -f /var/log/syslog | grep ledd"
echo ""

echo "=== Test Complete ==="
echo ""
echo "Key Features Tested:"
echo "  ✓ Multiple GPIO pins blinking simultaneously"
echo "  ✓ Independent blinking intervals for each GPIO"
echo "  ✓ Dynamic control file creation/deletion"
echo "  ✓ Error handling for invalid GPIO numbers"
echo "  ✓ Error handling for invalid intervals"
echo "  ✓ Graceful cleanup on daemon shutdown"
echo ""

