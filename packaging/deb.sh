#!/bin/sh
# Builds a .deb from a finished build:
#   packaging/deb.sh VERSION ARCH BUILD_DIR OUT_DIR      (ARCH: amd64, i386, arm64)
# The package holds /usr/bin/mmdbio, the shell completions and the license. The maintainer is $DEB_MAINTAINER, else the
# git user of this checkout. A build for another architecture needs
# $MMDBIO_HOST, an mmdbio this machine runs, to write the completions.
set -eu
version=$1 arch=$2 build=$3 out=$4
here=$(cd "$(dirname "$0")/.." && pwd)
maintainer=${DEB_MAINTAINER:-}
if [ -z "$maintainer" ]; then
	gname=$(git -C "$here" config user.name 2> /dev/null || true)
	gmail=$(git -C "$here" config user.email 2> /dev/null || true)
	maintainer=${gname:-mmdbio}${gmail:+ <$gmail>}
fi
umask 022
root=$(mktemp -d)
chmod 755 "$root"
trap 'rm -rf "$root"' EXIT

install -d "$root/DEBIAN" "$root/usr/bin" "$root/usr/share/doc/mmdbio" \
	"$root/usr/share/bash-completion/completions" "$root/usr/share/zsh/vendor-completions" \
	"$root/usr/share/fish/vendor_completions.d"
install -m 755 "$build/mmdbio" "$root/usr/bin/mmdbio"
host=${MMDBIO_HOST:-$build/mmdbio}
"$host" completion bash > "$root/usr/share/bash-completion/completions/mmdbio"
"$host" completion zsh > "$root/usr/share/zsh/vendor-completions/_mmdbio"
"$host" completion fish > "$root/usr/share/fish/vendor_completions.d/mmdbio.fish"
install -m 644 "$here/LICENSE" "$root/usr/share/doc/mmdbio/copyright"
size=$(du -sk "$root/usr" | cut -f1)
cat > "$root/DEBIAN/control" <<EOF
Package: mmdbio
Version: $version
Architecture: $arch
Maintainer: $maintainer
Installed-Size: $size
Section: utils
Priority: optional
Homepage: https://ipgeolocation.io/cli/mmdbio
Description: read, write, shrink and compare MMDB files
 mmdbio looks up IPs in MMDB databases, exports them to JSON, CSV, TSV or
 CIDR lists, compares and verifies them, shrinks them losslessly, and builds
 them from CSV, TSV, JSON or NDJSON, gzipped or not. One static binary.
EOF
mkdir -p "$out"
dpkg-deb --root-owner-group --build "$root" "$out/mmdbio_${version}_${arch}.deb" > /dev/null
echo "$out/mmdbio_${version}_${arch}.deb"
