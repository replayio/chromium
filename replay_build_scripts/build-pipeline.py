"""Generates the pipeline that builds the driver/linker and Chromium.

REPLAY_BACKEND_REV is "<commit> [<branch>]". The branch (default: master) is the
backend branch the commit is on: build-driver-linker's agents verify that.
"""

import json
import os
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


# The branches chromium-build builds macOS on without BUILD_MAC=1: the
# condition of the macOS steps in .buildkite/pipeline.yml.
MAC_BRANCHES = ("master", "rebase/157/working")


def builds_mac():
    """True if this build's chromium-build builds macOS too."""
    return (
        os.environ.get("BUILDKITE_BRANCH") in MAC_BRANCHES
        or os.environ.get("BUILD_MAC") == "1"
    )


def driver_archive_present(archive):
    """DriverBuildCheck: True if this driver archive is already on S3."""
    url = f"https://static.replay.io/downloads/{archive}"
    req = urllib.request.Request(url, method="HEAD")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return 200 <= resp.status < 300
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError):
        return False


def driver_step(pin, driver_revision, mac):
    """trigger build-driver-linker for the archives this build needs that are
    not on S3 yet, or DriverBuildCheck noop if all are there."""
    linux = f"linux-recordreplay-{driver_revision}.tgz"
    macs = [f"macOS-recordreplay-{driver_revision}{suffix}.tgz" for suffix in ("", "-arm")]
    linux_present = driver_archive_present(linux)
    mac_missing = mac and not all(driver_archive_present(archive) for archive in macs)
    if linux_present and not mac_missing:
        present = " ".join([linux, *(macs if mac else [])])
        return {
            "label": "DriverBuildCheck (archives present)",
            "key": "build-driver-linker",
            "command": f'echo "DriverBuildCheck: {present} already on S3"',
            "agents": ["deploy=true"],
            "plugins": [{"thedyrt/skip-checkout#v0.1.1": None}],
        }
    # build-driver-linker builds the macOS pieces only with BUILD_MAC=1, on a
    # Mac, and leaves out the Linux ones with SKIP_LINUX=1.
    env = {}
    if mac_missing:
        env["BUILD_MAC"] = "1"
    if linux_present:
        env["SKIP_LINUX"] = "1"
    build = {
        "commit": pin.commit,
        "branch": pin.branch,
        "message": "Triggered from chromium: ${BUILDKITE_MESSAGE}",
    }
    if env:
        build["env"] = env
    return {"trigger": "build-driver-linker", "key": "build-driver-linker", "build": build}


def generate_buildkite_pipeline(pin):
    """Generates a Buildkite pipeline with a dynamic commit hash in JSON format."""

    # driver_revision is the first 12 characters of the commit hash.
    driver_revision = pin.commit[:12]
    mac = builds_mac()
    chromium_build = {
        "message": "${BUILDKITE_MESSAGE}",
        "branch": "${BUILDKITE_BRANCH}",
        "commit": "${BUILDKITE_COMMIT}",
    }
    if os.environ.get("BUILD_MAC") == "1":
        chromium_build["env"] = {"BUILD_MAC": "1"}
    pipeline = {
        "steps": [
            driver_step(pin, driver_revision, mac),
            {
                # No pin in env: chromium-build reads it from the commit, as
                # every other way of starting it must.
                "trigger": "chromium-build",
                "build": chromium_build,
                "depends_on": ["build-driver-linker"],
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
