#!/bin/sh
# Keep ./st -e tools/bench.sh working; exec preserves st as Python's parent.
exec python3 "$(dirname -- "$0")/bench.py" "$@"
