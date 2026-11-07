#include <mcdk/log_classifier.hpp>

#include <bit>
#include <cstddef>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#include <emmintrin.h>
#define MCDK_LOG_CLASSIFIER_SSE2 1
#endif

// 一遍扫描同时找四个关键字，取代原先每个关键字各扫一遍。
namespace mcdk {

    namespace {

        // 只对字母有意义：关键字全是字母，非字母（含 UTF-8 高位字节）折叠后不会撞上。
        [[nodiscard]] inline char fold(char c) noexcept { return static_cast<char>(c | 0x20); }

        // 位置 i 上命中的关键字若比 best 优先级更高就返回它，否则返回 best。
        [[nodiscard]] inline LogLineKind
        matchAt(const char* p, std::size_t i, std::size_t n, LogLineKind best) noexcept {
            switch (fold(p[i])) {
            case 's':
                if (i + 3 <= n && fold(p[i + 1]) == 'u' && fold(p[i + 2]) == 'c') {
                    return LogLineKind::Success;
                }
                break;
            case 'e':
                if (best > LogLineKind::Error && i + 5 <= n && fold(p[i + 1]) == 'r' && fold(p[i + 2]) == 'r'
                    && fold(p[i + 3]) == 'o' && fold(p[i + 4]) == 'r') {
                    return LogLineKind::Error;
                }
                break;
            case 'w':
                if (best > LogLineKind::Warn && i + 4 <= n && fold(p[i + 1]) == 'a' && fold(p[i + 2]) == 'r'
                    && fold(p[i + 3]) == 'n') {
                    return LogLineKind::Warn;
                }
                break;
            case 'd':
                if (best > LogLineKind::Debug && i + 5 <= n && fold(p[i + 1]) == 'e' && fold(p[i + 2]) == 'b'
                    && fold(p[i + 3]) == 'u' && fold(p[i + 4]) == 'g') {
                    return LogLineKind::Debug;
                }
                break;
            default:
                break;
            }
            return best;
        }

    } // namespace

    LogLineKind classifyLogLine(std::string_view line) noexcept {
        if (line.find("[INFO][Developer]") != std::string_view::npos) {
            return LogLineKind::Developer;
        }
        const char*       p    = line.data();
        const std::size_t n    = line.size();
        LogLineKind       best = LogLineKind::Plain;
        std::size_t       i    = 0;

#ifdef MCDK_LOG_CLASSIFIER_SSE2
        // 每次看 16 个位置的前两个字母，只对 su / er / wa / de 开头的位置细比。
        const __m128i lower = _mm_set1_epi8(0x20);
        const __m128i s     = _mm_set1_epi8('s');
        const __m128i u     = _mm_set1_epi8('u');
        const __m128i e     = _mm_set1_epi8('e');
        const __m128i r     = _mm_set1_epi8('r');
        const __m128i w     = _mm_set1_epi8('w');
        const __m128i a     = _mm_set1_epi8('a');
        const __m128i d     = _mm_set1_epi8('d');
        for (; i + 17 <= n; i += 16) {
            const __m128i v0  = _mm_or_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i)), lower);
            const __m128i v1  = _mm_or_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i + 1)), lower);
            __m128i       hit = _mm_and_si128(_mm_cmpeq_epi8(v0, s), _mm_cmpeq_epi8(v1, u));
            hit = _mm_or_si128(hit, _mm_and_si128(_mm_cmpeq_epi8(v0, e), _mm_cmpeq_epi8(v1, r)));
            hit = _mm_or_si128(hit, _mm_and_si128(_mm_cmpeq_epi8(v0, w), _mm_cmpeq_epi8(v1, a)));
            hit = _mm_or_si128(hit, _mm_and_si128(_mm_cmpeq_epi8(v0, d), _mm_cmpeq_epi8(v1, e)));
            for (auto mask = static_cast<unsigned>(_mm_movemask_epi8(hit)); mask != 0; mask &= mask - 1) {
                best = matchAt(p, i + static_cast<std::size_t>(std::countr_zero(mask)), n, best);
                if (best == LogLineKind::Success) {
                    return best;
                }
            }
        }
#endif
        for (; i < n; ++i) {
            best = matchAt(p, i, n, best);
            if (best == LogLineKind::Success) {
                return best;
            }
        }
        return best;
    }

} // namespace mcdk
