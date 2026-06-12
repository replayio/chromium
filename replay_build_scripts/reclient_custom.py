def merge_reproxy_cfg(reproxy_cfg):
    # Chromium's reproxy template hardcodes the Google-internal
    # 'rbe-chrome-untrusted' instance, which autoninja refuses to use on a
    # non-corp machine. We build against EngFlow (authenticated via RBE_service
    # and mTLS certs), so the instance value is irrelevant at runtime; override
    # it to satisfy autoninja's instance check.
    reproxy_cfg['instance'] = 'projects/rbe-chromium-untrusted/instances/default_instance'
    return reproxy_cfg
