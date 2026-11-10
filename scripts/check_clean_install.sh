#!/usr/bin/env bash
# Check that README section 1 works from nothing: runs the bash blocks of README
# 1.2, 1.3 and 1.4 (taken from README.md itself, not copied) in an EMPTY home
# directory with an empty environment, then one test as a smoke check (1.6).
# It clones the repository from GitHub, so it checks what was pushed.
#   scripts/check_clean_install.sh /path/to/empty/dir          # install + one smoke test
#   scripts/check_clean_install.sh --all /path/to/empty/dir    # install + every test (grader view)
# Not covered: README 1.1 (apt packages, needs root) -- those must already be installed.
set -euo pipefail
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd -P)"
TESTS="--test TC_019_CS"
if [ "${1:-}" = "--all" ]; then TESTS="--all --markdown"; shift; fi
CLEAN="${1:?usage: $0 [--all] /path/to/empty/dir}"
mkdir -p "$CLEAN"
[ -z "$(ls -A "$CLEAN")" ] || { echo "$CLEAN is not empty"; exit 1; }
CLEAN="$(cd "$CLEAN" && pwd -P)"
python3 - "$REPO_DIR/README.md" "$TESTS" > "$CLEAN/steps.sh" <<'PY'
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
print("set -euo pipefail\nset -x")
for sec in ("1.2", "1.3", "1.4"):
    m = re.search(r"^### " + re.escape(sec) + r"\b.*?(?=^### |\Z)", text, re.S | re.M)
    if not m:
        sys.exit(f"README section {sec} not found")
    print(f"echo '===== README {sec} ====='")
    print("\n".join(re.findall(r"```bash\n(.*?)```", m.group(0), re.S)))
print("echo '===== tests (README 1.6) ====='")
# a failing test is a result, not an install failure: report it in the summary instead
print("cd ~/starkoff-ws/starkoff && git log --oneline -1 && { scripts/run_lab.sh --qemu --quiet " + sys.argv[2] +
      " || echo '(some tests did not pass, see summary)'; }")
print("echo '===== CLEAN-INSTALL OK ====='")
PY
for p in 7000 9000; do
  if ss -ltn 2>/dev/null | grep -q ":$p "; then echo "port $p is in use, stop other QEMU/lab first"; exit 1; fi
done
echo "running README in empty HOME=$CLEAN (log: $CLEAN/run.log)"
env -i HOME="$CLEAN" PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    LANG=C.UTF-8 TERM=dumb bash "$CLEAN/steps.sh" > "$CLEAN/run.log" 2>&1 \
  && { sed -n '/===== summary/,$p' "$CLEAN/run.log" | grep -E "^(TC|X)_" | awk '{print $2}' | sort | uniq -c; tail -2 "$CLEAN/run.log"; } \
  || { echo "FAILED, last lines:"; tail -30 "$CLEAN/run.log"; exit 1; }
B="$CLEAN/starkoff-ws/starkoff/build/CMakeCache.txt"
echo "SDK used:    $(sed -n 's/^ZEPHYR_SDK_INSTALL_DIR:PATH=//p' "$B")"
echo "Zephyr used: $(git -C "$CLEAN/starkoff-ws/zephyr" rev-parse --short HEAD) (v4.2.0 = 413b789)"
