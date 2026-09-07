#!/bin/bash
# 01-Edu Localhost Siege Stress Test Script
# Subject rule: "Do stress tests with siege -b [IP]:[PORT], it must stay available at all costs (availability should be up to 99.5)"

TARGET_HOST="127.0.0.1"
TARGET_PORT="8080"
DURATION="20S"
CONCURRENT=50

echo "=================================================="
echo "Starting Siege Stress Test on http://$TARGET_HOST:$TARGET_PORT/"
echo "Target Availability Requirement: >= 99.5%"
echo "=================================================="

if ! command -v siege &> /dev/null; then
    echo "Error: 'siege' command not found. Install with: sudo apt install siege"
    exit 1
fi

siege -b -t "$DURATION" -c "$CONCURRENT" "http://$TARGET_HOST:$TARGET_PORT/"
