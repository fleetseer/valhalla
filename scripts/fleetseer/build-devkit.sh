#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${FLEETSEER_VALHALLA_BUILD_DIR:-$repo_root/build}"
commit="$(git -C "$repo_root" rev-parse HEAD)"
short_commit="${commit:0:7}"
output_dir="${FLEETSEER_VALHALLA_DEVKIT_OUTPUT_DIR:-$repo_root/dist}"
package_root="fleetseer-valhalla-devkit-linux-x64-$short_commit"
staging_dir="$output_dir/$package_root"
archive_path="$output_dir/fleetseer-valhalla-devkit-linux-x64.tar.gz"

static_lib="$build_dir/src/libvalhalla.a"
if [[ ! -f "$static_lib" ]]; then
  echo "Missing Valhalla static library: $static_lib" >&2
  echo "Build Valhalla before creating the FleetSeer devkit." >&2
  exit 1
fi

rm -rf "$staging_dir"
mkdir -p "$staging_dir"

copy_path() {
  local source="$1"
  local destination="$2"
  mkdir -p "$(dirname "$destination")"
  cp -a "$source" "$destination"
}

copy_path "$repo_root/valhalla" "$staging_dir/valhalla"
copy_path "$repo_root/proto" "$staging_dir/proto"
copy_path "$repo_root/third_party/rapidjson/include" "$staging_dir/third_party/rapidjson/include"
copy_path "$repo_root/third_party/date/include" "$staging_dir/third_party/date/include"
copy_path "$repo_root/third_party/unordered_dense/include" "$staging_dir/third_party/unordered_dense/include"
copy_path "$repo_root/date_time" "$staging_dir/date_time"
copy_path "$static_lib" "$staging_dir/build/src/libvalhalla.a"

while IFS= read -r generated_file; do
  relative_path="${generated_file#$build_dir/}"
  copy_path "$generated_file" "$staging_dir/build/$relative_path"
done < <(find "$build_dir/src" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.inc' \))

cat > "$staging_dir/fleetseer-valhalla-devkit.json" <<JSON
{
  "repository": "https://github.com/fleetseer/valhalla.git",
  "commit": "$commit",
  "buildDir": "build",
  "staticLibrary": "build/src/libvalhalla.a"
}
JSON

tar -czf "$archive_path" -C "$staging_dir" .
sha256sum "$archive_path" > "$archive_path.sha256"

echo "$archive_path"
cat "$archive_path.sha256"
