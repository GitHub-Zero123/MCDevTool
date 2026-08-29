#include <mcdk/port_range.hpp>

#include <iostream>
#include <string>

namespace {

    bool expect(bool condition, const std::string& description) {
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << description << '\n';
        return condition;
    }

    bool expectRange(const mcdk::PortRange& range, int begin, int end, const std::string& description) {
        const bool ok = range.begin == begin && range.end == end;
        if (!ok) {
            std::cout << "       expected " << begin << "-" << end << ", got " << range.toString() << '\n';
        }
        return expect(ok, description);
    }

    // 旧版本只写单值端口，这条兼容性必须始终成立。
    bool testSingleValueCompatibility() {
        bool passed = true;

        const auto parsed = mcdk::parsePortRange("19133");
        passed            = expect(parsed.ok, "单值端口解析成功") && passed;
        passed            = expectRange(parsed.range, 19133, 19133, "单值端口退化为 begin == end") && passed;
        passed            = expect(parsed.range.isSingle(), "单值端口 isSingle() 为真") && passed;
        passed            = expect(parsed.range.size() == 1, "单值端口只展开出一个端口") && passed;
        passed            = expect(parsed.range.toString() == "19133", "单值端口输出不带区间连字符") && passed;
        passed            = expect(parsed.warning.empty(), "单值端口不产生归一化提示") && passed;

        const auto viaMake = mcdk::makePortRange(19133, 19133);
        passed             = expectRange(viaMake.range, 19133, 19133, "makePortRange 同值等价于单值端口") && passed;

        passed = expectRange(mcdk::PortRange::single(19133), 19133, 19133, "PortRange::single 构造单值区间") && passed;
        return passed;
    }

    bool testRangeForms() {
        bool passed = true;

        passed = expectRange(mcdk::parsePortRange("19133-19142").range, 19133, 19142, "连字符区间") && passed;
        passed = expectRange(mcdk::parsePortRange("19133..19142").range, 19133, 19142, "双点区间") && passed;
        passed = expectRange(mcdk::parsePortRange("19133:19142").range, 19133, 19142, "冒号区间") && passed;
        passed = expectRange(mcdk::parsePortRange("  19133 - 19142  ").range, 19133, 19142, "区间两侧空白被忽略")
              && passed;
        passed = expect(mcdk::parsePortRange("19133-19142").range.toString() == "19133-19142", "区间文本输出")
              && passed;
        return passed;
    }

    // 用户可能刻意先写大后写小，必须归一化而不是报错。
    bool testReversedOrder() {
        bool passed = true;

        const auto parsed = mcdk::parsePortRange("19142-19133");
        passed            = expect(parsed.ok, "顺序颠倒的区间仍然解析成功") && passed;
        passed            = expectRange(parsed.range, 19133, 19142, "顺序颠倒的区间被交换为升序") && passed;
        passed            = expect(!parsed.warning.empty(), "顺序颠倒时给出提示") && passed;

        const auto viaMake = mcdk::makePortRange(19142, 19133);
        passed             = expectRange(viaMake.range, 19133, 19142, "makePortRange 同样处理顺序颠倒") && passed;
        return passed;
    }

    bool testClampAndSpanCap() {
        bool passed = true;

        const auto tooHigh = mcdk::makePortRange(65530, 70000);
        passed             = expectRange(tooHigh.range, 65530, 65535, "上界越界被钳制到 65535") && passed;
        passed             = expect(!tooHigh.warning.empty(), "越界时给出提示") && passed;

        const auto tooLow = mcdk::makePortRange(0, 10);
        passed            = expectRange(tooLow.range, 1, 10, "下界越界被钳制到 1") && passed;

        const auto tooWide = mcdk::makePortRange(1024, 65535);
        passed             = expectRange(tooWide.range, 1024, 1024 + mcdk::PortRange::MaxSpan - 1, "超宽区间被截断")
              && passed;
        passed = expect(tooWide.range.size() == mcdk::PortRange::MaxSpan, "截断后跨度等于上限") && passed;
        passed = expect(!tooWide.warning.empty(), "截断时给出提示") && passed;
        return passed;
    }

    bool testInvalidInput() {
        bool passed = true;

        const auto fallback = mcdk::PortRange::single(19133);
        for (const auto* text : {"", "   ", "abc", "19133-", "-19133", "19133-abc", "19133 19142", "1.9"}) {
            const auto parsed = mcdk::parsePortRange(text, fallback);
            passed            = expect(!parsed.ok, std::string("非法输入被拒绝：\"") + text + "\"") && passed;
            passed            = expect(!parsed.error.empty(), std::string("非法输入给出错误原因：\"") + text + "\"")
                  && passed;
            passed = expectRange(parsed.range, 19133, 19133, std::string("非法输入回退到 fallback：\"") + text + "\"")
                  && passed;
        }
        return passed;
    }

    bool testContainsAndPorts() {
        bool passed = true;

        const auto range = mcdk::PortRange::normalized(19133, 19135);
        passed           = expect(range.contains(19133) && range.contains(19135), "区间包含端点") && passed;
        passed           = expect(!range.contains(19132) && !range.contains(19136), "区间不包含端点外的值") && passed;

        const auto ports = range.ports();
        passed           = expect(ports.size() == 3, "区间展开出正确数量的端口") && passed;
        passed = expect(ports.front() == 19133 && ports.back() == 19135, "区间按升序展开") && passed;
        return passed;
    }

} // namespace

int main() {
    bool passed = testSingleValueCompatibility();
    passed      = testRangeForms() && passed;
    passed      = testReversedOrder() && passed;
    passed      = testClampAndSpanCap() && passed;
    passed      = testInvalidInput() && passed;
    passed      = testContainsAndPorts() && passed;
    return passed ? 0 : 1;
}
