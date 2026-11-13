#!/bin/sh
#
# Generates Chromium's FFmpeg configuration for OS/2 (ia32):
#
#   chromium/config/{Chromium,Chrome}/os2/ia32/
#     config.h, [config_components.h,] [config.asm,] libavutil/avconfig.h, libavutil/ffversion.h,
#     libavcodec/{bsf,codec,parser}_list.c,
#     libavformat/{demuxer,muxer,protocol}_list.c
#
# This mirrors what build_ffmpeg.py + copy_config.sh do for Linux ia32, using
# the configure flags recorded in the Linux ia32 config (see FLAGS_FROM), so that the Linux ia32 source lists in
# ffmpeg_generated.gni can be reused for OS/2 (see use_linux_config there).
# x86 assembly (NASM) is disabled for now; see BUILD.gn (has_nasm_deps).
#
# Run on OS/2 from anywhere, with the OS/2 toolchain on PATH:
#
#   dash chromium/scripts/os2_generate_config.sh
#
# Optional environment variables:
#   CONFIGURE_SHELL  shell used to run FFmpeg's configure (default: dash)
#   OS_DIR           name of the config directory to write (default: os2)
#   EXTRA_FLAGS      extra configure flags (e.g. --disable-inline-asm)
#   WORK_DIR         scratch directory for the configure runs
#                    (default: a new temporary directory)
#   FLAGS_FROM       config directory whose recorded configure flags are used
#                    (default: linux/ia32)
#   COMPARE_WITH     config directory to compare CONFIG_* against
#                    (default: linux/ia32)

set -e

FFMPEG_DIR=$(cd "$(dirname "$0")/../.." && pwd)
CHROMIUM_ROOT=$(cd "$FFMPEG_DIR/../.." && pwd)
CONFIGURE_SHELL=${CONFIGURE_SHELL:-dash}
OS_DIR=${OS_DIR:-os2}
ARCH_DIR=ia32
COMPARE_WITH=${COMPARE_WITH:-linux/ia32}
WORK_DIR=${WORK_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/ffmpeg-os2-config.XXXXXX")}

if [ -e "$FFMPEG_DIR/config.h" ]; then
  echo "error: $FFMPEG_DIR/config.h exists; FFmpeg's configure refuses" \
       "out-of-tree builds when the source tree has been configured." >&2
  exit 1
fi

# The configure flags come from the Linux ia32 config.h of the same branding
# (FLAGS_FROM), where Chromium's build_ffmpeg.py records the exact configure
# command line as FFMPEG_CONFIGURATION. Using that instead of a copy keeps
# this script working across Chromium/FFmpeg versions, whose flags differ
# (e.g. --disable-lzo was dropped when FFmpeg removed liblzo support).
#
# From those flags, drop the ones that only apply to Chromium's Linux build
# machines (clang, -m32, PIC, NASM, the machine's opus path and -O2 quoting)
# and add the OS/2 ones.
FLAGS_FROM=${FLAGS_FROM:-linux/ia32}
OS2_FLAGS="
  --extra-cflags=-I$CHROMIUM_ROOT/third_party/opus/src/include
  --optflags=-O2
  --disable-x86asm
  --disable-avx --disable-avx2 --disable-avx512
"

# Prints the configure flags recorded in config.h $1, each single-quoted and
# space-separated, without the Linux-build-machine-only ones.
reference_flags() {
  line=$(sed -n 's/^.*#define FFMPEG_CONFIGURATION "\(.*\)".*$/\1/p' "$1" |
         head -n 1)
  if [ -z "$line" ]; then
    echo "error: no FFMPEG_CONFIGURATION in $1" >&2
    return 1
  fi
  # The C string escapes double quotes.
  line=$(printf '%s\n' "$line" | sed 's/\\"/"/g')
  eval "set -- $line"
  for arg; do
    case $arg in
      --cc=*|--cxx=*|--ld=*|--enable-pic|--enable-lto|--x86asmexe=*) ;;
      --optflags=*|--enable-cross-compile|--target-os=*|--sysroot=*) ;;
      --extra-cflags=*-m32*|--extra-ldflags=*-m32*) ;;
      --extra-cflags=-I*/third_party/opus/src/include) ;;
      *) printf "'%s' " "$arg" ;;
    esac
  done
}

FILES="config.h libavutil/avconfig.h libavcodec/bsf_list.c
  libavcodec/codec_list.c libavcodec/parser_list.c libavformat/demuxer_list.c
  libavformat/muxer_list.c libavformat/protocol_list.c"

status=0
for branding in Chromium Chrome; do
  build_dir="$WORK_DIR/$branding"
  out_dir="$FFMPEG_DIR/chromium/config/$branding/$OS_DIR/$ARCH_DIR"
  ref_dir="$FFMPEG_DIR/chromium/config/$branding/$COMPARE_WITH"
  flags=$(reference_flags \
           "$FFMPEG_DIR/chromium/config/$branding/$FLAGS_FROM/config.h")
  for f in $OS2_FLAGS; do
    flags="$flags '$f'"
  done

  echo "=== $branding: configuring in $build_dir"
  mkdir -p "$build_dir"
  # shellcheck disable=SC2086
  printf '%s\n' "$flags" $EXTRA_FLAGS > "$build_dir/configure.flags"
  (cd "$build_dir" &&
   eval "\"\$CONFIGURE_SHELL\" \"\$FFMPEG_DIR/configure\" $flags" $EXTRA_FLAGS \
       > configure.log 2>&1) || {
    echo "configure failed for $branding; see $build_dir/configure.log and" \
         "$build_dir/ffbuild/config.log" >&2
    echo "--- last lines of configure.log:" >&2
    tail -n 15 "$build_dir/configure.log" >&2
    echo "--- last lines of ffbuild/config.log:" >&2
    tail -n 40 "$build_dir/ffbuild/config.log" >&2
    exit 1
  }

  # Chromium's edits to config.h (build_ffmpeg.py: pre_make_rewrites for
  # ia32 and post_make_rewrites).
  sed -e 's|^\(#define HAVE_EBP_AVAILABLE [01]\)$|/* \1 -- ebp selection is done by the chrome build */|' \
      -e 's|^\(#define FFMPEG_CONFIGURATION .*\)$|/* \1 -- elide long configuration string from binary */|' \
      "$build_dir/config.h" > "$build_dir/config.h.new"
  mv "$build_dir/config.h.new" "$build_dir/config.h"

  # Copy with Unix line endings (tools on OS/2 may write CRLF).
  for f in $FILES; do
    mkdir -p "$out_dir/$(dirname "$f")"
    tr -d '\r' < "$build_dir/$f" > "$out_dir/$f"
  done
  # Newer FFmpeg (e.g. in Chromium 102) moves the component switches
  # (CONFIG_*_DECODER etc.) from config.h into config_components.h.
  if [ -f "$build_dir/config_components.h" ]; then
    tr -d '\r' < "$build_dir/config_components.h" \
        > "$out_dir/config_components.h"
  else
    rm -f "$out_dir/config_components.h"
  fi
  # config.asm is only written when x86 assembly is enabled; it's only used
  # by the ffmpeg_nasm target.
  if [ -f "$build_dir/config.asm" ]; then
    tr -d '\r' < "$build_dir/config.asm" > "$out_dir/config.asm"
  else
    rm -f "$out_dir/config.asm"
  fi
  # ffversion.h only depends on the FFmpeg source revision, which is the same
  # for every platform; reuse the Linux one instead of running make/git.
  mkdir -p "$out_dir/libavutil"
  cp "$ref_dir/libavutil/ffversion.h" "$out_dir/libavutil/ffversion.h"
  echo "    wrote $out_dir"

  # The Linux source lists are only valid if the same components are
  # enabled. Compare CONFIG_* (components and features) with Linux.
  # CONFIG_PIC is expected to differ: OS/2 doesn't build PIC (see BUILD.gn).
  # Include config_components.h where FFmpeg has one.
  cat "$ref_dir/config.h" "$ref_dir"/config_components.h 2>/dev/null |
    grep '^#define CONFIG_' | grep -v '^#define CONFIG_PIC ' |
    sort > "$build_dir/ref.cfg"
  cat "$out_dir/config.h" "$out_dir"/config_components.h 2>/dev/null |
    grep '^#define CONFIG_' | grep -v '^#define CONFIG_PIC ' |
    sort > "$build_dir/new.cfg"
  if diff -u "$build_dir/ref.cfg" "$build_dir/new.cfg" > "$build_dir/cfg.diff"; then
    echo "    CONFIG_* matches $COMPARE_WITH"
  else
    echo "    CONFIG_* differs from $COMPARE_WITH (review before using):"
    sed -n 's/^\([-+]#define .*\)$/      \1/p' "$build_dir/cfg.diff"
    status=2
  fi
  for f in libavcodec/codec_list.c libavcodec/parser_list.c \
           libavformat/demuxer_list.c; do
    if ! cmp -s "$ref_dir/$f" "$out_dir/$f"; then
      echo "    $f differs from $COMPARE_WITH:"
      diff -u "$ref_dir/$f" "$out_dir/$f" | sed -n '3,40s/^/      /p'
      status=2
    fi
  done
done

echo "Done. Scratch files are in $WORK_DIR"
exit $status
