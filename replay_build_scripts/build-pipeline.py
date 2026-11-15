"""Pipeline for the `runtime-build-and-test` Buildkite pipeline: nothing.

That pipeline runs this script on every push and uploads what it prints. On
master it triggers the driver build and `chromium-build`. Chromium 157 builds
are created on `chromium-build` directly (.buildkite/pipeline.yml), so a push
to this branch must trigger neither: the only step is a `wait`, which needs no
agent and runs nothing.
"""

import json


def main():
    print(json.dumps({"steps": ["wait"]}, indent=4))


if __name__ == "__main__":
    main()
