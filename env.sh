# Source this script to add OrphanVM output binary directory to PATH:
#   source env.sh [arm|amd|simics|intel]
#
# If no architecture argument is provided, defaults to x86_64 (or arm64 if the host is aarch64/arm64).

if [ -n "$BASH_SOURCE" ]; then
    ORPHANVM_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
elif [ -n "$ZSH_VERSION" ]; then
    ORPHANVM_ROOT="$(cd "$(dirname "${(%):-%x}")" && pwd)"
else
    ORPHANVM_ROOT="$(pwd)"
fi

TARGET_ARG="$1"
if [ -z "$TARGET_ARG" ]; then
    if [ "$(uname -m)" = "aarch64" ] || [ "$(uname -m)" = "arm64" ]; then
        TARGET_ARG="arm"
    else
        TARGET_ARG="amd"
    fi
fi

case "$TARGET_ARG" in
    simics|simics64|x86_64-simics)
        TARGET_ARCH="x86_64"
        OVM_PLATFORM="simics"
        DEFAULT_SSH_PORT="2224"
        PROMPT_LABEL="Simics"
        ;;
    intel|intel64|x86_64-intel)
        TARGET_ARCH="x86_64"
        OVM_PLATFORM="intel"
        DEFAULT_SSH_PORT="2225"
        PROMPT_LABEL="Intel"
        ;;
    amd|amd64|x86_64|x86_64-amd)
        TARGET_ARCH="x86_64"
        OVM_PLATFORM="amd"
        DEFAULT_SSH_PORT="2222"
        PROMPT_LABEL="AMD"
        ;;
    arm|arm64|aarch64)
        TARGET_ARCH="arm64"
        OVM_PLATFORM="arm"
        DEFAULT_SSH_PORT="2223"
        PROMPT_LABEL="arm"
        ;;
    *)
        echo "[OrphanVM] Warning: Unrecognized target '$TARGET_ARG'. Expected 'intel', 'amd', 'arm', or 'simics'." >&2
        TARGET_ARCH="x86_64"
        OVM_PLATFORM="amd"
        DEFAULT_SSH_PORT="2222"
        PROMPT_LABEL="AMD"
        ;;
esac

out_bin="$ORPHANVM_ROOT/output_${TARGET_ARCH}/bin"

export ARCH="$TARGET_ARCH"
export OVM_PLATFORM="$OVM_PLATFORM"
export SSH_PORT="$DEFAULT_SSH_PORT"

if [ ! -d "$out_bin" ]; then
    echo "[OrphanVM] Note: $out_bin does not exist yet (will be used once built)."
fi

case ":$PATH:" in
    *":$out_bin:"*)
        echo "[OrphanVM] $out_bin is already in PATH (ARCH=$TARGET_ARCH, OVM_PLATFORM=$OVM_PLATFORM exported)."
        ;;
    *)
        export PATH="$out_bin:$PATH"
        echo "[OrphanVM] Added to PATH: $out_bin (ARCH=$TARGET_ARCH, OVM_PLATFORM=$OVM_PLATFORM exported)"
        ;;
esac

# Update prompt in interactive shells to easily identify the active environment and architecture
if [ -n "$PS1" ]; then
    PS1="$(echo "$PS1" | sed -E 's/^(\((OVM|ovm):[^)]*\) )+//')"
    export PS1="(ovm:$PROMPT_LABEL) $PS1"
fi
