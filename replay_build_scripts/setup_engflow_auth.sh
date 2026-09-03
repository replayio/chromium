#!/usr/bin/env bash
# fetch the pinned engflow_auth binary and import the CI credential into a file
# store on the mounted /chromium dir, so reproxy (which runs inside the build
# container) can call engflow_auth as its RBE credential helper instead of using
# the old 90-day mTLS certs.
#
# the binary lands at ~/chromium/engflow_auth and the token store under
# ~/chromium/.engflow, both of which the build container sees at /chromium/...
# via the bind mount in buildLinux.mjs.
#
# NOT VALIDATED END-TO-END YET. open questions:
#   - does the in-container `engflow_auth get` read the store via XDG_CONFIG_HOME,
#     or does it need HOME/.config (and/or an explicit -store=file)?
#   - exact _args for the helper (`get` vs `get -store=file` vs cluster url)
#   - does reproxy accept engflow_auth's expiry field/format, or do we need a
#     thin wrapper to reshape the JSON?
# needs the `engflow-credential` secret to exist in AWS SM us-east-2 first (one
# `engflow_auth login` + `export` as a service account).
set -euo pipefail

DEST=/home/ubuntu/chromium/engflow_auth
URL=https://github.com/EngFlow/auth/releases/download/v0.0.14/engflow_auth_linux_x64
SHA=1f78c0a56fde3e2b234c7ad0932688322f6b56d4cd621ce6f19350cbf03d9d4a
STORE=/home/ubuntu/chromium/.engflow

# download + verify the binary (skip if already present and matching)
if [ ! -f "$DEST" ] || ! echo "$SHA  $DEST" | sha256sum -c - >/dev/null 2>&1; then
  curl -fsSL "$URL" -o "$DEST"
  echo "$SHA  $DEST" | sha256sum -c -
  chmod +x "$DEST"
fi

# import the credential (loaded by the aws-sm plugin into ENGFLOW_CRED) into a
# file store the build container can read
mkdir -p "$STORE"
XDG_CONFIG_HOME="$STORE" "$DEST" import -store=file <<< "${ENGFLOW_CRED:?ENGFLOW_CRED not set}"
