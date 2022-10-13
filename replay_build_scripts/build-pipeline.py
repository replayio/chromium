import json
import urllib.error
import urllib.request


def read_commit_hash(file_path):
    """Reads the commit hash from the given file."""
    with open(file_path, "r") as file:
        return file.read().strip()


def driver_archive_present(driver_revision):
    """DriverBuildCheck: True if linux driver archive for this rev is already on S3."""
    url = f"https://static.replay.io/downloads/linux-recordreplay-{driver_revision}.tgz"
    req = urllib.request.Request(url, method="HEAD")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return 200 <= resp.status < 300
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError):
        return False


def driver_step(backend_commit_hash, driver_revision):
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
            "commit": backend_commit_hash,
            "message": "Triggered from chromium: ${BUILDKITE_MESSAGE}",
        },
    }


def generate_buildkite_pipeline(backend_commit_hash):
    """Generates a Buildkite pipeline with a dynamic commit hash in JSON format."""

    # driver_revision is the first 12 characters of the commit hash.
    driver_revision = backend_commit_hash.split()[0][:12]
    pipeline = {
        "steps": [
            driver_step(backend_commit_hash, driver_revision),
            {
                "trigger": "chromium-build",
                "build": {
                    "env": {
                        "DRIVER_REVISION": driver_revision,
                        "REPLAY_BACKEND_REV": backend_commit_hash,
                    },
                    "message": "${BUILDKITE_MESSAGE}",
                    "branch": "${BUILDKITE_BRANCH}",
                    "commit": "${BUILDKITE_COMMIT}",
                },
                "depends_on": ["build-driver-linker"],
            },
        ]
    }
    return json.dumps(pipeline, indent=4)


def main():
    backend_commit_hash = read_commit_hash("REPLAY_BACKEND_REV")
    pipeline_json = generate_buildkite_pipeline(backend_commit_hash)
    print(pipeline_json)


if __name__ == "__main__":
    main()
