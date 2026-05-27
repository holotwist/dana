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
  - **ALSA** (Advanced Linux Sound Architecture) for audio output, or PipeWire with ALSA support
  - **Ncurses with Wide Character Support** for the player interface

---

## Compilation

```bash
cd dana

mkdir build
cd build

cmake -DCMAKE_BUILD_TYPE=Release ..
make
```

Upon compilation, three executable files will be created in the build directory:
- `dana`: Command-line tool for encoding and decoding.
- `danaplay`: The player.
- `danaplayd`: Playback daemon.

---

## Command-Line Tool (`dana`) Usage

The main tool handles both encoding WAV files into DANA files and decoding them back into standard WAV format.

### Basic Syntax
```bash
./dana [options] <INPUT_FILE> <OUTPUT_FILE>
```

### General Options
- `-h, --help`: Show the command help message.
- `-v, --version`: Show version details.
- `-e, --encode`: Encode.
- `-d, --decode`: Decode.
- `-p, --verpose`: Enable verbose mode. *Note: The CLI option uses the exact spelling `--verpose`.*
- `-q, --quiet`: Run in quiet mode.

---

### Encoding Options & Functions

When encoding (`-e`), you can configure the compression behavior and embed metadata tags:

| Option | Argument | Description | Default |
| :--- | :--- | :--- | :--- |
| `-m, --mode` | `0` to `4` | Compression preset (0 = fastest decode, 4 = highest compression). | `2` |
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
| `-c, --crc-check` | `yes` or `no` | Validate data integrity block-by-block using CRC16 during decompression. | `yes` |
| `-s, --streaming`| None | Debug option to test the streaming decompressor framework at 120Hz. | Off |

#### Decoding Examples:

**Standard Lossless Decode**
```bash
./dana -d input.dana output.wav
```

**Hybrid Mode Lossless Reconstruction**
To reconstruct the original lossless file from a hybrid pair, simply point the decoder to the `.dahl` file:
```bash
./dana -d output.dahl reconstructed.wav
```
Note: The decompressor automatically searches for the corresponding `.dahc` file in the same directory. If present, it applies the corrections to rebuild the original lossless audio structure.

---

## The Player (`danaplay`)

`danaplay` is a CLI-based player. It scans directories for `.dana` or `.dahl` files and outputs audio via ALSA.

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
