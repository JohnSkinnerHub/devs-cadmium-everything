#!/bin/bash
# Compile-fail tests: Cadmium promises to reject invalid models AT COMPILE TIME. This script checks
# that promise for every snippet in tests/compile_fail/.
#
# Each snippet contains exactly one line guarded by `#ifdef BREAK`, and a first-line comment
#     // EXPECT: <text>
# The script compiles every snippet twice:
#   1. WITHOUT -DBREAK: must compile. (Proves the snippet is valid apart from the broken line, so a
#      failure below cannot be caused by an unrelated typo.)
#   2. WITH    -DBREAK: must NOT compile, and the compiler output must contain <text>, which is the
#      static_assert message written by Cadmium (or the library's own diagnostic).
# Environment: CXX and CXXFLAGS as used by the Makefile (they carry the Cadmium include paths).
set -u
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
dir=tests/compile_fail
pass=0; fail=0
for f in "$dir"/*.cpp; do
    name=$(basename "$f")
    expect=$(sed -n 's|^// EXPECT: *||p' "$f" | head -1)
    if ! $CXX ${CXXFLAGS:-} -I"$dir" -fsyntax-only "$f" >/dev/null 2>/tmp/cf_ok.$$; then
        echo "  [FAIL] $name: the VALID variant does not compile:"; head -5 /tmp/cf_ok.$$; fail=$((fail+1)); continue
    fi
    if $CXX ${CXXFLAGS:-} -I"$dir" -DBREAK -fsyntax-only "$f" >/dev/null 2>/tmp/cf_err.$$; then
        echo "  [FAIL] $name: the BROKEN variant compiled (it should have been rejected)"; fail=$((fail+1)); continue
    fi
    if grep -qF -- "$expect" /tmp/cf_err.$$; then
        echo "  [ ok ] $name -> \"$expect\""; pass=$((pass+1))
    else
        echo "  [FAIL] $name: rejected, but without the expected message \"$expect\""; grep -m3 "error" /tmp/cf_err.$$; fail=$((fail+1))
    fi
done
rm -f /tmp/cf_ok.$$ /tmp/cf_err.$$
echo "compile-fail tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
