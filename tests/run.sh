#!/usr/bin/env bash
#
# End to end test: starts a small JVM, dumps its classes and checks the result.
#
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d /tmp/jvmdump-test.XXXXXX)
pid=""

cleanup() {
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT

if ! command -v javac >/dev/null; then
    echo "SKIP: javac not found" >&2
    exit 0
fi

javac -d "$work/classes" "$root/tests/DemoApp.java"
java -cp "$work/classes" demo.DemoApp >"$work/app.log" 2>&1 &
pid=$!

for _ in $(seq 1 100); do
    grep -q DEMO_PID "$work/app.log" 2>/dev/null && break
    sleep 0.1
done

if ! grep -q DEMO_PID "$work/app.log"; then
    echo "FAIL: demo JVM did not start" >&2
    cat "$work/app.log" >&2
    exit 1
fi

echo "== full dump =="
"$root/bin/jvmdump" -o "$work/dump" "$pid"
test -f "$work/dump/classes/demo/DemoApp.class"
test -f "$work/dump/classes/demo/DemoApp\$Inner.class"
test -f "$work/dump/classes/demo/DemoApp\$Another.class"
test -f "$work/dump/dump.log"
test -f "$work/dump/manifest.tsv"
grep -q 'status: ok' "$work/dump/dump.log"

# The agent must keep HotSpot's "A JVMTI agent has been loaded dynamically"
# warning out of the target's stderr, and must hand stderr back afterwards.
if grep -q 'WARNING: A JVMTI agent has been loaded dynamically' "$work/app.log"; then
    echo "FAIL: the dynamic agent warning reached the target's stderr" >&2
    cat "$work/app.log" >&2
    exit 1
fi
test "$(readlink "/proc/$pid/fd/2")" = "$work/app.log"
echo "OK"

echo "== include filter =="
"$root/bin/jvmdump" -q -i 'demo.*' -o "$work/filtered" "$pid"
test -f "$work/filtered/classes/demo/DemoApp.class"
test ! -f "$work/filtered/classes/java/lang/String.class"
echo "OK"

echo "== list mode =="
"$root/bin/jvmdump" -q -l -o "$work/list" "$pid"
grep -qx 'demo.DemoApp' "$work/list/classes.txt"
grep -qx 'java.lang.String' "$work/list/classes.txt"
echo "OK"

echo "== flat mode =="
"$root/bin/jvmdump" -q -f -i 'demo.DemoApp' -o "$work/flat" "$pid"
test -f "$work/flat/classes/demo.DemoApp.class"
echo "OK"

echo "all tests passed"
