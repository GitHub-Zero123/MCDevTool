#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace mcdk::shader_reload_support {

    inline std::string buildReloadShadersPythonCode(const nlohmann::json& shaderNames, bool checkSyntax) {
        const auto shaderNamesJson = shaderNames.dump(-1, ' ', true);
        const auto shaderNamesLiteral = nlohmann::json(shaderNamesJson).dump(-1, ' ', true);
        const auto checkSyntaxLiteral = checkSyntax ? "True" : "False";
        return R"PY(
import json

_shader_names = json.loads(u)PY" + shaderNamesLiteral + R"PY()
_check_syntax = )PY" + checkSyntaxLiteral + R"PY(

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
