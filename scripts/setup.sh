#!/usr/bin/env bash
set -euo pipefail

echo "=== M5Stack Intelligent Agent - Setup ==="

# Check for Homebrew
if ! command -v brew &> /dev/null; then
    echo "Error: Homebrew is required. Install from https://brew.sh"
    exit 1
fi

# Install PlatformIO CLI
echo "--- Installing PlatformIO CLI ---"
if ! command -v pio &> /dev/null; then
    brew install platformio
else
    echo "PlatformIO already installed"
fi

# Install Ollama
echo "--- Installing Ollama ---"
if ! command -v ollama &> /dev/null; then
    brew install ollama
else
    echo "Ollama already installed"
fi

# Pull a small LLM model
echo "--- Pulling LLM model (llama3.2:3b) ---"
ollama pull llama3.2:3b

# Set up Python virtual environment for the server
echo "--- Setting up Python environment ---"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SERVER_DIR="$PROJECT_DIR/server"

cd "$SERVER_DIR"

if [ ! -d "venv" ]; then
    python3 -m venv venv
fi
source venv/bin/activate

pip install --upgrade pip
pip install -r requirements.txt

# Download Piper voice model
echo "--- Downloading Piper TTS voice ---"
VOICES_DIR="$SERVER_DIR/voices"
mkdir -p "$VOICES_DIR"

VOICE_NAME="en_US-lessac-medium"
if [ ! -f "$VOICES_DIR/${VOICE_NAME}.onnx" ]; then
    echo "Downloading voice model: $VOICE_NAME"
    curl -L "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/${VOICE_NAME}.onnx" \
        -o "$VOICES_DIR/${VOICE_NAME}.onnx"
    curl -L "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/${VOICE_NAME}.onnx.json" \
        -o "$VOICES_DIR/${VOICE_NAME}.onnx.json"
else
    echo "Voice model already downloaded"
fi

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Next steps:"
echo "  1. Edit firmware/config.h with your WiFi credentials and Mac IP address"
echo "  2. Start Ollama:     ollama serve"
echo "  3. Start the server: cd server && source venv/bin/activate && python server.py"
echo "  4. Build firmware:   pio run"
echo "  5. Flash M5Stack:    pio run -t upload"
