#!/bin/bash
# Four Swords 4P in RetroArch via the splitscreen core's gba_link_4p subsystem.
#
# Test binds (from /tmp/ra_human.cfg, this session only):
#   P1 Q(up)/A(down)/Z(A)   P2 W/S/X   P3 E/D/C   P4 T/G/B
# Menu flow per player: A at title -> file select confirm (heart) ->
# enter room; host (P1) starts the session. Guests show WAIT during setup.
#
# The appendconfig sets config_save_on_exit=false so nothing leaks into the
# user's main retroarch.cfg (see history.md 2026-09-30 (6) gotcha).
set -e
cd "$(dirname "$0")/../.."   # repo root

ROM="Test Roms/Legend of Zelda, The - A Link To The Past Four Swords (U) [!].gba"
[ -f "$ROM" ] || { echo "FS ROM not found: $ROM" >&2; exit 1; }
[ -f /tmp/ra_human.cfg ] || { echo "missing /tmp/ra_human.cfg (test rig config)" >&2; exit 1; }

env -u WAYLAND_DISPLAY EGL_PLATFORM=x11 XDG_SESSION_TYPE=x11 \
  setsid nohup retroarch \
    --appendconfig /tmp/ra_human.cfg \
    -L "$HOME/.config/retroarch/cores/mgba_splitscreen_libretro.so" \
    --subsystem gba_link_4p "$ROM" "$ROM" "$ROM" "$ROM" \
    < /dev/null > /tmp/ra_fs4p.log 2>&1 &

echo "launched; verifying in 5s (see /tmp/ra_fs4p.log)"
