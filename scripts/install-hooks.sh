#!/usr/bin/env bash
# install-hooks.sh: the pre-push hook that runs check-no-disc-data.sh over the tree and over every commit about to
# leave this machine. Run once per clone.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
hook="$(git -C "$repo" rev-parse --git-common-dir)/hooks/pre-push"
cat > "$hook" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
bash "$repo/scripts/check-no-disc-data.sh"
zero=0000000000000000000000000000000000000000
while read -r _ local_sha _ remote_sha; do
    [ "$local_sha" = "$zero" ] && continue
    if [ "$remote_sha" = "$zero" ]; then range=$local_sha; else range="$remote_sha..$local_sha"; fi
    bash "$repo/scripts/check-no-disc-data.sh" --history "$range"
done
EOF
chmod +x "$hook"
echo "installed $hook"
