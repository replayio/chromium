"""Generates the pipeline that builds the driver/linker and Chromium.

REPLAY_BACKEND_REV is "<commit> [<branch>]". The branch (default: master) is the
backend branch the commit is on: build-driver-linker's agents verify that.
"""

import json
import urllib.error
import urllib.request
from dataclasses import dataclass


@dataclass(frozen=True)
class BackendPin:
    commit: str
    branch: str


def read_pin(file_path):
    """Reads the backend pin from the given file."""
    with open(file_path, "r") as file:
        fields = file.read().split()
    return BackendPin(commit=fields[0], branch=fields[1] if len(fields) > 1 else "master")


def driver_archive_present(driver_revision):
    """DriverBuildCheck: True if linux driver archive for this rev is already on S3."""
    url = f"https://static.replay.io/downloads/linux-recordreplay-{driver_revision}.tgz"
    req = urllib.request.Request(url, method="HEAD")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return 200 <= resp.status < 300
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError):
        return False


def driver_step(pin, driver_revision):
    """trigger build-driver-linker, or DriverBuildCheck noop if archive already on S3."""
    if driver_archive_present(driver_revision):
        return {
            "label": "DriverBuildCheck (archive present)",
            "key": "build-driver-linker",
            "command": (
                f'echo "DriverBuildCheck: linux-recordreplay-{driver_revision}.tgz already on S3"'
            ),
            "agents": ["deploy=true"],
            "plugins": [{"thedyrt/skip-checkout#v0.1.1": None}],
        }
    return {
        "trigger": "build-driver-linker",
        "key": "build-driver-linker",
        "build": {
            "commit": pin.commit,
            "branch": pin.branch,
            "message": "Triggered from chromium: ${BUILDKITE_MESSAGE}",
        },
    }


def generate_buildkite_pipeline(pin):
    """Generates a Buildkite pipeline with a dynamic commit hash in JSON format."""

    pipeline = {
        "steps": [
            {
                # CI vehicle: chromium-build has no build step whose artifact
                # carries the pin, so the recording step takes it from env.
                "trigger": "chromium-build",
                "build": {
                    "env": {"REPLAY_BACKEND_REV": pin.commit},
                    "message": "${BUILDKITE_MESSAGE}",
                    "branch": "${BUILDKITE_BRANCH}",
                    "commit": "${BUILDKITE_COMMIT}",
                },
            },
        ]
    }
    return json.dumps(pipeline, indent=4)


def main():
    pin = read_pin("REPLAY_BACKEND_REV")
    pipeline_json = generate_buildkite_pipeline(pin)
    print(pipeline_json)


if __name__ == "__main__":
    main()
