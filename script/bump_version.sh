#!/bin/bash -e

# Sets the package version in every file that carries it.
#
# Usage:
#   script/bump_version.sh            # bump the patch version (0.26.7 -> 0.26.8)
#   script/bump_version.sh 0.27.0     # set an explicit version

cd "$(dirname "$0")/.."

current=$(sed -n 's/^version = "\(.*\)"$/\1/p' Cargo.toml | head -n 1)
if [ -z "$current" ]; then
  echo "could not read the current version from Cargo.toml"
  exit 1
fi

if [ -n "$1" ]; then
  next=$1
else
  IFS=. read -r major minor patch <<<"$current"
  next="$major.$minor.$((patch + 1))"
fi

if ! [[ $next =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "invalid version: $next"
  exit 1
fi

echo "bumping $current -> $next"

cur_re=${current//./\\.}
next_re=${next//./\\.}

files="CMakeLists.txt Makefile Cargo.toml pyproject.toml Cargo.lock package.json package-lock.json tree-sitter.json"

# Other dependencies may already be at the new version (e.g. tree-sitter-cli),
# so count how many occurrences each file gains rather than checking for presence.
declare -A before
for f in $files; do
  before[$f]=$(grep -c "$next_re" "$f" || true)
done

# CMakeLists.txt: VERSION "x.y.z" inside project(...)
sed -i.bak "s/^\( *VERSION \)\"$cur_re\"/\1\"$next\"/" CMakeLists.txt

# Makefile: VERSION := x.y.z
sed -i.bak "s/^VERSION := $cur_re$/VERSION := $next/" Makefile

# Cargo.toml and pyproject.toml: top-level version = "x.y.z"
sed -i.bak "s/^version = \"$cur_re\"$/version = \"$next\"/" Cargo.toml pyproject.toml

# Cargo.lock: only the tree-sitter-scala package entry
sed -i.bak "/^name = \"tree-sitter-scala\"$/{n;s/^version = \"$cur_re\"$/version = \"$next\"/;}" Cargo.lock

# package.json: top-level "version"
sed -i.bak "s/^  \"version\": \"$cur_re\",$/  \"version\": \"$next\",/" package.json

# package-lock.json: top-level "version" and the root package entry
sed -i.bak \
  -e "s/^  \"version\": \"$cur_re\",$/  \"version\": \"$next\",/" \
  -e "/^    \"\": {$/,/^    }/s/^      \"version\": \"$cur_re\",$/      \"version\": \"$next\",/" \
  package-lock.json

# tree-sitter.json: metadata.version
sed -i.bak "s/^    \"version\": \"$cur_re\",$/    \"version\": \"$next\",/" tree-sitter.json

for f in $files; do
  rm -f "$f.bak"
done

# Check that every file gained the expected number of new-version lines.
status=0
for f in $files; do
  expected=1
  [ "$f" = package-lock.json ] && expected=2
  after=$(grep -c "$next_re" "$f" || true)
  if [ $((after - before[$f])) -ne $expected ]; then
    echo "failed to update $f"
    status=1
  fi
done
exit $status
