# What the Chromium step's commands outside the build image run in
# (in-tools-image.sh): the Node of sync-repos.mjs and upload_build_artifacts.mjs,
# and the tools they call. An agent then needs docker, git, the aws CLI and
# buildkite-agent.
FROM node:26.12.1-bookworm-slim@sha256:88e480776c6149ff1d098cd304f44bf18057fb5d284a3329b78162d29821e28c
RUN apt-get update && \
    apt-get install -y --no-install-recommends awscli binutils ca-certificates curl git openssh-client xz-utils && \
    rm -rf /var/lib/apt/lists/*
