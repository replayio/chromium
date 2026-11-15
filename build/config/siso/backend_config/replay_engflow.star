# -*- bazel-starlark -*-
"""Siso backend config for Replay's EngFlow cluster.

Selected by the `reapi_backend_config_path` gclient custom_var
(replay_build_scripts/ci/gclient); the configure_siso hook copies it to
backend.star.
"""

load("@builtin//struct.star", "module")

def __platform_properties(ctx):
    # The worker image google.star names at this Chromium version, without its
    # RBE worker-pool labels.
    container_image = "docker://gcr.io/chops-public-images-prod/rbe/siso-chromium/linux@sha256:d7cb1ab14a0f20aa669c23f22c15a9dead761dcac19f43985bf9dd5f41fbef3a"
    return {
        "default": {
            "OSFamily": "Linux",
            "container-image": container_image,
        },
        "large": {
            "OSFamily": "Linux",
            "container-image": container_image,
        },
    }

backend = module(
    "backend",
    platform_properties = __platform_properties,
)
