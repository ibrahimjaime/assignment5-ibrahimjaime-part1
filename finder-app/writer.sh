#!/bin/sh
writefile="$1"
writestr="$2"
if [ "$#" != 2 ]
then
    echo "statements"
    exit 1
fi

dirpath=$(dirname "$writefile")
mkdir -p "$dirpath" || { echo "directory could not be created"; exit 1; }

echo "$writestr" > "$writefile" 2>/dev/null || { echo "file could not be created"; exit 1; }

exit 0