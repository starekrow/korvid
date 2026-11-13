#!/bin/sh
# Signs an iOS application bundle for development on a device: the development profile
# embedded, entitlements from it (the application identifier, the team, get-task-allow), a
# signature with the development identity. kormake package uses it for `ios`, and
# device_test.sh for each test it runs. The bundle's identifier is its Info.plist's.
#
#   sign_app.sh APP [DEVICE]   (DEVICE: a device identifier the profile must include)
#
# From the environment:
#   KORMAKE_IOS_PROFILE        a .mobileprovision; the newest of Xcode's that covers the
#                              identifier (and the device) when unset
#   KORMAKE_CODESIGN_IDENTITY  the signing identity; the first "Apple Development" one when unset
set -eu

app=${1:?usage: sign_app.sh APP [DEVICE]}
device=${2:-}
bundle_id=$(plutil -extract CFBundleIdentifier raw -o - "$app/Info.plist")
tmp=$(mktemp -d "${TMPDIR:-/tmp}/korvid-sign.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

identity=${KORMAKE_CODESIGN_IDENTITY:-}
if [ -z "$identity" ]; then
  identity=$(security find-identity -v -p codesigning | sed -n 's/^ *[0-9]*) \([0-9A-F]*\) "Apple Development:.*/\1/p' | head -1)
  [ -n "$identity" ] || { echo "sign_app: no Apple Development signing identity (sign into Xcode with your account)" >&2; exit 2; }
fi
profile=${KORMAKE_IOS_PROFILE:-}
if [ -z "$profile" ]; then
  profile=$(python3 -I - "$bundle_id" "$device" "$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles" "$HOME/Library/MobileDevice/Provisioning Profiles" <<'EOF'
import glob, os, plistlib, subprocess, sys, datetime
bundle, device, dirs = sys.argv[1], sys.argv[2], sys.argv[3:]
best = None
for d in dirs:
    for f in glob.glob(os.path.join(d, "*.mobileprovision")):
        try:
            p = plistlib.loads(subprocess.run(["security", "cms", "-D", "-i", f], capture_output=True, check=True).stdout)
        except Exception:
            continue
        app = p.get("Entitlements", {}).get("application-identifier", "")
        team = p.get("TeamIdentifier", [""])[0]
        if app not in (team + "." + bundle, team + ".*"): continue
        if device and device not in p.get("ProvisionedDevices", []): continue
        if p.get("ExpirationDate") and p["ExpirationDate"].replace(tzinfo=datetime.timezone.utc) < datetime.datetime.now(datetime.timezone.utc): continue
        if not best or os.path.getmtime(f) > os.path.getmtime(best): best = f
if not best:
    sys.stderr.write("sign_app: no development profile covers %s%s (build an app with that identifier to a device in Xcode once)\n" %
                     (bundle, " on " + device if device else ""))
    sys.exit(1)
print(best)
EOF
) || exit 2
fi
security cms -D -i "$profile" > "$tmp/profile.plist"
team=$(plutil -extract TeamIdentifier.0 raw -o - "$tmp/profile.plist")
cp "$profile" "$app/embedded.mobileprovision"
cat > "$tmp/entitlements.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>application-identifier</key><string>$team.$bundle_id</string>
  <key>com.apple.developer.team-identifier</key><string>$team</string>
  <key>get-task-allow</key><true/>
</dict></plist>
EOF
codesign --force --sign "$identity" --entitlements "$tmp/entitlements.plist" "$app" > "$tmp/sign.log" 2>&1 || { cat "$tmp/sign.log" >&2; exit 2; }
codesign --verify --strict "$app"
