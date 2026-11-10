#!/bin/sh
# Installs a released mmdbio for this machine:
#
#   curl -fsSL https://raw.githubusercontent.com/IPGeolocation/mmdbio/main/install.sh | sh
#
# MMDBIO_VERSION    the version (default: the latest release)
# MMDBIO_DIR        where the binaries go (default: /usr/local/bin when it is
#                   writable, else ~/.local/bin)
# MMDBIO_REPO       the GitHub repository (default IPGeolocation/mmdbio)
#
# The archive is checked against the release's SHA256SUMS before anything
# is installed.
set -eu

repo=${MMDBIO_REPO:-IPGeolocation/mmdbio}
fail() {
	echo "mmdbio install: $*" >&2
	exit 1
}

case $(uname -s) in
Linux) os=linux ;;
Darwin) os=darwin ;;
FreeBSD) os=freebsd ;;
OpenBSD) os=openbsd ;;
NetBSD) os=netbsd ;;
DragonFly) os=dragonfly ;;
*) fail "no release build for $(uname -s); build from source (make) instead" ;;
esac
case $(uname -m) in
x86_64 | amd64) arch=amd64 ;;
aarch64 | arm64) arch=arm64 ;;
armv7l | armv7 | armv8l) arch=armv7 ;;
i386 | i486 | i586 | i686) arch=386 ;;
*) fail "no release build for $(uname -m); build from source (make) instead" ;;
esac

if command -v curl > /dev/null 2>&1; then
	get() { curl -fsSL "$1" -o "$2"; }
	get_text() { curl -fsSL "$1"; }
elif command -v wget > /dev/null 2>&1; then
	get() { wget -q "$1" -O "$2"; }
	get_text() { wget -q -O - "$1"; }
else
	fail "needs curl or wget"
fi

version=${MMDBIO_VERSION:-}
if [ -z "$version" ]; then
	version=$(get_text "https://api.github.com/repos/$repo/releases/latest" |
		sed -n 's/.*"tag_name": *"v\{0,1\}\([^"]*\)".*/\1/p' | head -1)
	[ -n "$version" ] || fail "can't find the latest release of $repo"
fi
version=${version#v}

name=mmdbio-$version-$os-$arch
base=https://github.com/$repo/releases/download/v$version
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "downloading $name"
get "$base/$name.tar.gz" "$tmp/$name.tar.gz" || fail "no $name.tar.gz in release v$version"
get "$base/SHA256SUMS" "$tmp/SHA256SUMS" || fail "no SHA256SUMS in release v$version"
want=$(grep " $name.tar.gz\$" "$tmp/SHA256SUMS" | cut -d' ' -f1)
[ -n "$want" ] || fail "$name.tar.gz is not in SHA256SUMS"
if command -v sha256sum > /dev/null 2>&1; then
	got=$(sha256sum "$tmp/$name.tar.gz" | cut -d' ' -f1)
else
	got=$(shasum -a 256 "$tmp/$name.tar.gz" | cut -d' ' -f1)
fi
[ "$got" = "$want" ] || fail "checksum mismatch for $name.tar.gz"
tar -xzf "$tmp/$name.tar.gz" -C "$tmp"

dir=${MMDBIO_DIR:-}
if [ -z "$dir" ]; then
	if [ -w /usr/local/bin ]; then dir=/usr/local/bin; else dir=$HOME/.local/bin; fi
fi
mkdir -p "$dir"
cp "$tmp/$name/mmdbio" "$dir/mmdbio.new"
chmod 755 "$dir/mmdbio.new"
mv -f "$dir/mmdbio.new" "$dir/mmdbio"
echo "installed $("$dir/mmdbio" --version) to $dir"
case ":$PATH:" in
*":$dir:"*) ;;
*) echo "note: $dir is not in your PATH" ;;
esac
echo "shell completion: $dir/mmdbio completion install"
