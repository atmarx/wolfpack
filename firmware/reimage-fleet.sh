#!/usr/bin/env bash
# reimage-fleet — flash every Wolfpack radio in one sitting.
#
#   firmware/reimage-fleet.sh                 # newest UF2 in firmware/releases, 3 radios
#   firmware/reimage-fleet.sh -n 4 path.uf2   # a different count or image
#
# One radio at a time over USB: plug it in, the script kicks it into the
# UF2 bootloader (1200-baud touch — no double-tapping RESET), copies the
# image onto the drive, and waits for the radio to boot back into the app.
# Then it asks for the next one.  Radios are tracked by USB serial number,
# so plugging the same one in twice doesn't count toward the fleet.
#
# Config (channel, PSK, modem preset, tzdef, names) lives in the nRF52's
# LittleFS and survives an app flash — nothing to reapply afterwards.
#
# USB UF2 only.  Never BLE / NRF-OTA on the L1: that path can brick it.
set -euo pipefail

VID=2886           # Seeed
PID_APP=1668       # TRACKER L1 running Meshtastic (serial port)
PID_BOOT=1667      # UF2 bootloader (mass-storage drive, no serial port)
FLEET=3

usage() { sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
while getopts ":n:h" opt; do
  case $opt in
    n) FLEET=$OPTARG ;;
    h) usage 0 ;;
    *) usage 1 ;;
  esac
done
shift $((OPTIND - 1))

here=$(cd "$(dirname "$0")" && pwd)
UF2=${1:-$(ls -t "$here"/releases/*.uf2 2>/dev/null | head -1)}
[[ -f ${UF2:-} ]] || { echo "No UF2 found (looked in $here/releases)." >&2; exit 1; }

bold=$'\e[1m'; green=$'\e[32m'; yellow=$'\e[33m'; red=$'\e[31m'; dim=$'\e[2m'; off=$'\e[0m'
say()  { printf '%s\n' "$*"; }
step() { printf '  %s…%s\n' "$*" "$off"; }
warn() { printf '  %s%s%s\n' "$yellow" "$*" "$off"; }
die()  { printf '  %s✗ %s%s\n' "$red" "$*" "$off"; return 1; }

# --- USB discovery (sysfs, so it works without lsusb or root) --------------- #

# Print "<sysfs path> <serial>" for each attached device with this product id.
devices() {
  local d
  for d in /sys/bus/usb/devices/*; do
    [[ -f $d/idVendor && $(<"$d/idVendor") == "$VID" && $(<"$d/idProduct") == "$1" ]] || continue
    printf '%s %s\n' "$d" "$(cat "$d/serial" 2>/dev/null || echo unknown)"
  done
}

# The tty belonging to an app-mode USB device.
tty_of() {
  local t
  for t in "$1"/*/tty/tty*; do [[ -e $t ]] && { echo "/dev/$(basename "$t")"; return; }; done
  return 1
}

# The block device (the whole FAT volume) belonging to a bootloader-mode USB device.
disk_of() {
  local b usb
  usb=$(readlink -f "$1")
  for b in /sys/block/sd*; do
    [[ $(readlink -f "$b/device") == "$usb"/* ]] && { echo "/dev/$(basename "$b")"; return; }
  done
  return 1
}

# Wait up to $2 seconds for a device with product id $1; print "<path> <serial>".
wait_for() {
  local i line
  for ((i = 0; i < $2 * 4; i++)); do
    line=$(devices "$1" | head -1)
    [[ -n $line ]] && { echo "$line"; return 0; }
    sleep 0.25
  done
  return 1
}

# --- one radio --------------------------------------------------------------- #

enter_bootloader() {
  local tty=$1
  # Opening the port at 1200 baud and dropping DTR is the Adafruit nRF52
  # bootloader's "reset into UF2" signal.
  if [[ -w $tty ]]; then
    stty -F "$tty" 1200 2>/dev/null && return 0
  fi
  if [[ -n ${SUDO_OK:-} ]]; then
    sudo stty -F "$tty" 1200 2>/dev/null && return 0
  fi
  return 1
}

mount_of() {
  local disk=$1 i mnt part
  # UF2 drives are usually an unpartitioned FAT volume; allow for a partition anyway.
  part=$(lsblk -nlpo NAME,FSTYPE "$disk" 2>/dev/null | awk '$2=="vfat"{print $1; exit}')
  part=${part:-$disk}
  for ((i = 0; i < 24; i++)); do          # give the desktop's automounter a moment
    mnt=$(lsblk -no MOUNTPOINT "$part" 2>/dev/null | head -1)
    [[ -n $mnt ]] && { echo "$mnt"; return 0; }
    sleep 0.25
  done
  udisksctl mount -b "$part" --no-user-interaction >/dev/null 2>&1 || true
  mnt=$(lsblk -no MOUNTPOINT "$part" 2>/dev/null | head -1)
  [[ -n $mnt ]] && echo "$mnt"
}

flash_one() {
  local app=$1 serial=$2 tty boot bootdev disk mnt again i

  if [[ -n $app ]]; then
    tty=$(tty_of "$app") || { die "no serial port for this radio yet"; return 1; }
    step "Resetting into the bootloader via $tty"
    if ! enter_bootloader "$tty"; then
      warn "Couldn't open $tty — double-tap RESET on the radio now."
    fi
    boot=$(wait_for "$PID_BOOT" 20) || { die "radio never showed up as a UF2 drive"; return 1; }
  else
    boot=$(devices "$PID_BOOT" | head -1)    # it was already sitting in the bootloader
  fi
  bootdev=${boot%% *}

  step "Waiting for the UF2 drive"
  disk=""
  for ((i = 0; i < 40; i++)); do disk=$(disk_of "$bootdev") && break; sleep 0.25; done
  [[ -n $disk ]] || { die "bootloader is up but no disk appeared"; return 1; }
  mnt=$(mount_of "$disk") || true
  [[ -n $mnt ]] || { die "couldn't mount $disk (try: udisksctl mount -b $disk)"; return 1; }
  # Belt and braces: only ever write to something that is unmistakably a UF2 bootloader.
  [[ -f $mnt/INFO_UF2.TXT ]] || { die "$mnt has no INFO_UF2.TXT — refusing to write there"; return 1; }

  step "Copying $(basename "$UF2") → $mnt"
  # The bootloader reboots the moment the last block lands, so the drive can
  # vanish under cp/sync.  That's success, not an error — the reboot below is the real check.
  cp "$UF2" "$mnt/" 2>/dev/null || true
  sync 2>/dev/null || true

  step "Waiting for it to boot back up"
  again=""
  for ((i = 0; i < 120; i++)); do          # 30 s
    again=$(devices "$PID_APP" | awk -v s="$serial" '$2==s || s=="" {print; exit}')
    [[ -n $again ]] && break
    sleep 0.25
  done
  if [[ -z $again ]]; then
    if [[ -n $(devices "$PID_BOOT") ]]; then
      die "still in the bootloader — the image didn't take"
    else
      die "didn't come back within 30 s"
    fi
    return 1
  fi
  REBOOTED_SERIAL=${again#* }
}

# --- the fleet loop ---------------------------------------------------------- #

say "${bold}🐺 Wolfpack fleet reimage${off}"
say "  image: $(basename "$UF2")  ${dim}($(sha256sum "$UF2" | cut -c1-12))${off}"
say "  fleet: $FLEET radios"
say ""

if [[ -e /dev/ttyACM0 && ! -w /dev/ttyACM0 ]] || ! id -nG | grep -qw dialout; then
  say "  You're not in the ${bold}dialout${off} group, so resetting a radio into its bootloader"
  say "  needs sudo.  (Permanent fix: ${bold}sudo usermod -aG dialout \$USER${off}, then log back in.)"
  if sudo -v; then SUDO_OK=1; else warn "No sudo — you'll double-tap RESET on each radio instead."; fi
  say ""
fi

declare -a done_serials=()
already() { local s; for s in "${done_serials[@]}"; do [[ $s == "$1" ]] && return 0; done; return 1; }

while ((${#done_serials[@]} < FLEET)); do
  n=$((${#done_serials[@]} + 1))
  say "${bold}Radio $n of $FLEET${off}"

  app=$(devices "$PID_APP" | head -1)
  boot=$(devices "$PID_BOOT" | head -1)
  if [[ -z $app && -z $boot ]]; then
    printf '  Plug it in, then press Enter. '; read -r
    app=$(wait_for "$PID_APP" 10 || true)
    [[ -z $app ]] && boot=$(devices "$PID_BOOT" | head -1)
    if [[ -z $app && -z $boot ]]; then warn "Don't see a Tracker L1 — check the cable (a charge-only cable won't show up)."; continue; fi
  fi
  if (($(devices "$PID_APP" | wc -l) + $(devices "$PID_BOOT" | wc -l) > 1)); then
    warn "More than one radio is plugged in — leave just one and press Enter."; read -r; continue
  fi

  serial=""
  if [[ -n $app ]]; then
    serial=${app#* }
    say "  found TRACKER L1 ${dim}$serial${off}"
    if already "$serial"; then
      warn "Already did this one.  Unplug it, plug in a different radio, press Enter."; read -r; continue
    fi
  else
    warn "This radio is already sitting in the bootloader, so I can't read its serial until it reboots."
  fi

  REBOOTED_SERIAL=""
  if flash_one "${app%% *}" "$serial"; then
    serial=${serial:-$REBOOTED_SERIAL}
    if already "$serial"; then
      warn "That turned out to be a radio I'd already done ($serial) — flashed it again, not counting it twice."
    else
      done_serials+=("$serial")
      say "  ${green}✓ radio $n flashed and booted ${dim}($serial)${off}"
    fi
  else
    warn "Radio $n failed.  Unplug, replug, and press Enter to try it again."; read -r; continue
  fi

  if ((${#done_serials[@]} < FLEET)); then
    say ""
    printf '  Unplug it, plug in the next radio, then press Enter. '; read -r
    # Make sure the one we just did is actually gone before hunting for the next.
    while devices "$PID_APP" | awk -v s="$serial" '$2==s{f=1} END{exit !f}'; do
      printf '  %sStill see %s — unplug it, then press Enter.%s ' "$yellow" "$serial" "$off"; read -r
    done
  fi
  say ""
done

say ""
say "${green}${bold}All $FLEET radios reimaged${off} with $(basename "$UF2"):"
for s in "${done_serials[@]}"; do say "  ✓ $s"; done
