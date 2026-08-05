#include "result_builder.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "TracyWorker.hpp"

namespace mcdev::tracy_bridge {
namespace {

constexpr std::size_t MaximumThreads = 64;
constexpr std::size_t MaximumCallDepth = 64;
constexpr std::string_view DebugEnvironmentSource = "DEBUG_ENV_SCRIPT";

struct ZoneResult {
    std::string name;
    std::string sourceFile;
    std::uint32_t sourceLine;
    std::string threadId;
    std::string threadName;
    std::uint64_t calls;
    std::int64_t total;
    std::int64_t self;
    std::int64_t mean;
    std::int64_t maximum;
};

struct CallNode {
    std::uint32_t id;
    std::int16_t sourceLocation;
    std::string name;
    std::string sourceFile;
    std::uint32_t sourceLine;
    std::uint64_t calls = 0;
    std::int64_t total = 0;
    std::int64_t self = 0;
    std::int64_t maximum = 0;
    std::vector<CallNode> children;
};

struct ThreadResult {
    std::string id;
    std::string name;
    std::uint64_t calls = 0;
    std::int64_t total = 0;
    std::vector<CallNode> roots;
};

struct CallTreeState {
    std::size_t nodes = 0;
    std::size_t maximumNodes;
    std::uint32_t nextId = 0;
    bool truncated = false;
};

struct ZoneAggregate {
    std::uint64_t calls = 0;
    std::int64_t total = 0;
    std::int64_t self = 0;
    std::int64_t maximum = 0;
};

bool isIgnoredSourceFile(std::string_view sourceFile) {
    if (sourceFile == DebugEnvironmentSource) return true;
    if (!sourceFile.starts_with(DebugEnvironmentSource) || sourceFile.size() <= DebugEnvironmentSource.size()) {
        return false;
    }
    const auto separator = sourceFile[DebugEnvironmentSource.size()];
    return separator == '.' || separator == '/' || separator == '\\';
}

void appendJsonString(std::ostringstream& output, std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) {
                output << "\\u00" << hex[character >> 4] << hex[character & 0x0f];
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
}

template<typename Callback>
void forEachZone(const tracy::Vector<tracy::short_ptr<tracy::ZoneEvent>>& zones, Callback&& callback) {
    if (zones.is_magic()) {
        const auto& direct = *reinterpret_cast<const tracy::Vector<tracy::ZoneEvent>*>(&zones);
        for (const auto& zone : direct) callback(zone);
    } else {
        for (const auto& zone : zones) callback(*zone);
    }
}

std::int64_t zoneSelfTime(tracy::Worker& worker, const tracy::ZoneEvent& zone, std::int64_t duration) {
    std::int64_t childrenTotal = 0;
    if (zone.HasChildren()) {
        const auto& children = worker.GetZoneChildren(zone.Child());
        forEachZone(children, [&](const tracy::ZoneEvent& child) {
            childrenTotal += std::max<std::int64_t>(0, worker.GetZoneEnd(child) - child.Start());
        });
    }
    return std::max<std::int64_t>(0, duration - childrenTotal);
}

CallNode* findOrAddCallNode(
    tracy::Worker& worker,
    const tracy::ZoneEvent& zone,
    std::vector<CallNode>& siblings,
    CallTreeState& state
) {
    const auto sourceLocation = zone.SrcLoc();
    const char* rawName = worker.GetZoneName(zone);
    const std::string_view name = rawName ? rawName : "";
    const auto found = std::find_if(siblings.begin(), siblings.end(), [&](const CallNode& node) {
        return node.sourceLocation == sourceLocation && node.name == name;
    });
    if (found != siblings.end()) return &*found;
    if (state.nodes >= state.maximumNodes) {
        state.truncated = true;
        return nullptr;
    }

    const auto& source = worker.GetSourceLocation(sourceLocation);
    const char* rawFile = worker.GetString(source.file);
    siblings.push_back(CallNode{
        state.nextId++,
        sourceLocation,
        std::string(name),
        rawFile ? rawFile : "",
        source.line
    });
    ++state.nodes;
    return &siblings.back();
}

std::int64_t collectCallTreeZone(
    tracy::Worker& worker,
    const tracy::ZoneEvent& zone,
    std::vector<CallNode>* siblings,
    ThreadResult& thread,
    CallTreeState& state,
    std::size_t depth
) {
    const auto duration = std::max<std::int64_t>(0, worker.GetZoneEnd(zone) - zone.Start());
    const auto& source = worker.GetSourceLocation(zone.SrcLoc());
    const char* rawSourceFile = worker.GetString(source.file);
    if (isIgnoredSourceFile(rawSourceFile ? rawSourceFile : "")) {
        if (siblings && zone.HasChildren()) {
            const auto& children = worker.GetZoneChildren(zone.Child());
            forEachZone(children, [&](const tracy::ZoneEvent& child) {
                collectCallTreeZone(worker, child, siblings, thread, state, depth);
            });
        }
        return duration;
    }

    ++thread.calls;
    if (!siblings || depth >= MaximumCallDepth) {
        state.truncated = true;
        return duration;
    }
    CallNode* node = findOrAddCallNode(worker, zone, *siblings, state);
    if (!node) return duration;

    std::int64_t childrenTotal = 0;
    if (zone.HasChildren()) {
        const auto& children = worker.GetZoneChildren(zone.Child());
        forEachZone(children, [&](const tracy::ZoneEvent& child) {
            childrenTotal += collectCallTreeZone(
                worker,
                child,
                &node->children,
                thread,
                state,
                depth + 1
            );
        });
    }

    if (node) {
        ++node->calls;
        node->total += duration;
        node->self += std::max<std::int64_t>(0, duration - childrenTotal);
        node->maximum = std::max(node->maximum, duration);
    }
    return duration;
}

void sortCallNodes(std::vector<CallNode>& nodes) {
    std::sort(nodes.begin(), nodes.end(), [](const CallNode& left, const CallNode& right) {
        return left.total > right.total;
    });
    for (auto& node : nodes) sortCallNodes(node.children);
}

std::vector<ThreadResult> buildCallTree(
    tracy::Worker& worker,
    std::uint32_t maximumZones,
    bool& truncated
) {
    std::vector<const tracy::ThreadData*> sourceThreads;
    for (const auto* thread : worker.GetThreadData()) {
        if (thread && !thread->timeline.empty()) sourceThreads.push_back(thread);
    }
    std::sort(sourceThreads.begin(), sourceThreads.end(), [](const auto* left, const auto* right) {
        return left->count > right->count;
    });
    if (sourceThreads.size() > MaximumThreads) {
        sourceThreads.resize(MaximumThreads);
        truncated = true;
    }

    const auto maximumCallNodes = std::max<std::size_t>({
        sourceThreads.size(),
        maximumZones,
        maximumZones * 8ull
    });
    std::size_t remainingNodes = maximumCallNodes;
    std::uint32_t nextNodeId = 0;
    std::vector<ThreadResult> threads;
    threads.reserve(sourceThreads.size());
    for (std::size_t threadIndex = 0; threadIndex < sourceThreads.size(); ++threadIndex) {
        const auto* sourceThread = sourceThreads[threadIndex];
        const auto remainingThreads = sourceThreads.size() - threadIndex;
        // Reserve a fair share for every active thread instead of letting one busy pool exhaust the tree.
        const auto threadNodeBudget = std::max<std::size_t>(1, remainingNodes / remainingThreads);
        CallTreeState state{0, threadNodeBudget, nextNodeId, false};
        const char* rawName = worker.GetThreadName(sourceThread->id);
        ThreadResult thread{std::to_string(sourceThread->id), rawName ? rawName : ""};
        forEachZone(sourceThread->timeline, [&](const tracy::ZoneEvent& zone) {
            collectCallTreeZone(worker, zone, &thread.roots, thread, state, 0);
        });
        nextNodeId = state.nextId;
        remainingNodes -= state.nodes;
        truncated = truncated || state.truncated;
        if (thread.roots.empty()) continue;
        sortCallNodes(thread.roots);
        for (const auto& root : thread.roots) thread.total += root.total;
        threads.push_back(std::move(thread));
    }
    std::sort(threads.begin(), threads.end(), [](const ThreadResult& left, const ThreadResult& right) {
        return left.total > right.total;
    });
    return threads;
}

void appendCallNode(std::ostringstream& output, const CallNode& node) {
    output << "{\"id\":" << node.id << ",\"name\":";
    appendJsonString(output, node.name);
    output << ",\"sourceFile\":";
    appendJsonString(output, node.sourceFile);
    output << ",\"sourceLine\":" << node.sourceLine
           << ",\"calls\":" << node.calls
           << ",\"totalNanoseconds\":" << std::max<std::int64_t>(0, node.total)
           << ",\"selfNanoseconds\":" << std::max<std::int64_t>(0, node.self)
           << ",\"meanNanoseconds\":" << (node.calls == 0 ? 0 : node.total / static_cast<std::int64_t>(node.calls))
           << ",\"maximumNanoseconds\":" << std::max<std::int64_t>(0, node.maximum)
           << ",\"children\":[";
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        if (index != 0) output << ',';
        appendCallNode(output, node.children[index]);
    }
    output << "]}";
}

} // namespace

std::string buildResultJson(
    tracy::Worker& worker,
    double capturedSeconds,
    std::uint32_t maximumZones
) {
    std::vector<ZoneResult> zones;
    const auto& sourceZones = worker.GetSourceLocationZones();
    zones.reserve(maximumZones);
    std::uint64_t totalZones = 0;
    const auto smallestFirst = [](const ZoneResult& left, const ZoneResult& right) {
        return left.total > right.total;
    };
    const auto addZone = [&](ZoneResult result) {
        if (result.total <= 0) return;
        ++totalZones;
        if (maximumZones == 0 || (zones.size() == maximumZones && result.total <= zones.front().total)) {
            return;
        }
        if (zones.size() == maximumZones) {
            std::pop_heap(zones.begin(), zones.end(), smallestFirst);
            zones.back() = std::move(result);
            std::push_heap(zones.begin(), zones.end(), smallestFirst);
        } else {
            zones.push_back(std::move(result));
            std::push_heap(zones.begin(), zones.end(), smallestFirst);
        }
    };

    for (const auto& entry : sourceZones) {
        const auto& data = entry.second;
        if (data.total == 0 || data.zones.empty()) continue;
        const auto& source = worker.GetSourceLocation(entry.first);
        const char* name = worker.GetString(source.name.active ? source.name : source.function);
        const char* file = worker.GetString(source.file);
        if (isIgnoredSourceFile(file ? file : "")) continue;

        const auto makeZone = [&](std::uint16_t compressedThread, const ZoneAggregate& aggregate) {
            const auto threadId = worker.DecompressThread(compressedThread);
            const char* threadName = worker.GetThreadName(threadId);
            addZone(ZoneResult{
                name ? name : "",
                file ? file : "",
                source.line,
                std::to_string(threadId),
                threadName ? threadName : "",
                aggregate.calls,
                aggregate.total,
                aggregate.self,
                aggregate.calls == 0 ? 0 : aggregate.total / static_cast<std::int64_t>(aggregate.calls),
                aggregate.maximum
            });
        };

        if (data.threadCnt.size() == 1) {
            makeZone(data.threadCnt.begin()->first, ZoneAggregate{
                static_cast<std::uint64_t>(data.zones.size()),
                data.total,
                data.selfTotal,
                data.max
            });
            continue;
        }

        std::unordered_map<std::uint16_t, ZoneAggregate> threadAggregates;
        threadAggregates.reserve(data.threadCnt.size());
        for (const auto& zoneThread : data.zones) {
            const auto* zone = zoneThread.Zone();
            if (!zone) continue;
            const auto duration = std::max<std::int64_t>(0, worker.GetZoneEnd(*zone) - zone->Start());
            auto& aggregate = threadAggregates[zoneThread.Thread()];
            ++aggregate.calls;
            aggregate.total += duration;
            aggregate.self += zoneSelfTime(worker, *zone, duration);
            aggregate.maximum = std::max(aggregate.maximum, duration);
        }
        for (const auto& [thread, aggregate] : threadAggregates) makeZone(thread, aggregate);
    }

    std::sort(zones.begin(), zones.end(), [](const ZoneResult& left, const ZoneResult& right) {
        return left.total > right.total;
    });

    bool callTreeTruncated = false;
    auto threads = buildCallTree(worker, maximumZones, callTreeTruncated);
    std::ostringstream output;
    output << std::setprecision(15)
           << "{\"capturedSeconds\":" << capturedSeconds
           << ",\"totalZones\":" << totalZones
           << ",\"truncated\":" << (totalZones > zones.size() ? "true" : "false")
           << ",\"callTreeTruncated\":" << (callTreeTruncated ? "true" : "false")
           << ",\"zones\":[";
    for (std::size_t index = 0; index < zones.size(); ++index) {
        if (index != 0) output << ',';
        const auto& zone = zones[index];
        output << "{\"id\":" << index << ",\"name\":";
        appendJsonString(output, zone.name);
        output << ",\"sourceFile\":";
        appendJsonString(output, zone.sourceFile);
        output << ",\"sourceLine\":" << zone.sourceLine << ",\"threadId\":";
        appendJsonString(output, zone.threadId);
        output << ",\"threadName\":";
        appendJsonString(output, zone.threadName);
        output << ",\"calls\":" << zone.calls
               << ",\"totalNanoseconds\":" << std::max<std::int64_t>(0, zone.total)
               << ",\"selfNanoseconds\":" << std::max<std::int64_t>(0, zone.self)
               << ",\"meanNanoseconds\":" << std::max<std::int64_t>(0, zone.mean)
               << ",\"maximumNanoseconds\":" << std::max<std::int64_t>(0, zone.maximum)
               << '}';
    }
    output << "],\"threads\":[";
    for (std::size_t threadIndex = 0; threadIndex < threads.size(); ++threadIndex) {
        if (threadIndex != 0) output << ',';
        const auto& thread = threads[threadIndex];
        output << "{\"id\":";
        appendJsonString(output, thread.id);
        output << ",\"name\":";
        appendJsonString(output, thread.name);
        output << ",\"calls\":" << thread.calls
               << ",\"totalNanoseconds\":" << std::max<std::int64_t>(0, thread.total)
               << ",\"roots\":[";
        for (std::size_t rootIndex = 0; rootIndex < thread.roots.size(); ++rootIndex) {
            if (rootIndex != 0) output << ',';
            appendCallNode(output, thread.roots[rootIndex]);
        }
        output << "]}";
    }
    output << "]}";
    return output.str();
}

} // namespace mcdev::tracy_bridge
