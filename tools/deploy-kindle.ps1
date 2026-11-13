param(
    [string]$Device = $env:KINDLE_HOST,
    [int]$Port = 2222,
    [string]$Package = ''
)

$ErrorActionPreference = 'Stop'
if ($Device -notmatch '^[A-Za-z0-9.-]+$' -or $Port -lt 1 -or $Port -gt 65535) {
    throw 'Set -Device or KINDLE_HOST to a valid Kindle address and use an SSH port from 1 to 65535'
}
if (-not $Package) {
    $root = Split-Path $PSScriptRoot -Parent
    $Package = Join-Path $root 'dist/kindle-dashboard-0.1.0-kpw3.tar.gz'
    $packageTime = (Get-Item -LiteralPath $Package).LastWriteTimeUtc
    $inputs = @('src', 'assets', 'kindle', 'cmake', 'CMakeLists.txt', 'config.yaml',
        'tools/build-arm.sh', 'tools/package-arm.sh')
    foreach ($inputPath in $inputs) {
        if (Get-ChildItem -LiteralPath (Join-Path $root $inputPath) -Recurse -File |
            Where-Object LastWriteTimeUtc -gt $packageTime) {
            throw 'Package is older than project inputs. Run: wsl bash tools/build-arm.sh; wsl bash tools/package-arm.sh'
        }
    }
}
$Package = (Resolve-Path -LiteralPath $Package -ErrorAction Stop).Path
$entries = & tar -tzf $Package
if ($LASTEXITCODE -ne 0) { throw "Invalid package: $Package" }
foreach ($entry in $entries) {
    if ($entry -notmatch '^(kindle-dashboard/|extensions/$|extensions/kindle-dashboard-native/)' -or
        $entry -match '(^|/)\.\.(/|$)') {
        throw "Unexpected package entry: $entry"
    }
}
foreach ($required in @('kindle-dashboard/bin/kindle-dashboard', 'kindle-dashboard/control.sh',
    'kindle-dashboard/config.yaml', 'extensions/kindle-dashboard-native/menu.json')) {
    if ($entries -notcontains $required) { throw "Package is missing $required" }
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Package).Hash.ToLowerInvariant()
$archive = "/mnt/us/.kindle-dashboard-$hash.tar.gz"
$target = "root@${Device}:$archive"
Write-Host "Uploading $Package to $Device`:$Port ($hash)"
& scp -P $Port -o BatchMode=yes -o ConnectTimeout=10 $Package $target
if ($LASTEXITCODE -ne 0) { throw 'Package upload failed; old installation was not touched' }

$remote = @'
set -eu
expected=$1
archive="/mnt/us/.kindle-dashboard-$expected.tar.gz"
app=/mnt/us/kindle-dashboard
ext=/mnt/us/extensions/kindle-dashboard-native
stage=/mnt/us/.kindle-dashboard-stage
old_app=/mnt/us/.kindle-dashboard-old
old_ext=/mnt/us/extensions/.kindle-dashboard-native-old
state=/tmp/kindle-dashboard-service

actual=$(sha256sum "$archive")
actual=${actual%% *}
if [ "$actual" != "$expected" ]; then
    rm -f "$archive"
    echo 'Package SHA-256 mismatch; old installation was not touched' >&2
    exit 1
fi
if [ -e "$stage" ] || [ -e "$old_app" ] || [ -e "$old_ext" ]; then
    rm -f "$archive"
    echo 'Previous staging or backup directory exists; inspect it before deploying' >&2
    exit 1
fi
mkdir "$stage" || { rm -f "$archive"; exit 1; }
moved_app=0
moved_ext=0
new_app=0
new_ext=0
was_running=0
cleanup() {
    rc=$?
    trap - EXIT
    set +e
    if [ "$rc" -ne 0 ]; then
        echo 'Deployment failed; restoring previous version' >&2
        if [ "$new_app" = 1 ] && [ -x "$app/control.sh" ]; then
            "$app/control.sh" stop
            i=0
            while { [ "$("$app/control.sh" status)" != stopped ] || [ -e "$state" ]; } && [ "$i" -lt 20 ]; do
                sleep 1
                i=$((i + 1))
            done
            if [ "$("$app/control.sh" status)" != stopped ] || [ -e "$state" ]; then
                echo 'New service did not stop; backups are retained for manual recovery' >&2
                rm -rf "$stage"
                rm -f "$archive"
                exit "$rc"
            fi
        fi
        if [ "$new_app" = 1 ]; then rm -rf "$app"; fi
        if [ "$new_ext" = 1 ]; then rm -rf "$ext"; fi
        if [ "$moved_app" = 1 ]; then mv "$old_app" "$app"; fi
        if [ "$moved_ext" = 1 ]; then mv "$old_ext" "$ext"; fi
        if [ "$was_running" = 1 ] && [ -x "$app/control.sh" ]; then "$app/control.sh" start; fi
    fi
    rm -rf "$stage"
    rm -f "$archive"
    exit "$rc"
}
trap cleanup EXIT

tar -xzf "$archive" -C "$stage"
[ -x "$stage/kindle-dashboard/bin/kindle-dashboard" ]
[ -x "$stage/kindle-dashboard/control.sh" ]
[ -f "$stage/kindle-dashboard/config.yaml" ]
[ -f "$stage/extensions/kindle-dashboard-native/menu.json" ]
if [ -e "$app" ]; then
    [ -x "$app/control.sh" ] || { echo 'Old control.sh is missing' >&2; exit 1; }
    case "$("$app/control.sh" status)" in running*) was_running=1;; esac
    "$app/control.sh" stop
    i=0
    while [ "$("$app/control.sh" status)" != stopped ] || [ -e "$state" ]; do
        i=$((i + 1))
        [ "$i" -lt 20 ] || { echo 'Old service did not stop' >&2; exit 1; }
        sleep 1
    done
    mv "$app" "$old_app"
    moved_app=1
fi
mkdir -p /mnt/us/extensions
if [ -e "$ext" ]; then
    mv "$ext" "$old_ext"
    moved_ext=1
fi
mv "$stage/kindle-dashboard" "$app"
new_app=1
mv "$stage/extensions/kindle-dashboard-native" "$ext"
new_ext=1
"$app/control.sh" start
sleep 3
"$app/control.sh" status | grep -q '^running PID [0-9][0-9]*$'

trap - EXIT
rm -rf "$old_app" "$old_ext" "$stage"
rm -f "$archive" /mnt/us/kindle-dashboard-*.tar.gz
"$app/control.sh" status
# end
'@

($remote -replace "`r", '') | & ssh -p $Port -o BatchMode=yes -o ConnectTimeout=10 "root@$Device" "sh -s -- $hash"
if ($LASTEXITCODE -ne 0) { throw 'Deployment failed; inspect SSH output and device state' }
Write-Host 'Deployment complete'
