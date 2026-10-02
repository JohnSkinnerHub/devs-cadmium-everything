#!/bin/bash
# API coverage audit: does this project exercise EVERYTHING Cadmium provides?
#
# docs/cadmium_api.txt lists every public item of Cadmium (see the format at the top of that file).
# For each line this script checks:
#   used      the identifier occurs as a whole word in src/ or include/greenhouse/ (or in the given file)
#   via       the evidence regex matches in the evidence file (the public call that drives a private helper)
#   excluded  a reason is given (items that cannot be used in this Cadmium revision; see README)
# and, in the other direction, that EVERY header under $CADMIUM/include/cadmium is listed, so an
# upstream header added later is flagged until someone decides how to cover it.
#
# Usage: CADMIUM=/path/to/cadmium tools/coverage_check.sh
set -u
cd "$(dirname "$0")/.."
CADMIUM=${CADMIUM:-../Cadmium-Simulation-Environment/cadmium}
list=docs/cadmium_api.txt
used=0; via=0; excluded=0; bad=0

while IFS='|' read -r header id status evidence; do
    case "$header" in ''|\#*) continue ;; esac
    if [ ! -f "$CADMIUM/include/cadmium/$header" ]; then
        echo "  [FAIL] $header is listed but does not exist in $CADMIUM (stale list?)"; bad=$((bad+1)); continue
    fi
    case "$status" in
        used)
            if [ -n "$evidence" ]; then targets=("$evidence"); else targets=(src include/greenhouse); fi
            if grep -rqw -- "$id" "${targets[@]}" 2>/dev/null; then used=$((used+1))
            else echo "  [FAIL] $header: '$id' is never used in ${targets[*]}"; bad=$((bad+1)); fi ;;
        via)
            file=${evidence%%::*}; regex=${evidence#*::}
            if [ -f "$file" ] && grep -qE -- "$regex" "$file"; then via=$((via+1))
            else echo "  [FAIL] $header: '$id' - evidence /$regex/ not found in $file"; bad=$((bad+1)); fi ;;
        excluded)
            if [ -n "$evidence" ]; then excluded=$((excluded+1)); echo "  [skip] $header: $id: $evidence"
            else echo "  [FAIL] $header: '$id' is excluded without a reason"; bad=$((bad+1)); fi ;;
        *) echo "  [FAIL] $header: '$id' has unknown status '$status'"; bad=$((bad+1)) ;;
    esac
done < "$list"

# reverse direction: every Cadmium header must appear in the list
missing=0
while read -r h; do
    rel=${h#"$CADMIUM/include/cadmium/"}
    if ! grep -q "^$rel|" "$list"; then echo "  [FAIL] header $rel is not in $list"; missing=$((missing+1)); fi
done < <(find "$CADMIUM/include/cadmium" -name '*.hpp' | sort)
headers=$(find "$CADMIUM/include/cadmium" -name '*.hpp' | wc -l)

echo "API coverage: $used used + $via via another call + $excluded excluded (documented), $headers/$headers headers listed" \
     "$( [ $((bad+missing)) -eq 0 ] && echo '-> COMPLETE' || echo "-> $((bad+missing)) PROBLEM(S)" )"
[ $((bad+missing)) -eq 0 ]
