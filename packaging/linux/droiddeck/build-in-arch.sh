#!/usr/bin/bash
# Builds Sunshine for DroidDeck's Arch Linux ARM runtime; run inside menci/archlinuxarm:base-devel with the repo at /work.
set -euxo pipefail
WORK=/work
cd "$WORK"

grep -q '^DisableSandbox' /etc/pacman.conf || sed -i 's/^\[options\]/[options]\nDisableSandbox/' /etc/pacman.conf
{ for m in https://ca.us.mirror.archlinuxarm.org https://fl.us.mirror.archlinuxarm.org https://de3.mirror.archlinuxarm.org https://nl.mirror.archlinuxarm.org; do echo "Server = $m/\$arch/\$repo"; done; cat /etc/pacman.d/mirrorlist; } > /etc/pacman.d/mirrorlist.new
mv /etc/pacman.d/mirrorlist.new /etc/pacman.d/mirrorlist

pacman -Syu --noconfirm --needed \
  git cmake make gcc zstd binutils \
  boost boost-libs nodejs npm python-jinja shaderc \
  avahi curl libcap libdrm libevdev libpipewire libpulse libva \
  libx11 libxcb libxfixes libxrandr libxtst miniupnpc numactl openssl opus \
  systemd-libs vulkan-icd-loader vulkan-headers wayland wayland-protocols mesa

git config --global --add safe.directory '*'
git submodule update --recursive --init --depth 1

export CFLAGS="-O2 -pipe" CXXFLAGS="-O2 -pipe"
# A dated version in Sunshine's own scheme, so the web UI does not offer the upstream release as an update.
export BRANCH=droiddeck BUILD_VERSION="v$(date -u +%Y.%-m%d.%H%M%S)"
cmake -S . -B build -Wno-dev \
  -D CMAKE_BUILD_TYPE=RelWithDebInfo \
  -D CMAKE_INSTALL_PREFIX=/usr/local \
  -D SUNSHINE_EXECUTABLE_PATH=/usr/local/bin/sunshine \
  -D SUNSHINE_ASSETS_DIR=share/sunshine \
  -D BUILD_DOCS=OFF \
  -D BUILD_TESTS=OFF \
  -D SUNSHINE_ENABLE_CUDA=OFF \
  -D SUNSHINE_ENABLE_VULKAN=OFF \
  -D CUDA_FAIL_ON_MISSING=OFF \
  -D SUNSHINE_ENABLE_TRAY=OFF \
  -D SUNSHINE_PUBLISHER_NAME='Pv1E (DroidDeck fork)' \
  -D SUNSHINE_PUBLISHER_WEBSITE='https://github.com/Pv1E/Sunshine' \
  -D SUNSHINE_PUBLISHER_ISSUE_URL='https://github.com/Pv1E/Sunshine/issues'
cmake --build build -j"$(nproc)"

rm -rf out && DESTDIR="$WORK/out" cmake --install build
# The unstripped copy resolves the backtraces printed by libsunshine-crash.so.
cp out/usr/local/bin/sunshine sunshine.debug
strip --strip-unneeded out/usr/local/bin/sunshine* || true

# The runtime has every library Sunshine links except miniupnpc; it ships beside the binary.
mkdir -p out/usr/local/lib/sunshine
cp -L /usr/lib/libminiupnpc.so.* out/usr/local/lib/sunshine/
# Preloaded by the launcher: lists DroidDeck's emulated /dev/video-enc0 for FFmpeg's V4L2 M2M scan.
gcc -O2 -shared -fPIC -Wall -Wextra -o out/usr/local/lib/sunshine/libsunshine-devenc.so packaging/linux/droiddeck/devenc.c -ldl -lpthread
gcc -O2 -shared -fPIC -Wall -Wextra -o out/usr/local/lib/sunshine/libsunshine-crash.so packaging/linux/droiddeck/crash.c

# Libraries the binary needs that the DroidDeck runtime may not have; informational, they are installed with pacman there.
readelf -d out/usr/local/bin/sunshine | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | sort -u > sunshine-needed.txt
if [ -f droiddeck-runtime-sonames.txt ]; then
  grep -vxFf droiddeck-runtime-sonames.txt sunshine-needed.txt > sunshine-missing.txt || true
  echo "not in the runtime:"; cat sunshine-missing.txt
fi
# The pacman packages providing those libraries, to install in the runtime.
for lib in $(cat sunshine-needed.txt); do
  f=$(ls /usr/lib/"$lib" 2>/dev/null | head -1) || continue
  [ -n "$f" ] && pacman -Qoq "$f" 2>/dev/null
done | sort -u > sunshine-pacman-deps.txt
cat sunshine-pacman-deps.txt

(cd out && tar --use-compress-program='zstd -19 -T0' -cf ../sunshine-droiddeck.tzst usr)
sha256sum sunshine-droiddeck.tzst | tee sunshine-droiddeck.tzst.sha256
ls -l sunshine-droiddeck.tzst
