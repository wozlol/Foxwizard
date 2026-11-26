#!/bin/sh
# Publishes Patchling and the live firmware in firmware/ to woz.lol/fox. Run build_firmware.sh (no options) first, or copy the tested
# test channel files into firmware/ and firmware.json beside them.
#
# It backs up the server's fox folder, reads the firmware.json the server has now, and writes the new firmware.json with that release as
# "previous" (the roll back Patchling offers from the version in its connection pill). The previous release is chosen again at every
# publish. Publishing the same version again keeps the previous it had. The release files stay on the server, nothing is deleted there.
# firmware.json goes up last, so Patchling never reads a release whose files are not there yet.
#
# ./publish_to_server.sh --dry-run   the checks and the list of files that would go up, nothing written on the server
# ./publish_to_server.sh             the publish
set -e
cd "$(dirname "$0")"
HOST=bluehost
SITE=public_html/woz/fox
DRY=""
case "$1" in --dry-run) DRY=1 ;; "") ;; *) echo "Usage: $0 [--dry-run]" >&2; exit 1 ;; esac

md5_of() { md5 -q "$1" 2>/dev/null || md5sum "$1" | cut -d' ' -f1; }

# The local release must be complete and match its own firmware.json and Patchling's version.
python3 - <<'PY'
import hashlib, json, os, sys
j = json.load(open("firmware/firmware.json"))
def check(entry, what):
    path = os.path.join("firmware", entry["bin"])
    if not os.path.exists(path): sys.exit("Missing " + path)
    data = open(path, "rb").read()
    if len(data) != entry["size"] or hashlib.md5(data).hexdigest() != entry["md5"]:
        sys.exit(what + " does not match its firmware.json entry: " + path)
check(j, "Main firmware")
for key in ("host", "bluetooth"):
    if key in j: check(j[key], key)
index = open("index.html").read(); sw = open("sw.js").read()
v = "%d.%d" % (j["major"], j["minor"])
if ("FOXWIZARD PATCHLING v%s " % v) not in index or ("foxwizard-patchling-%s'" % v) not in sw:
    sys.exit("Patchling's footer and cache name are not " + v)
print("Local release %s is complete." % v)
PY

STAMP=$(date +%Y%m%d-%H%M%S)
if [ -z "$DRY" ]; then
  # One connection: back up the folder, then read what the server has and which files it holds.
  REMOTE=$(ssh "$HOST" "mkdir -p ~/backups && tar czf ~/backups/fox-$STAMP.tgz -C ~/public_html/woz fox && echo BACKUP_OK && cat ~/$SITE/firmware/firmware.json && echo FILES && ls ~/$SITE/firmware")
else
  REMOTE=$(ssh "$HOST" "echo BACKUP_SKIPPED && cat ~/$SITE/firmware/firmware.json && echo FILES && ls ~/$SITE/firmware")
fi
echo "$REMOTE" | head -1
printf '%s\n' "$REMOTE" > .remote_state.tmp

python3 - <<'PY'
import json, os, sys
raw = open(".remote_state.tmp").read()
os.remove(".remote_state.tmp")
_, rest = raw.split("\n", 1)
body, files = rest.split("\nFILES\n")
server = json.loads(body)
have = set(files.split())
new = json.load(open("firmware/firmware.json"))
new.pop("previous", None)
same = (server["major"], server["minor"]) == (new["major"], new["minor"])
prev = server.get("previous") if same else {k: v for k, v in server.items() if k != "previous"}
if prev:
    entries = [prev] + [prev[k] for k in ("host", "bluetooth") if k in prev]
    for e in entries:
        # A server release is only a roll back point while its files are still there. Local files go up with this publish.
        if e["bin"] not in have and not os.path.exists(os.path.join("firmware", e["bin"])):
            sys.exit("The server does not have " + e["bin"] + ", the roll back point")
    new["previous"] = prev
json.dump(new, open("firmware/firmware.json.publish", "w"), indent=2)
open("firmware/firmware.json.publish", "a").write("\n")
print("Server has %d.%d, this publish is %d.%d, roll back point: %s" % (server["major"], server["minor"], new["major"], new["minor"], ("%d.%d" % (prev["major"], prev["minor"])) if prev else "none"))
PY

RSYNC="rsync -rlt --itemize-changes --exclude .DS_Store --exclude firmware/test --exclude firmware/firmware.json --exclude firmware/firmware.json.publish --exclude publish_to_server.sh"
if [ -n "$DRY" ]; then
  $RSYNC -n ./ "$HOST:$SITE/"
  rm -f firmware/firmware.json.publish
  echo "Dry run, nothing was written on the server."
  exit 0
fi
$RSYNC ./ "$HOST:$SITE/"
rsync -lt firmware/firmware.json.publish "$HOST:$SITE/firmware/firmware.json"
mv firmware/firmware.json.publish firmware/firmware.json
echo "Published. Check https://woz.lol/fox/firmware/firmware.json"
