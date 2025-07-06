# Bleash 📱

A Bluetooth leash application for the Flipper Zero that monitors BLE connection strength and alerts when devices move too far apart.

## Features 🌟

- Real-time BLE connection status monitoring
- RSSI (Signal Strength) tracking and visualization
- Configurable distance threshold alerts
- True background monitoring capability
- Vibration and LED alerts for weak signals and disconnections
- Automatic event logging with timestamps and BT status
- Persistent state saving (remembers monitoring status)
- Smart dual-mode operation (GUI mode + background mode)

## Installation 🔧

1. Clone this repository:
```bash
git clone https://github.com/C0d3-5t3w/Bleash.git
```

2. Build using Flipper Build Tool (fbt):
```bash
cd Bleash
ufbt
```

3. Copy the generated `bleash.fap` to your Flipper Zero's `apps` directory

## Usage 📖

1. Launch Bleash from the "Examples" menu on your Flipper Zero
2. **OK Button**: Toggle background monitoring on/off
   - Green LED blink = Monitoring enabled
   - Red LED blink = Monitoring disabled
3. **Back Button**: Hide GUI and continue monitoring in background
4. **Long Back Button**: Fully exit the application
5. The app will monitor BLE connection status and alert you when:
   - Signal strength drops below threshold (vibration + red LED)
   - Device disconnects completely (double vibration pattern)
6. All events are logged to `/ext/Bleash/bleash.log`

## Display Information 📊

The app shows:
- **BT Status**: Off, Advertising, Connected, or Unavailable
- **Signal Strength**: RSSI value in dBm with visual indicator
- **Monitoring Status**: ON/OFF indicator
- **Controls**: Button usage hints

## Configuration ⚙️

Default settings in `bleash.c`:
- `RSSI_THRESHOLD`: -70 dBm (signal strength threshold)
- `POLL_INTERVAL_MS`: 1000ms (monitoring frequency)
- `DEFAULT_BACKGROUND_RUNNING`: false (starts with monitoring off)
- `VIEW_UPDATE_INTERVAL`: 500ms (GUI refresh rate)

## Alert System 🚨

The app provides different alert patterns:
- **Weak Signal**: Single vibration + red LED blink (when connected but signal < threshold)
- **Disconnection**: Double vibration pattern (when device disconnects)
- **Status Change**: Green/Red LED feedback when toggling monitoring

## Background Operation 🔄

- **Hide GUI**: Press Back button to hide interface while keeping monitoring active
- **True Background**: Worker thread continues monitoring even when GUI is hidden
- **Persistent State**: Monitoring status is saved and restored between sessions
- **Full Exit**: Long press Back button to completely stop the application

## Logs 📝

Log entries are stored in `/ext/Bleash/bleash.log` with the following format:
```
YYYY-MM-DD HH:MM:SS: BT=Status RSSI=Value
```

Example entries:
```
2025-07-05 20:15:30: BT=Connected RSSI=-65
2025-07-05 20:15:35: BT=Off RSSI=-127
2025-07-05 20:15:40: BT=Advertising RSSI=-80
```

## Troubleshooting 🔧

**App crashes or doesn't start:**
- Ensure your Flipper Zero firmware supports BLE functionality
- Check that `/ext/` directory exists and is writable
- Verify BT service is available in system

**No alerts when device moves away:**
- Check that monitoring is enabled (green indicator)
- Verify BT device is properly paired and connected
- Adjust RSSI_THRESHOLD if needed for your use case

**Background monitoring not working:**
- Use Back button (not Long Back) to hide GUI
- Check logs to confirm worker thread is active
- Relaunch app if monitoring stops unexpectedly

## Technical Details 🔧

**Architecture:**
- Multi-threaded design with dedicated worker thread for BLE monitoring
- Mutex-based synchronization for thread-safe operations
- Event-driven GUI updates with timer-based refresh
- State persistence using Flipper's storage API

**BLE Integration:**
- Uses Flipper's BT service for connection status monitoring
- Implements BT status change callbacks for real-time updates
- Supports GATT/GAP detection and active connection checking
- Mock RSSI calculation based on connection state (real RSSI requires hardware-specific implementation)

**Memory Management:**
- Proper resource allocation and cleanup
- Background operation with selective GUI cleanup
- Thread-safe termination handling

**File System:**
- Logs stored in `/ext/Bleash/` directory
- State file: `/ext/Bleash/bleash.state`
- Auto-creates directory structure if missing

## Contributing 🤝

Pull requests are welcome! For major changes, please open an issue first to discuss what you'd like to change. I will try my best to respond.

## License 📄

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details

## Credits 🤘🏼

- GitHub: [@C0d3-5t3w](https://github.com/C0d3-5t3w)

