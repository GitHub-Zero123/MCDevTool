#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace mcdk::particle_reload_support {

    inline std::string buildReloadParticlesPythonCode(const nlohmann::json& particlePaths) {
        const auto particlePathsJson = particlePaths.dump(-1, ' ', true);
        const auto particlePathsLiteral = nlohmann::json(particlePathsJson).dump(-1, ' ', true);
        return R"PY(
import json

_particle_paths = json.loads(u)PY" + particlePathsLiteral + R"PY()

try:
    import _particle_system
    result = {
        'ok': True,
        'attempted': 0,
        'reloaded': 0,
        'failed': [],
        'unsupported': False,
    }
    if not hasattr(_particle_system, 'load'):
        result['ok'] = False
        result['unsupported'] = True
        result['error'] = '_particle_system.load is not available; particle hot reload is not supported.'
    else:
        for particle_path in _particle_paths:
            result['attempted'] += 1
            try:
                if not particle_path.startswith('particles/'):
                    particle_path = 'particles/' + particle_path
                ok = _particle_system.load(particle_path)
                if ok:
                    result['reloaded'] += 1
                else:
                    result['failed'].append({'particle': particle_path, 'error': '_particle_system.load returned False'})
            except Exception as exc:
                result['failed'].append({'particle': particle_path, 'error': repr(exc)})
    _result = json.dumps(result, ensure_ascii=False)
except ImportError as exc:
    _result = json.dumps({
        'ok': False,
        'unsupported': True,
        'error': repr(exc),
    }, ensure_ascii=False)
except Exception as exc:
    import traceback
    _result = json.dumps({
        'ok': False,
        'error': repr(exc),
        'trace': traceback.format_exc(),
    }, ensure_ascii=False)
)PY";
    }

} // namespace mcdk::particle_reload_support
