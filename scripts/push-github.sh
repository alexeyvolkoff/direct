#!/usr/bin/env bash
# ==============================================================================
# Clean Orphan / Incremental Sync to Public GitHub (alexeyvolkoff/onmydisk)
#
# Publishes the exact Git tree of TARGET_REF (default: HEAD) onto the clean
# public GitHub `direct` branch without ever exposing pre-open-source legacy
# commits. Safe to run manually (`git push-gh`) and from GitLab CI.
# Never modifies the local working tree, index, or current branch.
# ==============================================================================
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

TARGET_REF="${1:-HEAD}"
GITHUB_REMOTE="${GITHUB_REMOTE:-github}"
GITHUB_URL="${GITHUB_URL:-git@github.com:alexeyvolkoff/onmydisk.git}"
GITHUB_BRANCH="${GITHUB_BRANCH:-direct}"

if ! git remote get-url "$GITHUB_REMOTE" >/dev/null 2>&1; then
  git remote add "$GITHUB_REMOTE" "$GITHUB_URL"
fi

TARGET_SHA="$(git rev-parse --verify "${TARGET_REF}^{commit}")"
TARGET_TREE="$(git rev-parse --verify "${TARGET_SHA}^{tree}")"

echo "[push-github] Checking remote '$GITHUB_REMOTE' ($GITHUB_URL, branch '$GITHUB_BRANCH')..."
REMOTE_HEADS="$(git ls-remote --heads "$GITHUB_REMOTE" "$GITHUB_BRANCH")"

export GIT_AUTHOR_NAME="$(git log -1 --format='%an' "$TARGET_SHA")"
export GIT_AUTHOR_EMAIL="$(git log -1 --format='%ae' "$TARGET_SHA")"
export GIT_AUTHOR_DATE="$(git log -1 --format='%aI' "$TARGET_SHA")"
export GIT_COMMITTER_NAME="$GIT_AUTHOR_NAME"
export GIT_COMMITTER_EMAIL="$GIT_AUTHOR_EMAIL"

if [ -z "$REMOTE_HEADS" ]; then
  echo "[push-github] Branch '$GITHUB_BRANCH' not found on GitHub — creating clean initial open-source commit..."
  NEW_SHA="$(git commit-tree "$TARGET_TREE" -m "Initial open-source release of On My Disk Direct")"
else
  git fetch --quiet "$GITHUB_REMOTE" "+refs/heads/${GITHUB_BRANCH}:refs/remotes/${GITHUB_REMOTE}/${GITHUB_BRANCH}"
  GH_PARENT="$(git rev-parse "refs/remotes/${GITHUB_REMOTE}/${GITHUB_BRANCH}")"
  GH_TREE="$(git rev-parse "${GH_PARENT}^{tree}")"

  if [ "$GH_TREE" = "$TARGET_TREE" ]; then
    echo "[push-github] GitHub '$GITHUB_BRANCH' ($GH_PARENT) already matches $TARGET_SHA. Done."
    exit 0
  fi

  COMMIT_MSG="$(git log -1 --format='%B' "$TARGET_SHA")"
  NEW_SHA="$(git commit-tree "$TARGET_TREE" -p "$GH_PARENT" -m "$COMMIT_MSG")"
fi

echo "[push-github] Force-syncing clean commit $NEW_SHA (from $TARGET_SHA) -> $GITHUB_REMOTE ($GITHUB_BRANCH)..."
OMD_ALLOW_CLEAN_GITHUB_PUSH=1 git push --force "$GITHUB_REMOTE" \
  "${NEW_SHA}:refs/heads/${GITHUB_BRANCH}"

echo "[push-github] Successfully synced '$GITHUB_BRANCH' to GitHub!"
