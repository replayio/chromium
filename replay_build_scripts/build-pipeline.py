import json


def read_commit_hash(file_path):
    """Reads the commit hash from the given file."""
    with open(file_path, "r") as file:
        return file.read().strip()


def generate_buildkite_pipeline(commit_hash):
    """Generates a Buildkite pipeline with a dynamic commit hash in JSON format."""
    pipeline = {
        "steps": [
            {
                "trigger": "deploy-driver-linker",
                "key": "deploy-driver-linker",
                "message": "Triggered from chromium: ${BUILDKIT_MESSAGE}",
                "build": {"commit": commit_hash},
            },
            {
                "trigger": "chromium-build",
                "build": {
                    "env": {"DRIVER_REVISION": commit_hash},
                },
                "depends_on": ["deploy-driver-linker"],
            },
        ]
    }
    return json.dumps(pipeline, indent=4)


def main():
    commit_hash = read_commit_hash("REPLAY_BACKEND_REV")
    pipeline_json = generate_buildkite_pipeline(commit_hash)
    print(pipeline_json)


if __name__ == "__main__":
    main()
