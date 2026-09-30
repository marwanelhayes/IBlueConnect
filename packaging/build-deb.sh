#!/bin/sh
# Assemble only inside the matching Ubuntu build environment; never install/start services.
set -eu
[ "$#" -eq 4 ] || { echo 'Usage: build-deb.sh 24.04|26.04 daemon ui rnnoise-plugin' >&2; exit 1; }
ubuntu=$1
daemon=$2
ui=$3
plugin=$4
case "$ubuntu" in 24.04|26.04) ;; *) echo 'Unsupported Ubuntu release.' >&2; exit 1 ;; esac
. /etc/os-release
[ "$ID" = ubuntu ] && [ "$VERSION_ID" = "$ubuntu" ] || {
    echo 'Build each package inside its matching Ubuntu release.' >&2; exit 1;
}
[ "$(dpkg --print-architecture)" = amd64 ] || { echo 'Only amd64 builds are supported.' >&2; exit 1; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=$(sed -n 's/^version = "\([0-9][0-9.]*\)"$/\1/p' "$root/Cargo.toml")
[ -n "$version" ] || { echo 'Missing app version.' >&2; exit 1; }
version="$version-1ubuntu$ubuntu"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
stage="$work/package"
lib="$stage/usr/lib/iblueconnect"
doc="$stage/usr/share/doc/iblueconnect"
mkdir -p "$lib" "$doc" "$stage/DEBIAN" "$work/debian" "$root/target/releases"
install -m755 "$daemon" "$lib/iblueconnectd"
install -m755 "$ui" "$lib/iblueconnect"
install -m644 "$plugin" "$lib/librnnoise_ladspa.so"
strip --strip-unneeded "$lib/iblueconnectd" "$lib/iblueconnect" "$lib/librnnoise_ladspa.so"
for script in ofono-enable.sh ofono-rollback.sh rnnoise.sh first-run.sh setup-user.py; do
    install -m755 "$root/packaging/$script" "$lib/$script"
done
python3 -m venv --without-pip "$lib/setup-venv"
# A relocatable private interpreter; activation scripts contain temporary build paths.
rm -f "$lib/setup-venv/bin/activate" "$lib/setup-venv/bin/activate.csh" \
    "$lib/setup-venv/bin/activate.fish" "$lib/setup-venv/bin/Activate.ps1"
rm -f "$lib/setup-venv/bin/python" "$lib/setup-venv/bin/python3" "$lib/setup-venv/bin"/python3.*
ln -s /usr/bin/python3 "$lib/setup-venv/bin/python3"
cat > "$lib/setup-venv/pyvenv.cfg" <<'EOF'
home = /usr/bin
include-system-site-packages = false
executable = /usr/bin/python3
EOF
install -Dm755 "$root/packaging/iblueconnect-launcher.sh" "$stage/usr/bin/iblueconnect"
install -Dm644 "$root/packaging/systemd/iblueconnectd-package.service" "$stage/usr/lib/systemd/user/iblueconnectd.service"
install -Dm644 "$root/packaging/desktop/org.iblueconnect.IBlueConnect.desktop" "$stage/usr/share/applications/org.iblueconnect.IBlueConnect.desktop"
printf '%s\n' "$ubuntu" > "$lib/ubuntu-version"
printf '%s\n' "$version" > "$lib/version"
upstream="$root/target/rnnoise/noise-suppression-for-voice-1.10"
install -m644 "$upstream/LICENSE" "$doc/noise-suppression-for-voice-LICENSE"
install -m644 "$upstream/external/rnnoise/COPYING" "$doc/RNNoise-COPYING"
cat > "$doc/copyright" <<'EOF'
IBlueConnect source: https://github.com/marwanelhayes/IBlueConnect
noise-suppression-for-voice v1.10 (LADSPA plugin):
 https://github.com/werman/noise-suppression-for-voice/tree/v1.10
 Its GPL-3.0 license and RNNoise copyright notice accompany this package.
 Matching upstream source is distributed with the GitHub release.
EOF
cat > "$work/debian/control" <<'EOF'
Source: iblueconnect
Section: utils
Priority: optional
Maintainer: IBlueConnect contributors <noreply@github.com>

Package: iblueconnect
Architecture: amd64
Description: Bluetooth phone calling for Ubuntu
EOF
shlibs=$(cd "$work" && dpkg-shlibdeps -O -e"$lib/iblueconnectd" -e"$lib/iblueconnect" -e"$lib/librnnoise_ladspa.so")
case "$shlibs" in shlibs:Depends=*) shlibs=${shlibs#shlibs:Depends=} ;; *) echo 'Missing native shared-library dependencies.' >&2; exit 1 ;; esac
installed=$(du -sk "$stage/usr" | cut -f1)
cat > "$stage/DEBIAN/control" <<EOF
Package: iblueconnect
Version: $version
Architecture: amd64
Maintainer: IBlueConnect contributors <noreply@github.com>
Section: utils
Priority: optional
Installed-Size: $installed
Depends: $shlibs, ofono, bluez, bluez-obexd, pipewire, wireplumber, pipewire-bin, libspa-0.2-bluetooth, jq, python3, zenity, pkexec, gnome-session-canberra, libcanberra-pulse
Homepage: https://github.com/marwanelhayes/IBlueConnect
X-IBlueConnect-Ubuntu: $ubuntu
Description: Bluetooth phone calling for Ubuntu $ubuntu
 Quiet background call controller, contacts and call history over Bluetooth,
 with optional call-only RNNoise microphone filtering. First launch requests
 permission before changing per-user audio configuration or starting services.
EOF
archive="$root/target/releases/iblueconnect_${version}_amd64.deb"
dpkg-deb --root-owner-group -Zxz --build "$stage" "$archive"
metadata_python="$root/.venv/bin/python3"
if [ ! -x "$metadata_python" ]; then
    python3 -m venv --without-pip "$work/venv"
    metadata_python="$work/venv/bin/python3"
fi
"$metadata_python" - "$archive" "$ubuntu" "$version" "$installed" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
archive, ubuntu, version, installed = sys.argv[1:]
archive = Path(archive)
metadata = {"file": archive.name, "ubuntu": ubuntu, "architecture": "amd64",
            "version": version, "download_bytes": archive.stat().st_size,
            "installed_kib": int(installed),
            "sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
(archive.parent / f"ubuntu-{ubuntu}-amd64.json").write_text(json.dumps(metadata, indent=2) + "\n")
print(json.dumps(metadata))
PY
