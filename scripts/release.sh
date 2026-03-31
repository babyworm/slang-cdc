#!/bin/bash
# Release script for sv-cdccheck
# Usage: ./scripts/release.sh v0.1.0

set -euo pipefail

VERSION="${1:?Usage: $0 <version-tag> (e.g., v0.1.0)}"

echo "=== sv-cdccheck release: $VERSION ==="

# Verify clean working tree
if [ -n "$(git status --porcelain)" ]; then
    echo "ERROR: Working tree is not clean. Commit or stash changes first."
    exit 1
fi

# Verify tests pass
echo "Running tests..."
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
cd build && ctest --output-on-failure && cd ..
echo "Tests passed."

# Verify version matches
CMAKE_VERSION=$(grep "project(sv-cdccheck VERSION" CMakeLists.txt | sed 's/.*VERSION \([0-9]*\.[0-9]*\.[0-9]*\).*/\1/')
TAG_VERSION="${VERSION#v}"
if [ "$CMAKE_VERSION" != "$TAG_VERSION" ]; then
    echo "ERROR: CMakeLists.txt version ($CMAKE_VERSION) does not match tag ($TAG_VERSION)"
    echo "Update CMakeLists.txt and src/main.cpp first."
    exit 1
fi

# Create and push tag
echo "Creating tag: $VERSION"
git tag -a "$VERSION" -m "Release $VERSION"
git push origin "$VERSION"

echo ""
echo "=== Tag $VERSION pushed ==="
echo "GitHub Actions will build and create the release automatically."
echo "Monitor at: https://github.com/babyworm/sv-cdccheck/actions"
