# VBAN4mac

A VBAN audio bridge implementation for macOS, allowing you to stream audio between devices using the VBAN protocol.

## Prerequisites

- macOS (tested on 10.15 and later)
- Xcode Command Line Tools
- Git

## Installation

1. Clone the repository:

```bash
git clone https://github.com/yourusername/VBAN4mac.git
```

2. Compile the project:

```bash
cd VBAN4mac
make clean
make
```

## Configuration

Create a configuration file (e.g., `config.ini`) with the following format:

```ini
[network]
remote_ip=192.168.1.100
stream_name=MyStream1
port=6980
[audio]
input_device=Built-in Microphone
output_device=Built-in Output
```

### Configuration Parameters

- `remote_ip`: The IP address of the remote VBAN host
- `stream_name`: Name of the VBAN stream (must be unique for multiple instances)
- `port`: UDP port for VBAN communication (default: 6980)
- `input_device`: Name of the audio input device
- `output_device`: Name of the audio output device

## Usage

The project includes a management script (`scripts/vban_bridge.sh`) to control VBAN bridges.

### Basic Commands

1. Start a new bridge:

```bash
./scripts/vban_bridge.sh start config.ini
```

2. Check status of all running bridges:

```bash
./scripts/vban_bridge.sh status
```

3. Stop a specific bridge:

```bash
./scripts/vban_bridge.sh stop MyStream1
```

4. Restart a bridge:

```bash
./scripts/vban_bridge.sh restart MyStream1 config.ini
```

### Running Multiple Instances

You can run multiple bridges simultaneously by using different configuration files with unique stream names.

**You need to use different ports for each bridge, in the future I will add support for multiple bridges on the same port.**

Example:

```bash
./scripts/vban_bridge.sh start config1.ini
./scripts/vban_bridge.sh start config2.ini
```

### Check status of all bridges

```bash
./scripts/vban_bridge.sh status
```

## Logs

The bridge runs as a daemon and logs to syslog. View logs with:

```bash
sudo tail -f /var/log/system.log | grep vban_bridge
```

## Troubleshooting

1. **Bridge fails to start**
   - Check if the configured audio devices exist
   - Verify the port is not in use
   - Ensure the remote IP is correct

2. **No audio transmission**
   - Check network connectivity
   - Verify VBAN is enabled on the remote host
   - Confirm audio device permissions

## License

This project is licensed under the MIT License. See the LICENSE file for details.

## Contributing

Contributions are welcome! Please open an issue or submit a pull request.
