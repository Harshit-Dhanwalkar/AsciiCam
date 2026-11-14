# Homebrew formula. Publish it in a tap repository named homebrew-asciicam:
#   github.com/Harshit-Dhanwalkar/homebrew-asciicam/Formula/asciicam.rb
# Users then run:  brew install Harshit-Dhanwalkar/asciicam/asciicam
#
# Update url and sha256 for each tag:
#   curl -L https://github.com/Harshit-Dhanwalkar/AsciiCam/archive/refs/tags/v0.1.0.tar.gz | shasum -a 256
class Asciicam < Formula
  desc "Webcam to ASCII art in the terminal, no libc and no dependencies"
  homepage "https://github.com/Harshit-Dhanwalkar/AsciiCam"
  url "https://github.com/Harshit-Dhanwalkar/AsciiCam/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "PolyForm-Noncommercial-1.0.0"
  head "https://github.com/Harshit-Dhanwalkar/AsciiCam.git", branch: "main"

  # The renderer talks to the Linux kernel directly
  depends_on arch: :x86_64

  def install
    cd "C" do
      system "make", "install", "PREFIX=#{prefix}"
    end
  end

  test do
    assert_match "Usage", shell_output("#{bin}/asciicam --help")
    # --demo needs no camera
    assert_predicate share/"asciicam/plugins/invert.so", :exist?
  end
end
