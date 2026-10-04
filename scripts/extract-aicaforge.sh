#!/bin/sh
# Only the NEW destination clone is rewritten. Never invoke filter-repo in AICAflow.
set -eu
if [ "$#" -ne 3 ]; then
    echo "usage: $0 AICAFLOW_REPOSITORY SOURCE_TAG NEW_DESTINATION" >&2
    exit 2
fi
source_repo=$1
source_tag=$2
destination=$3
test ! -e "$destination" || { echo "Destination already exists: $destination" >&2; exit 1; }
git filter-repo --version >/dev/null
paths=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/aicaforge-paths.txt
git clone --no-local --no-tags --single-branch --branch "$source_tag" "$source_repo" "$destination"
git -C "$destination" switch -c aicaforge-extraction
git -C "$destination" filter-repo --paths-from-file "$paths"
git -C "$destination" branch -M main
echo "Extracted history at $destination; no remote has been published."
echo "Inspect git log --follow -- src/afx_compile_c.c and git blame src/afx_compile_c.c."
