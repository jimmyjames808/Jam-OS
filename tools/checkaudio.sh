#!/bin/sh
# make check: no audio file is tracked by git (music and recordings stay
# out of the repository; tests generate their own under build/). Exit 1
# naming any that is.
found=$(git ls-files | grep -i -E '\.(mp3|wav|flac|m4a|aac|ogg|opus|aiff?|wma)$')
if [ -n "$found" ]; then
    echo "checkaudio: audio files are tracked (they must not be):"
    echo "$found"
    exit 1
fi
echo "checkaudio: no audio files tracked"
