#!/bin/sh
# Builds an .rpm from a finished build:
#   packaging/rpm.sh VERSION ARCH BUILD_DIR OUT_DIR      (ARCH: x86_64, i686, aarch64)
# The same files as the .deb: mmdbio, its completions and the license. A build for another architecture needs $MMDBIO_HOST, an
# mmdbio this machine runs, to write the completions.
set -eu
version=$1 arch=$2 build=$(cd "$3" && pwd) out=$4
here=$(cd "$(dirname "$0")/.." && pwd)
umask 022
top=$(mktemp -d)
trap 'rm -rf "$top"' EXIT
mkdir -p "$top/SPECS" "$top/RPMS" "$top/host"
ln -s "$(cd "$(dirname "${MMDBIO_HOST:-$build/mmdbio}")" && pwd)/$(basename "${MMDBIO_HOST:-$build/mmdbio}")" "$top/host/mmdbio"
cat > "$top/SPECS/mmdbio.spec" <<'EOF'
Name:           mmdbio
Version:        %{mmdbio_version}
Release:        1
Summary:        Read, write, shrink and compare MMDB files
License:        Apache-2.0
URL:            https://ipgeolocation.io/cli/mmdbio
%global debug_package %{nil}
%global __os_install_post %{nil}

%description
mmdbio looks up IPs in MMDB databases, exports them to JSON, CSV, TSV or
CIDR lists, compares and verifies them, shrinks them losslessly, and builds
them from CSV, TSV, JSON or NDJSON, gzipped or not. One static binary.

%install
mkdir -p %{buildroot}/usr/bin %{buildroot}/usr/share/licenses/mmdbio \
	%{buildroot}/usr/share/bash-completion/completions %{buildroot}/usr/share/zsh/site-functions \
	%{buildroot}/usr/share/fish/vendor_completions.d
install -m 755 %{mmdbio_build}/mmdbio %{buildroot}/usr/bin/mmdbio
%{mmdbio_host}/mmdbio completion bash > %{buildroot}/usr/share/bash-completion/completions/mmdbio
%{mmdbio_host}/mmdbio completion zsh > %{buildroot}/usr/share/zsh/site-functions/_mmdbio
%{mmdbio_host}/mmdbio completion fish > %{buildroot}/usr/share/fish/vendor_completions.d/mmdbio.fish
install -m 644 %{mmdbio_license} %{buildroot}/usr/share/licenses/mmdbio/LICENSE

%files
/usr/bin/mmdbio
/usr/share/bash-completion/completions/mmdbio
/usr/share/zsh/site-functions/_mmdbio
/usr/share/fish/vendor_completions.d/mmdbio.fish
/usr/share/licenses/mmdbio/LICENSE
EOF
rpmbuild -bb --quiet --target "$arch" --define "_topdir $top" --define "mmdbio_version $version" \
	--define "mmdbio_build $build" --define "mmdbio_license $here/LICENSE" --define "mmdbio_host $top/host" \
	"$top/SPECS/mmdbio.spec"
mkdir -p "$out"
rpm=$(find "$top/RPMS" -name '*.rpm' | head -1)
cp "$rpm" "$out/"
echo "$out/$(basename "$rpm")"
