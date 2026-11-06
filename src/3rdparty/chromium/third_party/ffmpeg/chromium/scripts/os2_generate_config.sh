#!/bin/sh
#
# Generates Chromium's FFmpeg configuration for OS/2 (ia32):
#
#   chromium/config/{Chromium,Chrome}/os2/ia32/
#     config.h, [config.asm,] libavutil/avconfig.h, libavutil/ffversion.h,
#     libavcodec/{bsf,codec,parser}_list.c,
#     libavformat/{demuxer,muxer,protocol}_list.c
#
# This mirrors what build_ffmpeg.py + copy_config.sh do for Linux ia32, using
# the same configure flags, so that the Linux ia32 source lists in
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

# Same flags as Chromium's Linux ia32 build (build_ffmpeg.py; also recorded
# in chromium/config/Chrome/linux/ia32/config.h), minus the clang/-m32/PIC
# and nasm settings, plus the OS/2 x86 settings used for system FFmpeg.
COMMON_FLAGS="
  --disable-everything --disable-all --disable-doc --disable-htmlpages
  --disable-manpages --disable-podpages --disable-txtpages --disable-static
  --enable-avcodec --enable-avformat --enable-avutil --enable-fft --enable-rdft
  --enable-static --enable-libopus
  --disable-debug --disable-bzlib --disable-error-resilience --disable-iconv
  --disable-lzo --disable-network --disable-schannel --disable-sdl2
  --disable-symver --disable-xlib --disable-zlib --disable-securetransport
  --disable-faan --disable-alsa --disable-autodetect
  --enable-decoder=vorbis,libopus,flac
  --enable-decoder=pcm_u8,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,mp3
  --enable-decoder=pcm_s16be,pcm_s24be,pcm_mulaw,pcm_alaw
  --enable-demuxer=ogg,matroska,wav,flac,mp3,mov
  --enable-parser=opus,vorbis,flac,mpegaudio,vp9
  --extra-cflags=-I$CHROMIUM_ROOT/third_party/opus/src/include
  --disable-linux-perf
  --optflags=-O2
  --enable-decoder=theora,vp8
  --enable-parser=vp3,vp8
  --arch=i686
  --disable-x86asm
  --disable-avx --disable-avx2 --disable-avx512
"
CHROME_FLAGS="
  --enable-decoder=aac,h264
  --enable-demuxer=aac
  --enable-parser=aac,h264
"

FILES="config.h libavutil/avconfig.h libavcodec/bsf_list.c
  libavcodec/codec_list.c libavcodec/parser_list.c libavformat/demuxer_list.c
  libavformat/muxer_list.c libavformat/protocol_list.c"

status=0
for branding in Chromium Chrome; do
  build_dir="$WORK_DIR/$branding"
  out_dir="$FFMPEG_DIR/chromium/config/$branding/$OS_DIR/$ARCH_DIR"
  ref_dir="$FFMPEG_DIR/chromium/config/$branding/$COMPARE_WITH"
  flags="$COMMON_FLAGS"
  [ "$branding" = Chrome ] && flags="$flags $CHROME_FLAGS"

  echo "=== $branding: configuring in $build_dir"
  mkdir -p "$build_dir"
  # shellcheck disable=SC2086
  (cd "$build_dir" &&
   "$CONFIGURE_SHELL" "$FFMPEG_DIR/configure" $flags $EXTRA_FLAGS \
       > configure.log 2>&1) || {
    echo "configure failed for $branding; see $build_dir/configure.log and" \
         "$build_dir/ffbuild/config.log" >&2
    exit 1
  }

  # Chromium's edits to config.h (build_ffmpeg.py: pre_make_rewrites for
  # ia32 and post_make_rewrites).
  sed -e 's|^\(#define HAVE_EBP_AVAILABLE [01]\)$|/* \1 -- ebp selection is done by the chrome build */|' \
      -e 's|^\(#define FFMPEG_CONFIGURATION .*\)$|/* \1 -- elide long configuration string from binary */|' \
      "$build_dir/config.h" > "$build_dir/config.h.new"
  mv "$build_dir/config.h.new" "$build_dir/config.h"

  for f in $FILES; do
    mkdir -p "$out_dir/$(dirname "$f")"
    cp "$build_dir/$f" "$out_dir/$f"
  done
  # config.asm is only written when x86 assembly is enabled; it's only used
  # by the ffmpeg_nasm target.
  if [ -f "$build_dir/config.asm" ]; then
    cp "$build_dir/config.asm" "$out_dir/config.asm"
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
  grep '^#define CONFIG_' "$ref_dir/config.h" | sort > "$build_dir/ref.cfg"
  grep '^#define CONFIG_' "$out_dir/config.h" | sort > "$build_dir/new.cfg"
  if diff -u "$build_dir/ref.cfg" "$build_dir/new.cfg" > "$build_dir/cfg.diff"; then
    echo "    CONFIG_* matches $COMPARE_WITH"
  else
    echo "    CONFIG_* differs from $COMPARE_WITH (review before using):"
    sed -n 's/^\([-+]#define .*\)$/      \1/p' "$build_dir/cfg.diff"
    status=2
  fi
  for f in libavcodec/codec_list.c libavcodec/parser_list.c \
           libavformat/demuxer_list.c; do
    cmp -s "$ref_dir/$f" "$out_dir/$f" ||
      { echo "    $f differs from $COMPARE_WITH"; status=2; }
  done
done

echo "Done. Scratch files are in $WORK_DIR"
exit $status
