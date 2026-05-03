#!/bin/bash

# Simple launch script for the TCP/UDP Device Monitoring System on Linux

set -e

echo "Starting device monitoring system..."

# Check if binaries exist
if [ ! -f "./server" ]; then
    echo "Error: server binary not found. Run 'make' first."
    exit 1
fi

if [ ! -f "./client" ]; then
    echo "Error: client binary not found. Run 'make' first."
    exit 1
fi

# Start server in background
echo "Launching server..."
./server &
SERVER_PID=$!

# Wait a moment for server to start
sleep 2

# Start a few client instances
echo "Launching clients..."
./client device-01 &
CLIENT1_PID=$!

./client device-02 127.0.0.1 &
CLIENT2_PID=$!

./client device-03 &
CLIENT3_PID=$!

echo "System running. Press Ctrl+C to stop."

# Wait for interrupt
trap "echo 'Stopping...'; kill $SERVER_PID $CLIENT1_PID $CLIENT2_PID $CLIENT3_PID 2>/dev/null; exit" INT
wait