#pragma once

#include <string>
#include <vector>

namespace mcdk::particle_reload_support {

    [[nodiscard]] std::string buildReloadParticlesPythonCode(const std::vector<std::string>& particlePaths);

} // namespace mcdk::particle_reload_support
