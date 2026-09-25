#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go, in the order that
# fails fastest.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   build         a FRESH universal Release build. Not the dev build: CMake
#                 latches the architecture list at the first target, so the
#                 only build worth measuring is one configured from nothing.
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out. The text compiled here is
#                 what `jqtest --dump-shaders` writes: the exact strings the
#                 plugin hands the driver.
#   demo          the browser demo's copy of every shader, and of the loom's
#                 constants, ranges, swatches and option names, is still the
#                 plugin's, character for character (demo/tools/check_shaders.py).
#                 Says nothing about the page's hand PORT of the CPU half
#                 (demo/loom.js); only a reader checks that.
#   names         every parameter name 16 characters or fewer, and unique.
#   optimal       every pick the loom weaves costs exactly what an exhaustive
#                 search over colour and pattern finds. No GL.
#   checks        every picture check, at TWO rasters: 320x180, which is what
#                 CI renders at, and 1280x720. A check that holds at one
#                 raster was fitted to it:
#                   --floats     no float past Max Float, grid and picture
#                   --coverage   each structure's weft ratio, exactly
#                   --twill      the diagonal at atan( pick h / end w )
#                   --shuttle    one weft colour a pick, a shuttle's
#                   --distance   from a distance, nearer the picture than
#                                any single colour
#                   --resize     the shuttles and the loom's memory survive
#                   --alpha      the cloth is opaque; Mix blends all of RGBA
#                   --ties       over a black ground the ties scatter (v0.1.1)
#                   --negative   every one of those FAILS on a perturbed model
#   software      the same checks at 320x180 on Apple's SOFTWARE renderer,
#                 which is what GitHub's macOS runners have and which is not
#                 repeatable at the last bit (JQTEST_RENDERER=software).
#   pipe          the fleet's --pipe contract: whole frames only, a cue naming
#                 no parameter refused, a failed render is exit 1, a closed
#                 stdout is exit 1 (SIGPIPE ignored, never 141), and options
#                 STEP between cues rather than ramping through their values
#   sweep         does every control change the picture
#   bench         the render cost, for the record. Not pass/fail.
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the build really universal.
#   plist         does CFBundleExecutable name the binary that is on disk.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         a real FFGL host loads the bundle and reports the name, id
#                 and type it sees -- the name field is not null-terminated
#                 and a host truncates silently past 16 characters.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

step "build (fresh universal Release, $BUILD)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

JQTEST="$BUILD/jqtest"

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails. No shaders at all is a FAILURE: it means the dump broke.
# glslc is a 4.50 compiler and Mesa is the one that refused `packed` (atrac),
# so a grep for the GLSL 4.10 reserved words the fleet has tripped on runs too.
#---------------------------------------------------------------------------
step "shaders"
dir="$( mktemp -d )"
"$JQTEST" --dump-shaders "$dir" >/dev/null
n=0; bad=0
for shader in "$dir"/*.vert "$dir"/*.frag; do
	[ -e "$shader" ] || continue
	n=$(( n + 1 ))
	if command -v glslc >/dev/null 2>&1 \
	   && ! glslc --target-env=opengl4.5 -fauto-map-locations "$shader" -o /dev/null 2>"$dir/err"; then
		printf '   %s does not compile\n' "$( basename "$shader" )"
		sed "s|$dir/||; s|^|      |" "$dir/err"
		bad=$(( bad + 1 ))
	fi
	# Comments stripped; `layout(` is the one reserved word the vertex shader
	# is meant to use.
	if sed 's|//.*$||' "$shader" | grep -nwE 'patch|sample|input|output|filter|common|active|half|flat|packed|superp|noperspective' \
	   >"$dir/words"; then
		printf '   %s uses a GLSL reserved word as a name:\n' "$( basename "$shader" )"
		sed 's/^/      /' "$dir/words"
		bad=$(( bad + 1 ))
	fi
done
rm -rf "$dir"
if [ "$n" -ne 3 ]; then
	fail "$n shaders were dumped, expected 3 -- the dump has gone stale"
elif [ "$bad" -eq 0 ]; then
	if command -v glslc >/dev/null 2>&1; then pass "all $n shaders compile, no reserved word used as a name"
	else pass "no reserved word used as a name (glslc not installed: brew install shaderc)"; fi
else
	fail "$bad shader problem(s)"
fi

#---------------------------------------------------------------------------
# The browser demo's copy of every shader, and of the constants, ranges,
# swatches and option names its loom port reads, is the plugin's, character
# for character. A drifted comment counts. A change here means
# `python3 demo/tools/sync_shaders.py`, never a hand edit of demo/. It says
# nothing about the page's PORT of the CPU half; only a reader checks that.
#---------------------------------------------------------------------------
step "demo shaders"
if [ -f demo/tools/check_shaders.py ]; then
	if out=$(python3 demo/tools/check_shaders.py 2>&1); then
		pass "$( printf '%s\n' "$out" | tail -1 )"
	else
		fail "the demo's shaders or constants have drifted from source/ -- run: python3 demo/tools/sync_shaders.py"
		printf '%s\n' "$out" | tail -12
	fi
else
	printf '   skipped: no demo/\n'
fi

step "names"
if out=$("$JQTEST" --names 2>&1); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "jqtest --names"
	printf '%s\n' "$out" | sed 's/^/      /'
fi

step "optimal (no GL)"
if out=$("$JQTEST" --optimal 2>&1); then
	pass "jqtest --optimal: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
	printf '%s\n' "$out" | grep -E '^optimal, |greedy' | sed 's/^/        /'
else
	fail "jqtest --optimal"
	printf '%s\n' "$out" | tail -5 | sed 's/^/      /'
fi

CHECKS="floats coverage twill shuttle distance resize alpha ties negative"

for size in 320x180 1280x720; do
	step "checks at $size"
	for check in $CHECKS; do
		if out=$("$JQTEST" --$check --size $size 2>&1); then
			pass "jqtest --$check: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
		else
			fail "jqtest --$check at $size"
			printf '%s\n' "$out" | sed 's/^/      /'
		fi
	done
done

#---------------------------------------------------------------------------
# The same checks on Apple's software renderer, which is what CI's macOS
# runner falls back to and which rounds differently from this Mac's GPU.
#---------------------------------------------------------------------------
step "software renderer at 320x180"
for check in $CHECKS; do
	if out=$(JQTEST_RENDERER=software "$JQTEST" --$check --size 320x180 2>&1); then
		pass "jqtest --$check (software): $( printf '%s\n' "$out" | grep -v '^$' | grep -v '^jqtest:' | tail -1 )"
	else
		fail "jqtest --$check on the software renderer -- run: JQTEST_RENDERER=software $JQTEST --$check --size 320x180"
		printf '%s\n' "$out" | sed 's/^/      /'
	fi
done

#---------------------------------------------------------------------------
# --pipe, in the fleet's frame format.
#---------------------------------------------------------------------------
step "pipe"
frame=$(( 64 * 36 * 4 ))
raw=$( mktemp ); many=$( mktemp ); cues=$( mktemp ); outs=$( mktemp ); still=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
head -c $(( frame * 40 )) /dev/zero > "$many"

# Output to a file and the status taken straight from jqtest: after
# `got=$( a | b )` PIPESTATUS describes the assignment, not a, and only
# pipefail would have made the old form mean anything.
"$JQTEST" --pipe --size 64x36 < "$raw" > "$outs" 2>/dev/null
status=$?
got=$( wc -c < "$outs" | tr -d ' ' )
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi

# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever jqtest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
"$JQTEST" --pipe --size 64x36 --script "$cues" < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then
	pass "a cue naming no parameter is refused (exit 2)"
else
	fail "a cue naming no parameter gave exit $status, not 2"
fi

"$JQTEST" --pipe --size 64x36 --fail-render-at 1 < "$raw" > "$outs" 2>/dev/null
status=$?
got=$( wc -c < "$outs" | tr -d ' ' )
if [ "$status" -eq 1 ] && [ "$got" = "$frame" ]; then
	pass "a failed render at frame 1: exit 1, one frame out"
else
	fail "a failed render at frame 1 gave exit $status and $got bytes (want 1 and $frame)"
fi

# A reader that goes away after one byte: forty frames is far more than a
# pipe buffer holds, so the writes after head leaves must fail. Exit 1, said
# on stderr -- not the 141 of a process SIGPIPE killed before it could say
# anything. PIPESTATUS[0] is jqtest's own status, not head's.
"$JQTEST" --pipe --size 64x36 < "$many" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ]; then
	pass "a closed stdout (| head -c 1): the harness exits 1, not 141"
else
	fail "a closed stdout gave exit $status, not 1"
fi

# Options step between cues. Structure goes Auto (0) at frame 0 to Twill (2)
# at frame 10: stepped, frames 5 and 9 are still Auto and equal frame 1;
# ramped, frame 5 would be 1.0, which is Plain. Twelve copies of one still
# gradient, 16 ends so a crossing is 4 px, fixed Mono shuttles so nothing
# but the cue can move the cloth after the first frame (frame 0 has no
# memory yet, so the comparison starts at 1).
python3 -c "
import sys
w, h = 64, 36
row = bytearray()
for y in range(h):
    for x in range(w):
        v = (x * 255) // (w - 1)
        row += bytes((v, (y * 255) // (h - 1), 255 - v, 255))
sys.stdout.buffer.write(bytes(row) * 12)
" > "$still"
printf '0 Ends 16\n0 Palette 2\n0 Shuttles 4\n0 Structure 0\n10 Structure 2\n' > "$cues"
"$JQTEST" --pipe --size 64x36 --script "$cues" < "$still" > "$outs" 2>/dev/null
at() { tail -c +$(( frame * $1 + 1 )) "$outs" | head -c $frame | shasum | cut -c1-16; }
f1=$( at 1 ); f5=$( at 5 ); f9=$( at 9 ); f10=$( at 10 )
if [ "$f1" = "$f5" ] && [ "$f1" = "$f9" ] && [ "$f9" != "$f10" ]; then
	pass "an option steps between cues: Structure holds Auto through frame 9, Twill at 10"
else
	fail "an option did not step between cues (frames 1/5/9/10: $f1 $f5 $f9 $f10)"
fi
rm -f "$raw" "$many" "$cues" "$outs" "$still"

step "sweep"
for size in 320x180 480x270; do
	if out=$(python3 tools/sweep.py --binary "$JQTEST" --size $size 2>/dev/null); then
		pass "$size: $( printf '%s\n' "$out" | tail -1 )"
	else
		fail "tools/sweep.py reports a dead control at $size"
		printf '%s\n' "$out" | grep -E '^DEAD|DEAD CONTROLS' | sed 's/^/      /'
	fi
done

step "bench (for the record)"
"$JQTEST" --bench --frames 30 2>&1 | sed -n '3,10p' | sed 's/^/   /'

BUNDLE="$BUILD/Jacquard.bundle"
BIN="$BUNDLE/Contents/MacOS/Jacquard"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	version=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	if [ "$ident" = "com.stoatworks.ffgl.jacquard" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi
	about=$(sed -n 's/.*versionFallback = "v\([^"]*\)".*/\1/p' source/StoatworksAbout.h)
	if [ "$version" = "0.1.1" ] && [ "$about" = "0.1.1" ]; then
		pass "version 0.1.1 in the plist and in StoatworksAbout.h"
	else
		fail "version: plist '$version', StoatworksAbout.h '$about'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Jacquard.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-$HOME/Projects/resolume/oxbow/build/oxbow}"
	if [ -x "$OXBOW" ]; then
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		for want in "name:        SW Jacquard" "id:          JQ01" "type:        effect"; do
			case "$probe" in
				*"$want"*) pass "host sees '$want'" ;;
				*) fail "host does not see '$want' -- see: $OXBOW probe $BUNDLE" ;;
			esac
		done
		self=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$self" in
			*"selftest:    PASS"*) pass "instantiates through plugMain and renders 120 frames" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
