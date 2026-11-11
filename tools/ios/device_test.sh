#!/bin/sh
# Runs one test executable on an iPhone or iPad: kormake's test runner for the `ios` platform
# (toolchain-apple's test_runner), as `xcrun simctl spawn booted` is the Simulator's. A device
# runs only signed applications, so the executable becomes one: a bundle with what the tests
# read beside them (assets/, shaders/), an Info.plist, the development profile embedded, a
# signature with its entitlements. Every suite installs as the same application, one at a
# time (the toolchain's test_jobs = 1), and runs to its end with its output here.
#
#   device_test.sh /abs/path/to/bin/<suite>_tests
#
# From the environment (and sign_app.sh's: the profile and the identity):
#   KORMAKE_IOS_TEST_BUNDLE_ID  the application's identifier (required): one the profile covers
#   KORMAKE_IOS_DEVICE          the device's name or identifier; the one physical device
#                               connected when unset
set -eu

exe=${1:?usage: device_test.sh EXE}
name=$(basename "$exe")
bin=$(dirname "$exe")
bundle_id=${KORMAKE_IOS_TEST_BUNDLE_ID:-}
if [ -z "$bundle_id" ]; then
  echo "device_test: set KORMAKE_IOS_TEST_BUNDLE_ID to an application identifier your development profile covers" >&2
  exit 2
fi
work=$(dirname "$bin")/device
mkdir -p "$work"
tmp=$(mktemp -d "$work/run.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

# The device: named, or the one connected physical device.
devices="$tmp/devices.json"
xcrun devicectl list devices --json-output "$devices" >/dev/null 2>&1 || { echo "device_test: devicectl cannot list devices" >&2; exit 2; }
device=$(python3 -I - "$devices" "${KORMAKE_IOS_DEVICE:-}" <<'EOF'
import json, sys
want = sys.argv[2]
found = []
for d in json.load(open(sys.argv[1]))["result"]["devices"]:
    hw, props = d.get("hardwareProperties", {}), d.get("deviceProperties", {})
    if hw.get("reality") != "physical": continue
    if d.get("connectionProperties", {}).get("tunnelState") == "unavailable": continue
    udid = hw.get("udid", "")
    if want and want not in (udid, d.get("identifier"), props.get("name")): continue
    found.append(udid)
if len(found) != 1:
    sys.stderr.write("device_test: %s\n" % ("no connected device" + (" named " + want if want else "") if not found else
                     "more than one device; set KORMAKE_IOS_DEVICE"))
    sys.exit(1)
print(found[0])
EOF
) || exit 2

# The application.
app="$tmp/$name.app"
mkdir -p "$app"
cp "$exe" "$app/$name"
for d in assets shaders; do [ -d "$bin/$d" ] && cp -R "$bin/$d" "$app/$d"; done
cat > "$app/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>$name</string>
  <key>CFBundleIdentifier</key><string>$bundle_id</string>
  <key>CFBundleName</key><string>$name</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
  <key>LSRequiresIPhoneOS</key><true/>
  <key>MinimumOSVersion</key><string>26.0</string>
  <key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
</dict></plist>
EOF
sh "$(dirname "$0")/sign_app.sh" "$app" "$device" || exit 2

# Install, run to the end, and answer with the test's own exit code.
xcrun devicectl device install app --device "$device" "$app" > "$tmp/install.log" 2>&1 || { cat "$tmp/install.log" >&2; exit 2; }
set +e
xcrun devicectl device process launch --device "$device" --console --terminate-existing "$bundle_id" 2>&1 | tee "$tmp/run.log" |
  grep -v -E '^(Launched application with|Waiting for the application to terminate|The app terminated with)'
set -e
code=$(sed -n 's/^The app terminated with the exit code \(-*[0-9]*\)\.*$/\1/p' "$tmp/run.log" | tail -1)
if [ -z "$code" ]; then
  echo "device_test: $name did not report an exit code (crashed, or the device went away):" >&2
  tail -5 "$tmp/run.log" >&2
  exit 1
fi
# devicectl has been seen to report a crash as exit code 0: a pass is the exit code and the
# suite's own summary (test_report's "<suite>: N checks passed") together.
if [ "$code" = 0 ] && ! grep -q -E "test: $name: [0-9]+ checks passed" "$tmp/run.log"; then
  echo "device_test: $name exited 0 without its summary (it crashed, or its output was cut off)" >&2
  exit 1
fi
exit "$code"
