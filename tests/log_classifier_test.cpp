// classifyLogLine 必须与原先「逐关键字各扫一遍」的规则逐条一致，并且更快。
#include <mcdk/log_classifier.hpp>
#include <mcdk/utils.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using mcdk::LogLineKind;

    // 原实现，原样保留作为基准。
    LogLineKind reference(std::string_view line) {
        if (line.find("[INFO][Developer]") != std::string_view::npos) {
            return LogLineKind::Developer;
        }
        if (mcdk::containsIgnoreCase(line, "SUC")) {
            return LogLineKind::Success;
        }
        if (mcdk::containsIgnoreCase(line, "ERROR")) {
            return LogLineKind::Error;
        }
        if (mcdk::containsIgnoreCase(line, "WARN")) {
            return LogLineKind::Warn;
        }
        if (mcdk::containsIgnoreCase(line, "DEBUG")) {
            return LogLineKind::Debug;
        }
        return LogLineKind::Plain;
    }

    int gMismatches = 0;

    void check(const std::string& line) {
        if (mcdk::classifyLogLine(line) != reference(line) && ++gMismatches <= 5) {
            std::cerr << "Mismatch: expected " << static_cast<int>(reference(line)) << ", got "
                      << static_cast<int>(mcdk::classifyLogLine(line)) << " for [" << line << "]\n";
        }
    }

    volatile int gSink = 0;

    template <class Classify>
    double nanosPerLine(const std::vector<std::string>& lines, Classify classify) {
        double best = 1e18;
        for (int round = 0; round < 5; ++round) {
            int        acc   = 0;
            const auto start = std::chrono::steady_clock::now();
            for (int repeat = 0; repeat < 20; ++repeat) {
                for (const auto& line : lines) {
                    acc += static_cast<int>(classify(line));
                }
            }
            const auto end = std::chrono::steady_clock::now();
            gSink          = acc;
            best = std::min(best, std::chrono::duration<double, std::nano>(end - start).count() / (20.0 * lines.size()));
        }
        return best;
    }

} // namespace

int main() {
    // --- 边界：每个关键字放在 0..40 的每个位置，覆盖 16 字节块的接缝与行尾 ---
    const std::vector<std::string> keywords = {
        "SUC", "suc", "ERROR", "eRrOr", "WARN", "warn", "DEBUG", "Debug", "ERRO", "WAR", "DEBU", "SU",
        "[INFO][Developer]", "[info][developer]",
    };
    for (const auto& keyword : keywords) {
        for (std::size_t position = 0; position <= 40; ++position) {
            check(std::string(position, 'x') + keyword);
            check(std::string(position, 'x') + keyword + std::string(7, 'y'));
            check(std::string(position, '\xE7') + keyword); // UTF-8 高位字节
        }
    }
    for (const auto* line : {"", "s", "su", "e", "err", "w", "d", "debu", "DEBUG ERROR warn suc", "warn debug"}) {
        check(line);
    }

    // --- 随机：关键字、半截关键字、大小写、中文与可打印字符混合 ---
    std::mt19937                       rng(20260928);
    const char*                        pieces[] = {"suc",   "SuC",  "Error", "ERR", "eRRoR", "warn", "WaRn",
                                                   "war",   "debug", "DEBU", "deBUG", "[INFO][Developer]",
                                                   "[info][developer]", "success", "玩家", "e", "d", "s", "w", " "};
    std::uniform_int_distribution<int> pick(0, 19), count(0, 12), printable(32, 126);
    for (int iteration = 0; iteration < 300000; ++iteration) {
        std::string line;
        for (int part = count(rng); part > 0; --part) {
            if (pick(rng) < 8) {
                line += pieces[pick(rng)];
            } else {
                for (int c = count(rng); c > 0; --c) {
                    line += static_cast<char>(printable(rng));
                }
            }
        }
        check(line);
    }

    // --- 性能：真实形态的日志行 ---
    const std::vector<std::string> samples = {
        "[2026-09-21 10:32:11][INFO][Developer] ModMain.py:142 player entered dimension 0 at (128, 64, -512)",
        "[2026-09-21 10:32:11][INFO][Python] [ModServer] on player join: steve, uid=123456, level=overworld",
        "[2026-09-21 10:32:12][INFO][Python] load config from behavior_pack/scripts/config.json finished",
        "[2026-09-21 10:32:12][INFO][Python] [Warning] deprecated api GetEngineCompFactory used in client",
        "[2026-09-21 10:32:13][INFO][Python] register success: 42 entities, 17 items, 3 blocks",
        "[2026-09-21 10:32:13][INFO][Python] tick cost 3.2ms, entities=1203, chunks loaded=256, players=1",
        "[2026-09-21 10:32:14][INFO][Python] Traceback (most recent call last): some error happened here",
        "[2026-09-21 10:32:14][INFO][Python] [debug] ui node /main_panel/button_list size=(120, 40)",
        "[2026-09-21 10:32:15][INFO][Python] 玩家进入了维度 0，坐标 (128, 64, -512)，当前在线人数 3",
        "[2026-09-21 10:32:15][INFO][Python] spawn wave 3 with 12 zombies near village center",
    };
    std::vector<std::string> lines;
    for (int index = 0; index < 1000; ++index) {
        lines.push_back(samples[index % samples.size()]);
    }
    for (const auto& line : samples) {
        check(line);
    }
    const double before = nanosPerLine(lines, reference);
    const double after  = nanosPerLine(lines, mcdk::classifyLogLine);
    std::cout << "reference      " << before << " ns/line\n";
    std::cout << "classifyLogLine " << after << " ns/line  (" << before / after << "x)\n";

    bool passed = gMismatches == 0;
    if (!passed) {
        std::cerr << "Failed: " << gMismatches << " lines classified differently from the reference\n";
    }
    if (after >= before) {
        std::cerr << "Failed: classifyLogLine is not faster than the reference\n";
        passed = false;
    }
    std::cout << (passed ? "log_classifier_test passed\n" : "log_classifier_test failed\n");
    return passed ? 0 : 1;
}
