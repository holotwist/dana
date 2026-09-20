#!/usr/bin/env bash
set -e
set -o pipefail

BUILD_DIR="build"
DIST_DIR="dist"
PORTABLE=OFF
MODERN_X86=OFF
X86_LEVEL="x86-64-v3"
BUILD_TYPE="Release"
PACKAGE=0
PACKAGE_VER=""
CLEAN=0
DO_INSTALL=0
INSTALL_ESSENTIAL=0
DO_UNINSTALL=0
INSTALL_PREFIX=""
CMAKE_FLAGS=()

print_help() {
    cat << EOF
Usage: ./build.sh [OPTIONS] [-- <additional cmake flags>]

Build & Optimization Options:
  -r, --release               Build in Release mode (-O3, default)
  -d, --debug                 Build in Debug mode (-g)
  -c, --clean                 Wipe the build directory before building
  -m, --modern                Build for modern x86_64 baseline (x86-64-v3 - AVX2/FMA/BMI2)
  -l, --level <level>         Specify x86_64 level (x86-64-v2, x86-64-v3, x86-64-v4)
      --portable              Build generic binary without native hardware optimization

Installation Options:
      --install               Build and install all binaries (dana, danaplay, danaplayd)
      --install-essential     Build and install only the core 'dana' CLI binary
      --uninstall             Uninstall files using CMake install manifest
      --prefix <dir>          Installation prefix directory (default: /usr/local)

Packaging Options:
  -p, --package [ver]         Build release binaries and bundle into dist/*.tar.gz
  -h, --help                  Show this help message

Examples:
  ./build.sh                  Build all targets with native CPU optimizations
  ./build.sh --install        Build and install all tools to /usr/local/bin
  ./build.sh --install-essential  Build and install only 'dana' CLI
  ./build.sh -m -p v1.0.0     Package x86-64-v3 tarball with version v1.0.0
  ./build.sh --uninstall      Remove previously installed binaries
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--clean)
            CLEAN=1
            shift
            ;;
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -r|--release)
            BUILD_TYPE="Release"
            shift
            ;;
        --portable)
            PORTABLE=ON
            shift
            ;;
        -m|--modern)
            MODERN_X86=ON
            shift
            ;;
        -l|--level)
            X86_LEVEL="$2"
            MODERN_X86=ON
            shift 2
            ;;
        --install)
            DO_INSTALL=1
            shift
            ;;
        --install-essential)
            DO_INSTALL=1
            INSTALL_ESSENTIAL=1
            shift
            ;;
        --uninstall)
            DO_UNINSTALL=1
            shift
            ;;
        --prefix)
            INSTALL_PREFIX="$2"
            shift 2
            ;;
        -p|--package)
            PACKAGE=1
            if [[ $# -gt 1 && "$2" != -* ]]; then
                PACKAGE_VER="$2"
                shift 2
            else
                shift
            fi
            ;;
        -h|--help)
            print_help
            exit 0
            ;;
        --)
            shift
            CMAKE_FLAGS+=("$@")
            break
            ;;
        *)
            CMAKE_FLAGS+=("$1")
            shift
            ;;
    esac
done

# Handle uninstall
if [ "$DO_UNINSTALL" -eq 1 ]; then
    MANIFESTS=("${BUILD_DIR}"/install_manifest*.txt)
    FOUND=0
    for m in "${MANIFESTS[@]}"; do
        [ -f "$m" ] && FOUND=1
    done

    if [ "$FOUND" -eq 0 ]; then
        echo "Error: No install_manifest found in ${BUILD_DIR}. Cannot uninstall."
        exit 1
    fi

    echo "==> Uninstalling installed files..."
    for m in "${MANIFESTS[@]}"; do
        if [ -f "$m" ]; then
            while IFS= read -r file; do
                if [ -f "$file" ] || [ -L "$file" ]; then
                    echo "Removing $file"
                    if [ -w "$file" ]; then
                        rm -f "$file"
                    else
                        sudo rm -f "$file"
                    fi
                fi
            done < "$m"
            rm -f "$m"
        fi
    done
    echo "==> Uninstallation complete."
    exit 0
fi

if [ "$CLEAN" -eq 1 ] && [ -d "$BUILD_DIR" ]; then
    echo "==> Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

CMAKE_CONFIG_ARGS=(
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DPORTABLE_BUILD="$PORTABLE"
    -DMODERN_X86_64="$MODERN_X86"
)

if [ "$MODERN_X86" = "ON" ]; then
    CMAKE_CONFIG_ARGS+=("-DX86_64_LEVEL=$X86_LEVEL")
fi

if [ -n "$INSTALL_PREFIX" ]; then
    CMAKE_CONFIG_ARGS+=("-DCMAKE_INSTALL_PREFIX=$INSTALL_PREFIX")
fi

if [ ${#CMAKE_FLAGS[@]} -gt 0 ]; then
    CMAKE_CONFIG_ARGS+=("${CMAKE_FLAGS[@]}")
fi

echo "==> Configuring with: cmake ${CMAKE_CONFIG_ARGS[*]}"
cmake "${CMAKE_CONFIG_ARGS[@]}"

echo "==> Building project..."

BUILD_CMD=(cmake --build "$BUILD_DIR" --parallel)
if command -v stdbuf >/dev/null 2>&1; then
    BUILD_CMD=(stdbuf -oL -eL "${BUILD_CMD[@]}")
fi

if [ -t 1 ]; then
    "${BUILD_CMD[@]}" 2>&1 | while IFS= read -r line; do
        if [[ "$line" =~ ^\[[[:space:]]*([0-9]+(%|/[0-9]+))\][[:space:]]*(.*) ]]; then
            pct="${BASH_REMATCH[1]}"
            rest="${BASH_REMATCH[3]}"

            if [[ "$rest" =~ Building[[:space:]]C[[:space:]]object[[:space:]]+(.*) ]]; then
                file="${BASH_REMATCH[1]}"
                file="${file#CMakeFiles/*.dir/}"
                file="${file%.o}"
                printf "\r\033[K\033[1;32m[%4s]\033[0m %s" "$pct" "$file"
            elif [[ "$rest" =~ Linking[[:space:]]+(.*) ]]; then
                target="${BASH_REMATCH[1]}"
                target="${target#*executable }"
                target="${target#*library }"
                printf "\r\033[K\033[1;36m[%4s]\033[0m Linking %s" "$pct" "$target"
            elif [[ "$rest" =~ Built[[:space:]]target[[:space:]]+(.*) ]]; then
                printf "\r\033[K\033[1;34m[%4s]\033[0m Built %s" "$pct" "${BASH_REMATCH[1]}"
            fi
        else
            printf "\n%s" "$line"
        fi
    done
    printf "\r\033[K\033[1;32m==> Build complete.\033[0m\n"
else
    "${BUILD_CMD[@]}"
fi

# Handle installation
if [ "$DO_INSTALL" -eq 1 ]; then
    echo "==> Installing..."
    INSTALL_CMD=(cmake --install "$BUILD_DIR")
    if [ "$INSTALL_ESSENTIAL" -eq 1 ]; then
        INSTALL_CMD+=(--component essential)
    fi
    if [ -n "$INSTALL_PREFIX" ]; then
        INSTALL_CMD+=(--prefix "$INSTALL_PREFIX")
    fi

    TARGET_DIR="${INSTALL_PREFIX:-/usr/local}/bin"
    if [ ! -w "$TARGET_DIR" ] && [ "$EUID" -ne 0 ]; then
        echo "Need sudo permissions to install into $TARGET_DIR:"
        sudo "${INSTALL_CMD[@]}"
    else
        "${INSTALL_CMD[@]}"
    fi
    echo "==> Installation complete."
fi

# Handle packaging
if [ "$PACKAGE" -eq 1 ]; then
    echo "==> Packaging release tarballs..."

    VERSION="$PACKAGE_VER"
    if [ -z "$VERSION" ]; then
        if command -v git >/dev/null 2>&1 && git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
            VERSION=$(git describe --tags --always 2>/dev/null || echo "v1.0.0")
        else
            VERSION="v1.0.0"
        fi
    fi
    [[ "$VERSION" != v* ]] && VERSION="v${VERSION}"

    OS_NAME="$(uname -s | tr '[:upper:]' '[:lower:]')"
    RAW_ARCH="$(uname -m)"
    case "$RAW_ARCH" in
        x86_64|amd64) ARCH="x86_64" ;;
        aarch64|arm64) ARCH="aarch64" ;;
        *) ARCH="$RAW_ARCH" ;;
    esac

    calc_sha256() {
        local file="$1"
        if command -v sha256sum >/dev/null 2>&1; then
            sha256sum "$file"
        elif command -v shasum >/dev/null 2>&1; then
            shasum -a 256 "$file"
        else
            openssl dgst -sha256 "$file" | awk '{print $NF "  " "'"$file"'"}'
        fi
    }

    mkdir -p "$DIST_DIR"
    PKG_NAME="dana-${VERSION}-${OS_NAME}-${ARCH}"
    PKG_STAGE="${DIST_DIR}/${PKG_NAME}"
    rm -rf "$PKG_STAGE"
    mkdir -p "$PKG_STAGE"

    for bin in dana danaplay danaplayd; do
        if [ -f "${BUILD_DIR}/${bin}" ]; then
            cp "${BUILD_DIR}/${bin}" "${PKG_STAGE}/"
            command -v strip >/dev/null 2>&1 && strip -s "${PKG_STAGE}/${bin}" 2>/dev/null || true
        fi
    done

    for doc in README.md LICENSE LICENSE.txt NOTICE.txt; do
        [ -f "$doc" ] && cp "$doc" "${PKG_STAGE}/"
    done

    PKG_TAR="${PKG_NAME}.tar.gz"
    tar -czf "${DIST_DIR}/${PKG_TAR}" -C "$DIST_DIR" "$PKG_NAME"
    (cd "$DIST_DIR" && calc_sha256 "$PKG_TAR" > "${PKG_TAR}.sha256")
    rm -rf "$PKG_STAGE"
    echo -e "  \033[1;32m[+]\033[0m Created ${DIST_DIR}/${PKG_TAR}"
    echo -e "  \033[1;32m[+]\033[0m Checksum in ${DIST_DIR}/${PKG_TAR}.sha256"
fi