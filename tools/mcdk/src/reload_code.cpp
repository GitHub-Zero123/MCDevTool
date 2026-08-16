#include <mcdk/material_reload_support.hpp>
#include <mcdk/particle_reload_support.hpp>
#include <mcdk/shader_reload_support.hpp>

#include <nlohmann/json.hpp>

namespace {
    std::string jsonArrayLiteral(const std::vector<std::string>& values) {
        const auto jsonText = nlohmann::json(values).dump(-1, ' ', true);
        return nlohmann::json(jsonText).dump(-1, ' ', true);
    }
} // namespace

namespace mcdk::shader_reload_support {

    std::string buildReloadShadersPythonCode(const std::vector<std::string>& shaderNames, bool checkSyntax) {
        const auto shaderNamesLiteral = jsonArrayLiteral(shaderNames);
        const auto checkSyntaxLiteral = checkSyntax ? "True" : "False";
        return R"PY(
import json

_shader_names = json.loads(u)PY"
             + shaderNamesLiteral + R"PY()
_check_syntax = )PY"
             + checkSyntaxLiteral + R"PY(

try:
    import clientlevel
    result = {
        'ok': True,
        'attempted': 0,
        'reloaded': 0,
        'failed': [],
    }
    for shader_name in _shader_names:
        result['attempted'] += 1
        try:
            ok = clientlevel.reload_one_shader(shader_name, _check_syntax)
            if ok:
                result['reloaded'] += 1
            else:
                result['failed'].append({'shader': shader_name, 'error': 'reload_one_shader returned False'})
        except Exception as exc:
            result['failed'].append({'shader': shader_name, 'error': repr(exc)})
    _result = json.dumps(result, ensure_ascii=False)
except Exception as exc:
    import traceback
    _result = json.dumps({
        'ok': False,
        'error': repr(exc),
        'trace': traceback.format_exc(),
    }, ensure_ascii=False)
)PY";
    }

} // namespace mcdk::shader_reload_support

namespace mcdk::material_reload_support {

    std::string buildReloadMaterialsPythonCode(const std::vector<std::string>& materialPaths, bool checkSyntax) {
        const auto materialPathsLiteral = jsonArrayLiteral(materialPaths);
        const auto checkSyntaxLiteral   = checkSyntax ? "True" : "False";
        return R"PY(
import json

_material_paths = json.loads(u)PY"
             + materialPathsLiteral + R"PY()
_check_syntax = )PY"
             + checkSyntaxLiteral + R"PY(

try:
    import clientlevel
    result = {
        'ok': True,
        'attempted': 0,
        'reloaded': 0,
        'failed': [],
        'unsupported': False,
    }
    if not hasattr(clientlevel, 'reload_one_material_file'):
        result['ok'] = False
        result['unsupported'] = True
        result['error'] = 'clientlevel.reload_one_material_file is not available; material hot reload requires MC 3.9 or newer.'
    else:
        for material_path in _material_paths:
            result['attempted'] += 1
            try:
                if not material_path.startswith('materials/'):
                    material_path = 'materials/' + material_path
                ok = clientlevel.reload_one_material_file(material_path, _check_syntax)
                if ok:
                    result['reloaded'] += 1
                else:
                    result['failed'].append({'material': material_path, 'error': 'reload_one_material_file returned False'})
            except Exception as exc:
                result['failed'].append({'material': material_path, 'error': repr(exc)})
    _result = json.dumps(result, ensure_ascii=False)
except Exception as exc:
    import traceback
    _result = json.dumps({
        'ok': False,
        'error': repr(exc),
        'trace': traceback.format_exc(),
    }, ensure_ascii=False)
)PY";
    }

} // namespace mcdk::material_reload_support

namespace mcdk::particle_reload_support {

    std::string buildReloadParticlesPythonCode(const std::vector<std::string>& particlePaths) {
        const auto particlePathsLiteral = jsonArrayLiteral(particlePaths);
        return R"PY(
import json

_particle_paths = json.loads(u)PY"
             + particlePathsLiteral + R"PY()

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
