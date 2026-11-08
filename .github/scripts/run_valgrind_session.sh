#!/usr/bin/env bash
# Runs the app under valgrind for DURATION_MIN minutes while keeping it busy,
# then shuts it down cleanly (SIGTERM) so valgrind prints the leak summary.
#
# env: SCENARIO=window|background   DURATION_MIN=30   BIN=build/dictionary   OUT=valgrind-logs
set -u
SCENARIO="${SCENARIO:-window}"
DURATION_MIN="${DURATION_MIN:-30}"
BIN="$(readlink -f "${BIN:-build/dictionary}")"
OUT="$(mkdir -p "${OUT:-valgrind-logs}" && readlink -f "${OUT:-valgrind-logs}")"
LOG="$OUT/valgrind-$SCENARIO.log"
XML="$OUT/valgrind-$SCENARIO.xml"
APPLOG="$OUT/app-$SCENARIO.log"
CLIENTLOG="$OUT/client-$SCENARIO.log"

export G_SLICE=always-malloc G_DEBUG=gc-friendly
export GSK_RENDERER=cairo GDK_BACKEND=x11 GTK_A11Y=none NO_AT_BRIDGE=1
export LIBGL_ALWAYS_SOFTWARE=1

WORDS=(time people water light house garden river mountain silence journey bridge window
       memory shadow harvest compass lantern meadow anchor whisper horizon thunder mirror
       pebble orchard lantern velvet ember canvas marble breeze)

stamp() { date -u +%H:%M:%S; }
echo "[$(stamp)] scenario=$SCENARIO duration=${DURATION_MIN}min bin=$BIN" | tee "$OUT/session-$SCENARIO.txt"

ARGS=()
[ "$SCENARIO" = background ] && ARGS+=(--background)

valgrind \
  --tool=memcheck --leak-check=full --show-leak-kinds=definite,indirect,possible \
  --track-origins=no --num-callers=40 --error-limit=no --child-silent-after-fork=yes \
  --suppressions="${SUPP:-$(dirname "$0")/../valgrind.supp}" \
  --log-file="$LOG" --xml=yes --xml-file="$XML" \
  "$BIN" "${ARGS[@]}" -v >"$APPLOG" 2>&1 &
VG_PID=$!

# Wait for the app to claim its bus name (valgrind startup is slow).
for _ in $(seq 1 180); do
  kill -0 "$VG_PID" 2>/dev/null || { echo "app exited during startup"; break; }
  dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
    org.freedesktop.DBus.NameHasOwner string:dev.silvanus.dictionary 2>/dev/null | grep -q "boolean true" && break
  sleep 1
done
sleep 15   # let the database / word list finish loading under valgrind

END=$(( $(date +%s) + DURATION_MIN * 60 ))
n=0
while [ "$(date +%s)" -lt "$END" ] && kill -0 "$VG_PID" 2>/dev/null; do
  n=$((n + 1))
  w="${WORDS[$((RANDOM % ${#WORDS[@]}))]}"
  case $((n % 4)) in
    0) # random word shortcut (Ctrl+R)
       xdotool key --clearmodifiers ctrl+r >>"$CLIENTLOG" 2>&1 || true ;;
    1|2) # lookup through the single-instance command line (same path as tray / popup)
       timeout 60 "$BIN" --look-up "$w" >>"$CLIENTLOG" 2>&1 || true ;;
    3) # type into the search box and clear it (completion + live search)
       xdotool type --delay 40 "$w" >>"$CLIENTLOG" 2>&1 || true
       xdotool key ctrl+a BackSpace >>"$CLIENTLOG" 2>&1 || true ;;
  esac
  [ "$SCENARIO" = window ] || sleep 20   # background mode: mostly idle service
  sleep 10
  if [ $((n % 6)) -eq 0 ]; then
    rss=$(awk '/VmRSS/{print $2}' /proc/"$VG_PID"/status 2>/dev/null)
    echo "[$(stamp)] iteration=$n valgrind_rss_kb=${rss:-?}" | tee -a "$OUT/session-$SCENARIO.txt"
  fi
done

echo "[$(stamp)] test period over, shutting the app down" | tee -a "$OUT/session-$SCENARIO.txt"
kill -TERM "$VG_PID" 2>/dev/null
for _ in $(seq 1 300); do kill -0 "$VG_PID" 2>/dev/null || break; sleep 1; done
if kill -0 "$VG_PID" 2>/dev/null; then
  echo "app did not exit in 5 min; killing (leak report will be incomplete)" | tee -a "$OUT/session-$SCENARIO.txt"
  kill -KILL "$VG_PID" 2>/dev/null
fi
wait "$VG_PID" 2>/dev/null
echo "[$(stamp)] valgrind finished, exit=$?" | tee -a "$OUT/session-$SCENARIO.txt"
