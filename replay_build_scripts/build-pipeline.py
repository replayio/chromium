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


# The branch macOS builds on without BUILD_MAC=1.
MAC_BRANCHES = ("main",)


def builds_mac():
    """True if this build starts a macOS build of chromium-build too."""
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


def driver_build(pin, env):
    build = {
        "commit": pin.commit,
        "branch": pin.branch,
        "message": "Triggered from chromium: ${BUILDKITE_MESSAGE}",
    }
    if env:
        build["env"] = env
    return build


def driver_step(pin, driver_revision):
    """trigger build-driver-linker, or DriverBuildCheck noop if archive already on S3."""
    archive = f"linux-recordreplay-{driver_revision}.tgz"
    if driver_archive_present(archive):
        return {
            "label": "DriverBuildCheck (archive present)",
            "key": "build-driver-linker",
            "command": f'echo "DriverBuildCheck: {archive} already on S3"',
            "agents": ["deploy=true"],
            "plugins": [{"thedyrt/skip-checkout#v0.1.1": None}],
        }
    return {
        "trigger": "build-driver-linker",
        "key": "build-driver-linker",
        "build": driver_build(pin, {}),
    }


def mac_steps(pin, env):
    """A chromium-build build of macOS, which nothing here waits for.

    It needs the Mac, which can take hours to come up, and the Linux build and
    whatever waits for this build need none of it. REPLAY_MAC_BUILD=1 selects
    the macOS steps of .buildkite/pipeline.yml instead of the Linux ones.

    When the macOS drivers of the pin are missing, a build-driver-linker build of
    them starts too, which nothing waits for either: the macOS build waits for
    their archives on a light agent (macos-drivers), without holding the Mac that
    builds them. build-driver-linker builds the macOS pieces only with
    BUILD_MAC=1, and leaves out the Linux ones with SKIP_LINUX=1.

    soft_fail: a failure to create either build does not fail this one.
    """
    steps = []
    archives = [f"macOS-recordreplay-{pin.commit[:12]}{suffix}.tgz" for suffix in ("", "-arm")]
    if not all(driver_archive_present(archive) for archive in archives):
        steps.append(
            {
                "trigger": "build-driver-linker",
                "key": "build-driver-linker-macos",
                "async": True,
                "soft_fail": True,
                "build": driver_build(pin, {"BUILD_MAC": "1", "SKIP_LINUX": "1"}),
            }
        )
    steps.append(
        {
            "trigger": "chromium-build",
            "label": "chromium-build (macOS)",
            "async": True,
            "soft_fail": True,
            "build": chromium_build({**env, "REPLAY_MAC_BUILD": "1"}),
        }
    )
    return steps


def chromium_build(env):
    return {
        "env": env,
        "message": "${BUILDKITE_MESSAGE}",
        "branch": "${BUILDKITE_BRANCH}",
        "commit": "${BUILDKITE_COMMIT}",
    }


def generate_buildkite_pipeline(pin):
    """Generates a Buildkite pipeline with a dynamic commit hash in JSON format."""

    # driver_revision is the first 12 characters of the commit hash.
    driver_revision = pin.commit[:12]
    env = {
        "DRIVER_REVISION": driver_revision,
        "REPLAY_BACKEND_REV": pin.commit,
    }
    # What the 157 recording test step runs (.buildkite/pipeline.yml): set on
    # this build, it has to reach the triggered one.
    test_case = os.environ.get("REPLAY_157_TEST_CASE")
    if test_case:
        env["REPLAY_157_TEST_CASE"] = test_case
    pipeline = {
        "steps": [
            driver_step(pin, driver_revision),
            {
                "trigger": "chromium-build",
                "build": chromium_build(env),
                "depends_on": ["build-driver-linker"],
            },
            *(mac_steps(pin, env) if builds_mac() else []),
        ]
    }
    return json.dumps(pipeline, indent=4)


def main():
    pin = read_pin("REPLAY_BACKEND_REV")
    pipeline_json = generate_buildkite_pipeline(pin)
    print(pipeline_json)


if __name__ == "__main__":
    main()
