#!/bin/sh
# Continuously write a timestamp and message
while true; do
    echo "$(date '+%Y-%m-%d %H:%M:%S') - Alpine echo works!" >> /tmp/echo-output.txt
    sleep 1
done