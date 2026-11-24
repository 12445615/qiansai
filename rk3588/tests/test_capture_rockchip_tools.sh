#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT

mkdir -p "$tmp/fixtures"
for tool in afptool rkImageMaker unpack.sh; do
    printf '#!/usr/bin/env bash\necho %s\n' "$tool" >"$tmp/fixtures/$tool"
    chmod +x "$tmp/fixtures/$tool"
done
printf 'fake vdi\n' >"$tmp/ubuntu1.vdi"
cat >"$tmp/fake-guestfish" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
dest=${@: -1}
mkdir -p "$dest/rockdev"
for tool in afptool rkImageMaker unpack.sh; do
    cp "$FAKE_ROCKDEV_FIXTURES/$tool" "$dest/rockdev/$tool"
done
EOF
chmod +x "$tmp/fake-guestfish"

GUESTFISH_BIN="$tmp/fake-guestfish" \
FAKE_ROCKDEV_FIXTURES="$tmp/fixtures" \
bash "$project_dir/scripts/capture_rockchip_tools.sh" \
    --vdi "$tmp/ubuntu1.vdi" \
    --output-dir "$tmp/install"

test -x "$tmp/install/rockdev/afptool"
test -x "$tmp/install/rockdev/rkImageMaker"
test -x "$tmp/install/rockdev/unpack.sh"
grep -Fq 'afptool' "$tmp/install/manifest.sha256"
grep -Fq 'rkImageMaker' "$tmp/install/manifest.sha256"
(cd "$tmp/install" && sha256sum -c manifest.sha256)

echo 'Rockchip tool capture is verified'
