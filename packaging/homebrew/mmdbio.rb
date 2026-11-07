# Homebrew formula: copy it to a tap (e.g. a homebrew-tap repository's
# Formula/ directory) and set url and sha256 to the release's source archive;
# build-release.sh prints the sha256 of the archive it can see.
class Mmdbio < Formula
  desc "Read, write, shrink and compare MMDB files"
  homepage "https://ipgeolocation.io/cli/mmdbio"
  url "https://github.com/IPGeolocation/mmdbio/archive/refs/tags/v2.0.0.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "Apache-2.0"

  def install
    # a dynamic binary: Homebrew on Linux has no static libc
    system "make", "CC=#{ENV.cc}", "LDFLAGS="
    bin.install "build/mmdbio"
    generate_completions_from_executable(bin/"mmdbio", "completion")
  end

  test do
    assert_match "mmdbio version", shell_output("#{bin}/mmdbio --version")
    (testpath/"ranges.csv").write("network,kind\n8.8.8.0/24,dns\n")
    system bin/"mmdbio", "import", "ranges.csv", "ranges.mmdb", "-q"
    assert_match "dns", shell_output("#{bin}/mmdbio read 8.8.8.8 ranges.mmdb")
    assert_match "[OK]", shell_output("#{bin}/mmdbio shrink ranges.mmdb --lookups 100 --bench=false")
  end
end
