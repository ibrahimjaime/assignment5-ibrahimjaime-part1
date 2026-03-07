#!/bin/sh
filesdir=$1
searchstr=$2
if [ $# != 2 ]
then
    echo "Error: mising arguments 1) filesdir - 2) searchstr"
    exit 1
fi

if [ ! -d "$filesdir" ]
then
    echo "Error: path didn't exist"
    exit 1
fi

matchCount=$(grep -r "$searchstr" "$filesdir" | wc -l)
fileCount=$(find $filesdir -type f | wc -l)

echo "The number of files are $fileCount and the number of matching lines are $matchCount"
exit 0