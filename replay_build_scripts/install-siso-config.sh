#!/usr/bin/env bash
set -xeuo pipefail

thisFile=$(readlink -f "$BASH_SOURCE")
thisDir=$(dirname "$thisFile")
src=$(readlink -f "$thisDir/..")
gclientRoot=$(readlink -f "$src/..")

: "${RBE_tls_client_auth_cert:?must be set (per-machine EngFlow client cert path)}"
: "${RBE_tls_client_auth_key:?must be set (per-machine EngFlow client key path)}"

cp "$thisDir/replay.gclient" "$gclientRoot/.gclient"

python3 "$src/build/config/siso/configure_siso.py" \
  --reapi_instance=default \
  --reapi_address=simpsonite.cluster.engflow.com:443 \
  --reapi_backend_config_path="$thisDir/backend.star"

# .sisorc
cp "$thisDir/siso.sisorc" "$src/build/config/siso/.sisorc"

# args.gn — platform-specific, into the matching out/ dir.
if [[ "${REPLAY_BUILD_ARM:-}" == "1" ]]; then
  outdir="$src/out/Release-ARM"
else
  outdir="$src/out/Release"
fi
case "$(uname -s)" in
  Linux)  argsSrc="$thisDir/linux_args.gn" ;;
  Darwin)
    if [[ "$(uname -m)" == "arm64" ]]; then
      argsSrc="$thisDir/mac_arm64_args.gn"
    else
      argsSrc="$thisDir/mac_x86_64_args.gn"
    fi
    ;;
  *) argsSrc="$thisDir/windows_args.gn" ;;
esac
mkdir -p "$outdir"
cp "$argsSrc" "$outdir/args.gn"

echo "Installed $gclientRoot/.gclient"
echo "Installed $src/build/config/siso/.sisorc"
echo "Installed $outdir/args.gn"
