# Bleash 📱

A Bluetooth leash application for the Flipper Zero that monitors BLE connection strength and alerts when devices move too far apart.

## Features 🌟

- Real-time RSSI (Signal Strength) monitoring
- Configurable distance threshold
- Background monitoring capability
- Vibration alerts when signal drops below threshold
- Automatic event logging with timestamps

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
2. Use the OK button to toggle background monitoring on/off
3. Press BACK to exit the configuration screen
4. The app will monitor BLE signal strength and vibrate when devices move too far apart
5. Logs are stored in `/ext/Bleash/bleash.log`

## Configuration ⚙️

Default settings in `bleash.c`:
- `RSSI_THRESHOLD`: -70 dBm
- `POLL_INTERVAL_MS`: 1000ms
- `DEFAULT_BACKGROUND_RUNNING`: false

## Logs 📝

Log entries are stored in `/ext/Bleash/bleash.log` with the following format:
```
YYYY-MM-DD HH:MM:SS: RSSI <value>
```

## Contributing 🤝

Pull requests are welcome! For major changes, please open an issue first to discuss what you'd like to change. I will try my best to respond.

## License 📄

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details

## Credits 🤘🏼

- GitHub: [@C0d3-5t3w](https://github.com/C0d3-5t3w)

