#!/bin/sh
# Builds release files into dist/, testing each build it can run first:
#
#   mmdbio-<version>-<os>-<arch>.tar.gz   mmdbio, libmmdbio.a, mmdbio.h,
#                                         README, LICENSE
#   mmdbio-<version>-windows-<arch>.zip   the same for Windows
#   mmdbio_<version>_<arch>.deb, mmdbio-<version>-1.<arch>.rpm   (Linux)
#   SHA256SUMS                            every file above
#
# It builds for the machine it runs on. On Linux amd64 it also builds, when
# the compilers are installed (each tested where it can run here):
#
#   linux-386      $CC32 (default i686-linux-gnu-gcc, else "gcc -m32"), GCC 14+
#                  for _BitInt (gcc-multilib conflicts with the ARM cross
#                  compilers on Debian and Ubuntu; gcc-i686-linux-gnu does not)
#   linux-arm64    aarch64-linux-gnu-gcc, tested under qemu-aarch64
#   linux-armv7    clang (GCC has no _BitInt on 32-bit ARM), tested under qemu-arm
#   windows-amd64  x86_64-w64-mingw32-gcc, tested under wine
#   windows-386    clang with the i686 mingw-w64 runtime, tested under wine
#
# .deb and .rpm files need dpkg-deb and rpmbuild. Linux binaries are fully
# static. .github/workflows/release.yml runs this on each platform.
set -eu
cd "$(dirname "$0")"
here=$(pwd)
version=$(sed -n 's/^#define MMDBIO_VERSION "\(.*\)"/\1/p' src/cmds.h)
os=$(uname -s | tr '[:upper:]' '[:lower:]')
arch=$(uname -m)
case "$arch" in
x86_64 | amd64) arch=amd64 ;;
aarch64 | arm64) arch=arm64 ;;
i386 | i686) arch=386 ;;
esac
command -v gmake > /dev/null 2>&1 && MAKE=gmake || MAKE=make
mkdir -p dist
dist=$here/dist
rm -f dist/SHA256SUMS

have() { command -v "$1" > /dev/null 2>&1; }
# an llvm tool, plain or versioned (llvm-ar, llvm-ar-21)
llvm() {
	if have "$1"; then echo "$1"; return; fi
	ls /usr/bin/"$1"-[0-9]* 2> /dev/null | sort -V | tail -1
}

# pack BUILD_DIR OS ARCH: the archive of one build
pack() {
	b=$1 o=$2 a=$3
	name=mmdbio-$version-$o-$a
	stage=$(mktemp -d)
	mkdir "$stage/$name"
	cp README.md LICENSE include/mmdbio.h "$stage/$name/"
	[ -f "$b/libmmdbio.a" ] && cp "$b/libmmdbio.a" "$stage/$name/"
	if [ "$o" = windows ]; then
		cp "$b/mmdbio.exe" "$stage/$name/"
		rm -f "dist/$name.zip"
		(cd "$stage" && python3 -m zipfile -c "$dist/$name.zip" "$name")
	else
		cp "$b/mmdbio" "$stage/$name/"
		tar -C "$stage" -czf "dist/$name.tar.gz" "$name"
	fi
	rm -rf "$stage"
}

# packages BUILD_DIR ARCH: .deb and .rpm of a Linux build ($MMDBIO_HOST
# writes the completions when the build doesn't run here)
packages() {
	b=$1 a=$2
	deb=$a rpm=$a
	case $a in
	amd64) rpm=x86_64 ;;
	386) deb=i386 rpm=i686 ;;
	arm64) rpm=aarch64 ;;
	armv7) deb=armhf rpm=armv7hl ;;
	esac
	if have dpkg-deb; then sh packaging/deb.sh "$version" "$deb" "$b" dist; fi
	if have rpmbuild; then sh packaging/rpm.sh "$version" "$rpm" "$b" dist; fi
}

# check BUILD_DIR RUNNER: the unit tests, the library test and the whole
# end-to-end suite, through RUNNER (qemu-*, wine, or nothing)
check() {
	b=$1 run=$2
	w=$(mktemp -d)
	ext=
	[ "$run" = wine ] && ext=.exe
	p=$w/mmdbio
	case $run in
	wine) printf '#!/bin/sh\nWINEDEBUG=-all exec wine "%s/%s/mmdbio.exe" "$@"\n' "$here" "$b" > "$p" ;;
	"") ln -s "$here/$b/mmdbio" "$p" ;;
	*) printf '#!/bin/sh\nexec %s "%s/%s/mmdbio" "$@"\n' "$run" "$here" "$b" > "$p" ;;
	esac
	[ -L "$p" ] || chmod +x "$p"
	mkdir -p "$w/lib"
	if [ "$run" = wine ]; then
		WINEDEBUG=-all wine "$b/unit-tests.exe"
		WINEDEBUG=-all wine "$b/lib-test.exe" "$w/lib"
	else
		$run "$b/unit-tests$ext"
		$run "$b/lib-test$ext" "$w/lib"
	fi
	sh tests/run_tests.sh "$w/mmdbio"
	rm -rf "$w"
}

# cross NAME OS ARCH RUNNER MAKE_ARGS...: build, test when RUNNER is here, pack
cross() {
	name=$1 o=$2 a=$3 run=$4
	shift 4
	b=build-release-$name
	rm -rf "$b"
	ext=
	[ "$o" = windows ] && ext=.exe
	$MAKE BUILD="$b" "$@" all lib "$b/unit-tests$ext" "$b/lib-test$ext"
	if [ -z "$run" ] || have "$run"; then
		check "$b" "$run"
	else
		echo "$name built but not tested: $run is not installed"
	fi
	pack "$b" "$o" "$a"
	[ "$o" = linux ] && MMDBIO_HOST=$here/build-release-$os-$arch/mmdbio packages "$b" "$a"
	return 0
}

b=build-release-$os-$arch
rm -rf "$b"
$MAKE BUILD="$b" all lib
$MAKE BUILD="$b" test
pack "$b" "$os" "$arch"
[ "$os" = linux ] && packages "$b" "$arch"

if [ "$os" = linux ] && [ "$arch" = amd64 ]; then
	t='typedef unsigned _BitInt(128) t; int main(void){return 0;}'
	if [ -z "${CC32:-}" ] && have i686-linux-gnu-gcc; then CC32=i686-linux-gnu-gcc; fi
	cc32=${CC32:-gcc -m32}
	if echo "$t" | $cc32 -std=c2x -x c -o /dev/null - 2> /dev/null; then
		cross linux-386 linux 386 "" CC="$cc32"
	else
		echo "skipping linux-386: $cc32 can't build with _BitInt (GCC 14+ is needed)"
	fi
	if have aarch64-linux-gnu-gcc; then
		cross linux-arm64 linux arm64 qemu-aarch64 CC=aarch64-linux-gnu-gcc
	else
		echo "skipping linux-arm64: aarch64-linux-gnu-gcc is not installed"
	fi
	llvm_tools="LIBAR=$(llvm llvm-ar) NM=$(llvm llvm-nm) OBJCOPY=$(llvm llvm-objcopy)"
	if have clang && have arm-linux-gnueabihf-gcc; then
		# gcc-arm-linux-gnueabihf's linker, named: clang before 21 looks for an
		# armv7-linux-gnueabihf-ld and falls back to the host's x86 ld (the flag
		# is unused when only compiling, which -Werror would stop on)
		armcc="clang --target=armv7-linux-gnueabihf --ld-path=$(command -v arm-linux-gnueabihf-ld)"
		armcc="$armcc -Wno-unused-command-line-argument"
		# shellcheck disable=SC2086
		cross linux-armv7 linux armv7 qemu-arm CC="$armcc" $llvm_tools
	else
		echo "skipping linux-armv7: clang and gcc-arm-linux-gnueabihf (for its libc) are needed"
	fi
	if have x86_64-w64-mingw32-gcc; then
		cross windows-amd64 windows amd64 wine CC=x86_64-w64-mingw32-gcc
	else
		echo "skipping windows-amd64: x86_64-w64-mingw32-gcc is not installed"
	fi
	if have clang && [ -d /usr/i686-w64-mingw32 ]; then
		# mingw's binutils: llvm-objcopy can't hide names in COFF objects
		mingw_tools="LIBAR=i686-w64-mingw32-ar NM=i686-w64-mingw32-nm OBJCOPY=i686-w64-mingw32-objcopy"
		have i686-w64-mingw32-objcopy || mingw_tools=$llvm_tools
		# shellcheck disable=SC2086
		cross windows-386 windows 386 wine CC="clang --target=i686-w64-mingw32" $mingw_tools
	else
		echo "skipping windows-386: clang and the i686 mingw-w64 runtime are needed"
	fi
fi

cd dist
for f in *.tar.gz *.zip *.deb *.rpm; do
	[ -f "$f" ] || continue
	if have sha256sum; then sha256sum "$f"; else shasum -a 256 "$f"; fi
done > SHA256SUMS
cat SHA256SUMS
