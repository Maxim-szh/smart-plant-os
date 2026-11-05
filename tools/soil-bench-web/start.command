#!/bin/sh
cd "$(dirname "$0")" || exit 1
printf 'Откройте http://localhost:8765 в Chrome или Edge\n'
python3 -m http.server 8765 --bind 127.0.0.1
