#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace mcdk::material_reload_support {

    inline std::string buildReloadMaterialsPythonCode(const nlohmann::json& materialPaths, bool checkSyntax) {
        const auto materialPathsJson = materialPaths.dump(-1, ' ', true);
        const auto materialPathsLiteral = nlohmann::json(materialPathsJson).dump(-1, ' ', true);
        const auto checkSyntaxLiteral = checkSyntax ? "True" : "False";
        return R"PY(
import json

_material_paths = json.loads(u)PY" + materialPathsLiteral + R"PY()
_check_syntax = )PY" + checkSyntaxLiteral + R"PY(

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
