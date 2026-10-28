#!/bin/sh
# Deploy a staged CV610 waybeam bundle to a RUNNING craft, for bring-up.
#
# Scope: this is a bring-up helper, NOT the distribution path.  A CV610 image
# is meant to install waybeam from the firmware package
# (general/package/waybeam in OpenIPC/firmware), which owns the file layout
# and the init wiring.  This script exists so the daemon and, when the bundle
# carries one, a rebuilt sensor plugin can be exercised on a craft that has no
# such package yet, and it retires with that track.  Read it as scaffolding,
# not as the way waybeam reaches a camera.
#
# The IMX335 plugin is not in this repository: it is the OpenIPC/openhisilicon
# driver the hisilicon-opensdk package installs.  Deploying it is therefore
# not this script's business, and a bundle without it still deploys.
#
# Like its siblings scripts/star6e_direct_deploy.sh and
# scripts/maruko_direct_deploy.sh it deliberately does NOT install
# init.d/S95waybeam or restart the streamer: on a stock OpenIPC image the boot
# streamer (majestic) should first come up with the new sensor plugin so the
# driver itself can be validated, and the waybeam daemon is started
# afterwards.  It backs up the stock plugin once, then uploads.
#
# Binary payloads go over the wire as base64.  A plain `cat file | ssh` of the
# ~100 KB sensor .so truncates on this link, which yields a half-written .so
# and a sensor that never inits; the base64 hop is not a style choice.
#
# Every upload lands in a sibling temp file in the DESTINATION directory and
# is renamed into place only after its checksum matches what we sent.  That is
# the invariant this script keeps: the live artifact is never written
# through, so a truncated transfer, a full filesystem or an interrupted
# session cannot leave a half-written .so behind -- it leaves the previous
# file exactly as it was.  A sibling temp rather than /tmp keeps the rename
# inside one filesystem, which is what makes it atomic (a /tmp -> /usr move is
# copy+unlink), and it removes the fixed temp name that a second concurrent
# deploy, or anything that can write to a world-writable directory on the
# craft, could meet with a symlink.
#
# Usage:
#   scripts/cv610_direct_deploy.sh <camera-ip> [bundle-dir]
#
# Env:
#   SSH_BIN   ssh command to use (default: ssh).  Point it at a wrapper if the
#             craft only accepts password auth.
#   SSH_OPTS  extra ssh options (default: StrictHostKeyChecking=no + timeout)
#   SENSOR_OVERRIDE  deploy the plugin for this part instead of asking the craft
#             (for a board whose u-boot env names the wrong sensor).
set -eu

CAM=${1:?usage: $0 <camera-ip> [bundle-dir]}
BUNDLE=${2:-out/cv610}

SSH_BIN=${SSH_BIN:-ssh}
SSH_OPTS=${SSH_OPTS:--o StrictHostKeyChecking=no -o ConnectTimeout=8}
REMOTE="$SSH_BIN $SSH_OPTS root@$CAM"

# Which sensor does this craft run?  There is no config field to read any more
# (waybeam resolves the part from the system -- see docs/CV610_BACKEND.md
# § Sensor selection), so ask the craft the same question the daemon asks.
# Deploying a plugin for the other part leaves the ISP bound to a sensor that
# is not there, so the fallback only fires when the craft cannot answer.
SENSOR=${SENSOR_OVERRIDE:-}
if [ -z "$SENSOR" ]; then
	SENSOR=$($REMOTE 'fw_printenv -n sensor 2>/dev/null || ipcinfo --short-sensor 2>/dev/null' \
		2>/dev/null | tr -d '\r' | awk 'NF { print; exit }' || true)
fi
SENSOR=${SENSOR:-imx662}
SO="libsns_${SENSOR}.so"

[ -f "$BUNDLE/waybeam" ] || { echo "ERROR: no $BUNDLE/waybeam (run make stage first)" >&2; exit 1; }
[ -f "$BUNDLE/waybeam.json" ] || { echo "ERROR: no $BUNDLE/waybeam.json" >&2; exit 1; }

# A missing plugin is not an error any more: IMX335 is not built in this
# repository, its .so comes from the image's hisilicon-opensdk package, and
# overwriting that with nothing would be the bug.  IMX662 still ships here, so
# a bundle that carries it is uploaded as before.
SEND_SO=0
if [ -f "$BUNDLE/sensors/$SO" ]; then
	SEND_SO=1
fi

echo "== deploy to $CAM: sensor=$SENSOR, plugin=$SO $([ "$SEND_SO" = 1 ] && echo '(from bundle)' || echo '(from image)') =="

# Keep one copy of the stock plugin so a bad bring-up is a single reboot back.
# An absent source is not an error: a second run finds its own upload there,
# and the backup already holds the original.
$REMOTE 'set -e
	b=/root/sensor-backup; mkdir -p $b
	d=/usr/lib/sensors
	if [ -f "$d/'"$SO"'" ]; then
		if [ -e "$b/'"$SO"'.stock" ]; then
			echo "  stock backup kept: $(md5sum "$b/'"$SO"'.stock" | cut -d" " -f1)"
		else
			cp -a "$d/'"$SO"'" "$b/'"$SO"'.stock"
			echo "  stock backup made: $(md5sum "$b/'"$SO"'.stock" | cut -d" " -f1)"
		fi
	else
		echo "  no $d/'"$SO"' on the craft; nothing to back up"
	fi'

# Runs on the craft with the payload on stdin.  argv: <dst> <mode> <md5>.
# No single quotes in here: the whole snippet is passed inside '...' below.
# `cleanup` as a function, not an inline string, so the trap needs no quoting.
REMOTE_UPLOAD='
dst=$1; mode=$2; want=$3
dir=${dst%/*}
tmp=$(mktemp "$dir/.wb_up.XXXXXX") || { echo "ERROR: cannot create a temp file in $dir" >&2; exit 1; }
cleanup() { rm -f "$tmp"; }
trap cleanup EXIT
trap "exit 1" INT TERM HUP
base64 -d > "$tmp" || exit 1
got=$(md5sum "$tmp" | cut -d" " -f1)
if [ "$got" != "$want" ]; then
	echo "ERROR: checksum mismatch for $dst (sent $want, got $got); previous file left in place" >&2
	exit 1
fi
chmod "$mode" "$tmp" || exit 1
mv -f "$tmp" "$dst" || exit 1
echo "    ok $dst ($want)"
'

upload() { # src dst mode
	src=$1
	dst=$2
	mode=$3
	want=$(md5sum "$src" | cut -d' ' -f1)
	echo "  upload $src -> $dst"
	base64 "$src" | $REMOTE "sh -c '$REMOTE_UPLOAD' wb-up '$dst' '$mode' '$want'"
}

upload "$BUNDLE/waybeam"              /usr/bin/waybeam            0755
[ "$SEND_SO" = 1 ] && \
	upload "$BUNDLE/sensors/$SO"  /usr/lib/sensors/$SO        0644
upload "$BUNDLE/waybeam.json"         /etc/waybeam.json           0644
[ -f "$BUNDLE/waybeam-cv610.conf" ] && \
	upload "$BUNDLE/waybeam-cv610.conf" /etc/waybeam-cv610.conf  0644

$REMOTE 'sync'
echo "  deployed and checksummed. Reboot to make the kernel reload the sensor plugin cleanly:"
echo "    $SSH_BIN $SSH_OPTS root@$CAM reboot"
if [ "$SEND_SO" = 1 ]; then
	echo "  Revert: cp /root/sensor-backup/$SO.stock /usr/lib/sensors/$SO && reboot"
else
	echo "  Revert: reinstall the device package that owns /usr/lib/sensors/$SO"
fi
