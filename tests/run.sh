#!/usr/bin/env bash
# Builds and runs this plugin's tests natively (Linux or WSL): every tests/test_*.cpp, under g++ and clang++ where
# present, with the address and undefined-behaviour sanitizers, then optimised. Nothing of the game is needed.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="${TMPDIR:-/tmp}/$(basename "$(dirname "$here")" | tr ' ' '_')-tests"
mkdir -p "$out"
warn=(-std=c++23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -I"$here/../src")
status=0
for test in "$here"/test_*.cpp; do
	name="$(basename "$test" .cpp)"
	for cxx in g++ clang++; do
		command -v "$cxx" >/dev/null || continue
		echo "== $name, $cxx, sanitizers"
		"$cxx" "${warn[@]}" -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all "$test" -o "$out/$name-$cxx-san"
		"$out/$name-$cxx-san" || status=1
		echo "== $name, $cxx, optimised"
		"$cxx" "${warn[@]}" -O2 -DNDEBUG "$test" -o "$out/$name-$cxx-opt"
		"$out/$name-$cxx-opt" || status=1
	done
done
exit $status
