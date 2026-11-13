#!/bin/sh
# ODYSSEY-AGC: enforce the GUI/engine boundary (docs/AUDIO-ENGINE.md, "Control and
# monitoring API").  GUI-facing sources may use ONLY the control/monitoring calls;
# they must never touch a ring, the producer API (push/sink/active/watermark/
# prefill/latency), the consumer API (render), or the engine's lifetime calls.
#
#   check_gui_api.sh            check the real GUI sources (odyssey_gui*, odyssey_dsky*, gui/)
#   check_gui_api.sh FILE...    check the given files
#   check_gui_api.sh --selftest prove the checker flags violations and accepts the API
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"

forbidden='(\.|->)ring\b|(\.|->)slots\b|(\.|->)(head|tail)\b|(\.|->)chan\[|ody_rt_(push|push_timed|sink|render|set_active|set_watermark|publish_watermark|set_prefill|set_latency_cycles|frames_ready|init|destroy)\b|ody_rt_slot_t|ody_rt_ring_t'

scan() { # scan FILE...  -> prints violations, returns 1 if any
  bad=0
  for f in "$@"; do
    [ -f "$f" ] || continue
    if grep -nE "$forbidden" "$f" > "${TMPDIR:-/tmp}/gui_api.$$" 2>/dev/null; then
      sed "s|^|VIOLATION $f:|" "${TMPDIR:-/tmp}/gui_api.$$"; bad=1
    fi
  done
  rm -f "${TMPDIR:-/tmp}/gui_api.$$"
  return $bad
}

if [ "${1:-}" = "--selftest" ]; then
  d=$(mktemp -d); fails=0
  cat > "$d/good.c" <<'EOT'
ody_rt_set_gain_q15 (rt, 0, g); ody_rt_set_mute (rt, 1, 1); ody_rt_set_solo (rt, 0, 0);
ody_rt_set_master_q15 (rt, m); x = ody_rt_get_gain_q15 (rt, 0); ody_rt_get_stats (rt, 0, &st);
p = ody_rt_read_peak (rt, 1); n = ody_rt_rendered_frames (rt); c = ody_rt_clipped_frames (rt);
EOT
  scan "$d/good.c" > /dev/null || { echo "selftest FAIL: the allowed API was flagged"; fails=1; }
  for bad in 'x = rt->chan[0].ring.head;' 'ody_rt_push (rt, 0, 1);' 'r->slots[0].pcm = 1;' \
             'ody_rt_render (rt, b, 64, 2);' 'ody_rt_set_active (rt, 0, 1);' 'ody_rt_sink (c, s);' \
             'ody_rt_publish_watermark (rt, 5);' 'ody_rt_ring_t *r;' 'ody_rt_destroy (rt);' 'y = c.ring.tail;'; do
    echo "$bad" > "$d/bad.c"
    scan "$d/bad.c" > /dev/null && { echo "selftest FAIL: not flagged: $bad"; fails=1; }
  done
  rm -rf "$d"
  [ $fails = 0 ] && echo "check_gui_api selftest: allowed API accepted, 10 violation kinds flagged"
  exit $fails
fi

if [ $# -gt 0 ]; then files="$*"; else
  files=$(ls odyssey_gui*.c odyssey_gui*.h odyssey_dsky*.c odyssey_dsky*.h gui/* 2>/dev/null)
fi
if [ -z "$files" ]; then
  echo "check_gui_api: no GUI sources present (nothing to check yet)"; exit 0
fi
# shellcheck disable=SC2086
if scan $files; then echo "check_gui_api: $(echo $files | wc -w) file(s) use only the control/monitoring API"; exit 0; fi
echo "check_gui_api: GUI sources must not touch rings or the producer/consumer API"; exit 1
