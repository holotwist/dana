# DANA: Dana Audio Non-lossy Archive

DANA is a custom lossless audio codec designed for efficient predictive audio compression. It features multi-stage linear predictive coding (LPC/PARCOR), long-term prediction (LTP), adaptive LMS filtering, metadata/tag embedding, and a hybrid split mode.

The repository includes the core encoding/decoding library, a command-line interface tool (`dana`), a player (`danaplay`), and a playback daemon (`danaplayd`).

---

## Features

- **Compression Modes**: Supports multiple compression presets (from fast decoding to high compression configurations).
- **Hybrid Mode**: Optionally splits audio into a lossy base layer (`.dahl`) and a lossless correction layer (`.dahc`). Reconstructing the original lossless stream is done automatically if both files are present during decoding.
- **Seek Table Support**: Integrates embedded variable-length encoded seek tables inside the file header for seeking during playback.
- **Embedded Metadata (DanaID)**: Embeds title, artist, album, year, genre, track number, BPM, key, lyrics, and image cover art directly within the header structure.
- **Player**: A CLI player using the decode streaming API.
- **Control Daemon**: Background daemon that processes playback commands via UNIX domain sockets.

---

## System Requirements and Dependencies

To compile and run all components of Dana, your system requires:

- **Compiler**: A C11-compliant compiler (GCC or Clang recommended)
- **Build System**: CMake (version 3.16 or higher)
- **Libraries**:
  - ~~**ALSA** (Advanced Linux Sound Architecture) for audio output, or PipeWire with ALSA support~~
  - **Ncurses with Wide Character Support** for the player interface

---

## Compilation & Installation

The project includes an automated `build.sh` script supporting optimization tuning, installation, uninstallation, and release packaging.

### Quick Build (Native Hardware)
```bash
./build.sh
```

### Build Script Options
```text
Usage: ./build.sh [OPTIONS]

Build Options:
  -r, --release               Build in Release mode (-O3, default)
  -d, --debug                 Build in Debug mode (-g)
  -c, --clean                 Wipe build directory before building
  -m, --modern                Build for modern x86_64 baseline (x86-64-v3: AVX2/FMA/BMI2)
  -l, --level <level>         Specify x86_64 level (x86-64-v2, x86-64-v3, x86-64-v4)
      --portable              Build generic binary without native hardware flags

Installation Options:
      --install               Build and install all binaries (dana, danaplay, danaplayd)
      --install-essential     Build and install only the core 'dana' CLI binary
      --uninstall             Remove installed binaries using install manifest
      --prefix <dir>          Installation prefix (default: /usr/local)

Packaging:
  -p, --package [ver]         Bundle release binaries into dist/*.tar.gz with SHA-256
```

### Installation Examples

**Install only the core CLI encoder/decoder (`dana`):**
```bash
./build.sh --install-essential
```

**Install all tools to `/usr/local/bin`:**
```bash
./build.sh --install
```

**Cleanly uninstall:**
```bash
./build.sh --uninstall
```

### Manual CMake Build (Alternative)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

---

## Command-Line Tool (`dana`) Usage

The main tool handles both encoding WAV files into DANA files and decoding them back into standard WAV format.

### Basic Syntax
```bash
./dana [options] <INPUT_FILE> <OUTPUT_FILE>
```

### General Options
- `-h, --help`: Show command help message.
- `-v, --version`: Show version details.
- `-e, --encode`: Encode.
- `-d, --decode`: Decode.
- `-t, --threads <num>`: Limit worker thread count (default: auto-detect all CPU cores).
- `-p, --verpose`: Enable verbose mode.(displays stream info and completion status to `stderr`). *Note: The CLI option uses the exact spelling `--verpose`.*
- `-q, --quiet`: Quiet mode (suppresses all console output; errors only).

---

### Encoding Options & Functions

When encoding (`-e`), you can configure the compression behavior and embed metadata tags:

| Option | Argument | Description | Default |
| :--- | :--- | :--- | :--- |
| `-m, --mode` | `0` to `4` | Compression preset (0 = fastest decode, 4 = highest compression). | `2` |
| `-t, --threads` | `<num>` | Worker threads limit. | Auto |
| `-z, --seek-table` | `yes` or `no` | Generate and embed a seek table inside the header. | `yes` |
| `-x, --hybrid` | `<shift>` | Enable Hybrid mode. Specifies the bit-shift value (e.g., `6`) to output lossy `.dahl` and correction `.dahc` files. | Disabled |

#### Metadata Tagging Options:
- `--title "<string>"`: Audio track title.
- `--artist "<string>"`: Artist name.
- `--album "<string>"`: Album title.
- `--year "<string>"`: Release year.
- `--genre "<string>"`: Genre.
- `--track "<string>"`: Track number.
- `--bpm "<string>"`: Beats per minute.
- `--key "<string>"`: Musical key.
- `--lyrics "<string>"`: Embedded lyrics string.
- `--cover "<filepath>"`: Path to an image file (PNG/JPEG) to embed as cover art (maximum recommended size: 20MB).

#### Encoding Examples:

**Fastest Multicore Compression:**
```bash
dana -e -m 0 input.wav output.dana
```

**Maximum Compression using 4 Threads:**
```bash
dana -e -m 4 -t 4 input.wav output.dana
```

**Basic Lossless Encode (Preset 3 with Seek Table)**
```bash
./dana -e -m 3 -z yes input.wav output.dana
```

**Encoding with Metadata and Lyrics**
```bash
./dana -e -m 2 \
  --title "Song Title" \
  --artist "Artist Name" \
  --album "Album Title" \
  --lyrics "Hello\nWorld" \
  --cover "./artwork.jpg" \
  input.wav output.dana
```

**UNIX Pipe Streaming (stdin to stdout):**
```bash
cat input.wav | dana -e -m 0 - - > output.dana

# Transcode from FFmpeg pipe:
ffmpeg -i track.flac -f wav - | dana -e -m 0 - output.dana
```

**Hybrid Mode Encode**
Specifying a hybrid shift (e.g., `6` bits) divides the target audio file:
```bash
./dana -e -x 6 input.wav output
```
This command generates two files:
- `output.dahl`: The lossy base layer.
- `output.dahc`: The lossless correction layer.

---

### Decoding Options & Functions

When decoding (`-d`), the output format is determined by your destination file name (typically a `.wav` file):

| Option | Argument | Description | Default |
| :--- | :--- | :--- | :--- |
| `-t, --threads` | `<num>` | Worker threads limit. | Auto |
| `-c, --crc-check` | `yes` or `no` | Validate data integrity block-by-block using CRC16 during decompression. | `yes` |
| `-s, --streaming`| None | Debug option to test the streaming decompressor framework at 120Hz. | Off |

#### Decoding Examples:

**Standard Decode**
```bash
./dana -d input.dana output.wav
```

**UNIX Pipe Decompression:**
```bash
cat input.dana | dana -d - - > restored.wav

# Direct pipe playback via aplay:
dana -d input.dana - | aplay
```

**Hybrid Mode Lossless Reconstruction**
To reconstruct the original lossless file from a hybrid pair, simply point the decoder to the `.dahl` file:
```bash
./dana -d output.dahl reconstructed.wav
```
Note: The decompressor automatically searches for the corresponding `.dahc` file in the same directory. If present, it applies the corrections to rebuild the original lossless audio structure.

---

## The Player (`danaplay`)

`danaplay` is a CLI-based player. It scans directories for `.dana` or `.dahl` files and outputs audio via MiniAudio.

```bash
./danaplay

./danaplay /path/to/music/
```

### Key Bindings

| Key | Action |
| :---: | :--- |
| **Up / Down** | Navigate the file list / directory tree. |
| **Enter** | Open directory or play the selected song. |
| **Space / P** | Pause / Resume playback. |
| **Left / Right** | Seek backward / forward 5 seconds. |
| **N / >** | Skip to the next song in the directory list. |
| **B / <** | Skip to the previous song in the directory list. |
| **+ / =** | Increase volume. |
| **- / _** | Decrease volume. |
| **1** | Switch view to Visualizer. |
| **C / c** | Toggle Visualizer mode. |
| **2** | Switch view to Codec Stats. |
| **3** | Switch view to Lyrics (if present). |
| **F / f** | Toggle Fullscreen mode. |
| **Q / q** | Quit the player. |

---

## Playback Daemon (`danaplayd`)

For headless configurations or scripts, `danaplayd` runs as a background playback daemon. It communicates via a UNIX domain socket created at `/tmp/danaplayd.sock`.

### Running the Daemon
```bash
./danaplayd
```

### Client Control
You can write commands directly to the socket to control playback. For example, using `nc` (netcat):

```bash
# Play a file
echo "play /path/to/audio.dana" | nc -U /tmp/danaplayd.sock

# Pause playback
echo "pause" | nc -U /tmp/danaplayd.sock

# Seek to 30 seconds
echo "seek 30" | nc -U /tmp/danaplayd.sock

# Adjust volume to 120%
echo "set_vol 120" | nc -U /tmp/danaplayd.sock

# Retrieve current track metadata and playing state (returns JSON)
echo "get_data" | nc -U /tmp/danaplayd.sock

# Extract embedded cover artwork binary data
echo "get_cover" | nc -U /tmp/danaplayd.sock > cover.jpg

# Stop/kill the daemon
echo "quit" | nc -U /tmp/danaplayd.sock

---

## License & Attributions

Dana is licensed under the **Apache License, Version 2.0**. See [LICENSE](LICENSE) for details.

Third-party software attributions and acknowledgments are documented in [NOTICE.txt](NOTICE.txt)