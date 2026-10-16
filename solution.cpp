// Standalone online solution: GCC C++20 / C++17. No test files are read.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace pipeline {

constexpr int MAX_PIPES = 200000;
constexpr int MAX_TRENCH = 100000000;

// The benchmark can measure the two improvements independently.
// The submitted program always uses both.
struct Options { bool cohorts = true; bool coalescence = true; };

class Pipeline {
    struct Pipe { int position, length; };
    Options options;
    std::map<int, int> byPosition;          // Free intervals: start -> length.
    std::set<std::pair<int, int>> byLength; // The same intervals: (length, start).
    std::map<int, int> live;                // Occupied interval: start -> PID.
    std::vector<Pipe> pipes{{-1, 0}};
    std::array<int, 41> arrived{}, removed{};
    int trenchLength = 0, occupied = 0;

    // A batch is a run of positive commands, followed by removals.
    int batchFirst = 1, batchSize = 0, batchLength = 0;
    int removedOdd = 0, removedEven = 0, removedOutside = 0;
    int burstSize = 0, burstInitialAlive = 0;
    int expectedBatch = 0, coalescenceBudget = 0;
    bool deleting = false;

    void insertGap(int start, int length) {
        if (length <= 0) return;
        byPosition.emplace(start, length);
        byLength.emplace(length, start);
    }
    void eraseGap(int start, int length) {
        byPosition.erase(start);
        byLength.erase({length, start});
    }
    void observeArrival(int length) {
        if (deleting) {
            // Preserve merging opportunities temporarily after a removal burst.
            if (burstSize >= 3 && 4 * burstSize >= burstInitialAlive)
                coalescenceBudget = burstInitialAlive;
            // Learn alternating removals only from a completed homogeneous batch.
            const int sameParity = std::max(removedOdd, removedEven);
            expectedBatch = batchLength > 0 && batchSize >= 32 &&
                sameParity * 3 >= batchSize && std::min(removedOdd, removedEven) == 0 &&
                removedOutside == 0 ? batchSize : 0;
            batchFirst = static_cast<int>(pipes.size());
            batchSize = 0;
            batchLength = length;
            removedOdd = removedEven = removedOutside = 0;
            deleting = false;
        }
        if (batchSize == 0) batchLength = length;
        else if (batchLength != length) batchLength = -1;
        ++batchSize;
    }
    double neighborRisk(int start) const {
        double risk = 0.0;
        const auto right = live.lower_bound(start);
        if (right != live.begin()) {
            const int length = pipes[std::prev(right)->second].length;
            risk += static_cast<double>(removed[length]) / arrived[length];
        }
        if (right != live.end()) {
            const int length = pipes[right->second].length;
            risk += static_cast<double>(removed[length]) / arrived[length];
        }
        return risk;
    }
    auto chooseGap(int length, bool preserveNeighbors) const {
        auto best = byLength.lower_bound({length, 0});
        if (!preserveNeighbors || best == byLength.end()) return best;
        // Each neighbor contributes at most 5 m of penalty. A gap more than
        // 10 m longer than the smallest adequate gap cannot improve its score.
        const int limit = best->first + 10;
        double bestScore = 1e100;
        for (auto it = best; it != byLength.end() && it->first <= limit;) {
            const double score = it->first - length + 5.0 * neighborRisk(it->second);
            if (score < bestScore - 1e-12) { bestScore = score; best = it; }
            // Only the leftmost gap of each length; at most 11 candidates.
            it = byLength.lower_bound({it->first + 1, 0});
        }
        return best;
    }

public:
    explicit Pipeline(Options settings = {}) : options(settings) {}
    int trench() const { return trenchLength; }
    int occupiedMeters() const { return occupied; }

    std::pair<int, int> addPipe(int length) {
        if (length < 1 || length > 40 || pipes.size() > MAX_PIPES)
            throw std::invalid_argument("Invalid pipe length or too many pipes");
        observeArrival(length);
        const bool cohort = options.cohorts && expectedBatch > 0 && batchLength > 0 &&
                            batchSize <= expectedBatch;
        const bool preserve = options.coalescence && coalescenceBudget > 0 && !cohort;
        const bool fromRight = cohort && batchSize % 2 == 0;
        const auto best = chooseGap(length, preserve);
        int start, gapLength, digs = 0;
        if (best != byLength.end()) {
            gapLength = best->first;
            start = best->second;
            eraseGap(start, gapLength);
        } else {
            start = trenchLength;
            if (!byPosition.empty()) {
                const auto tail = std::prev(byPosition.end());
                if (tail->first + tail->second == trenchLength) {
                    start = tail->first;
                    eraseGap(tail->first, tail->second);
                }
            }
            const int tailLength = trenchLength - start;
            int target = length;
            if (cohort) {
                // The previous batch is an estimate, not a promise about future
                // commands. Grow the two-ended arena gradually. Extra free space
                // is capped at 10% of the old trench (minimum 20 m), plus at most
                // 19 m from rounding to full digging sections.
                const int forecast = std::min({expectedBatch - batchSize + 1,
                                               batchSize * 2 - 1, 256});
                target = std::min(forecast * length,
                                  length + std::max(20, trenchLength / 10));
            }
            digs = (target - tailLength + 19) / 20;
            if (trenchLength + digs * 20 > MAX_TRENCH)
                throw std::runtime_error("Trench length limit exceeded");
            trenchLength += digs * 20;
            gapLength = trenchLength - start;
        }
        const int position = fromRight ? start + gapLength - length : start;
        insertGap(fromRight ? start : start + length, gapLength - length);
        live.emplace(position, static_cast<int>(pipes.size()));
        pipes.push_back({position, length});
        ++arrived[length];
        occupied += length;
        if (coalescenceBudget > 0) --coalescenceBudget;
        return {digs, position};
    }

    int removePipe(int pid) {
        if (pid <= 0 || pid >= static_cast<int>(pipes.size()) || pipes[pid].length == 0)
            throw std::invalid_argument("Unknown or already removed PID");
        if (!deleting) {
            deleting = true;
            removedOdd = removedEven = removedOutside = burstSize = 0;
            burstInitialAlive = static_cast<int>(live.size());
        }
        ++burstSize;
        if (pid >= batchFirst && pid < batchFirst + batchSize) {
            if ((pid - batchFirst) % 2 == 0) ++removedOdd;
            else ++removedEven;
        } else ++removedOutside;
        const int position = pipes[pid].position;
        int start = position, length = pipes[pid].length;
        ++removed[length];
        occupied -= length;
        live.erase(position);
        auto right = byPosition.lower_bound(start);
        if (right != byPosition.begin()) {
            const auto left = std::prev(right);
            if (left->first + left->second == start) {
                start = left->first;
                length += left->second;
                eraseGap(left->first, left->second);
            }
        }
        if (right != byPosition.end() && start + length == right->first) {
            length += right->second;
            eraseGap(right->first, right->second);
        }
        insertGap(start, length);
        pipes[pid].length = 0;
        return position;
    }

    // Used only by tests; no scans in the interactive loop.
    bool checkInvariants() const {
        if (byPosition.size() != byLength.size()) return false;
        int freeMeters = 0, previousEnd = -1, activeMeters = 0;
        for (const auto& [start, length] : byPosition) {
            if (start <= previousEnd || length <= 0 || start + length > trenchLength ||
                byLength.count({length, start}) != 1) return false;
            previousEnd = start + length;
            freeMeters += length;
        }
        previousEnd = 0;
        for (const auto& [start, pid] : live) {
            const auto pipe = pipes[pid];
            if (pipe.position != start || pipe.length <= 0 || start < previousEnd ||
                start + pipe.length > trenchLength) return false;
            previousEnd = start + pipe.length;
            activeMeters += pipe.length;
        }
        return freeMeters + occupied == trenchLength && activeMeters == occupied;
    }
};

// Native reads return available bytes. fread(buffer, 4096, ...) would wait for
// more commands and can deadlock an interactive request/reply protocol.
class CommandReader {
    std::array<unsigned char, 4096> buffer{};
    int size = 0;
    int offset = 0;
#ifdef _WIN32
    const HANDLE handle = ::GetStdHandle(STD_INPUT_HANDLE);
#endif

    int nextByte() {
        if (offset == size) {
#ifdef _WIN32
            DWORD received = 0;
            if (!::ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &received, nullptr)) {
                if (::GetLastError() != ERROR_BROKEN_PIPE) throw std::runtime_error("Cannot read input");
                received = 0;
            }
            size = static_cast<int>(received);
#else
            do {
                size = static_cast<int>(::read(0, buffer.data(), buffer.size()));
            } while (size < 0 && errno == EINTR);
#endif
            offset = 0;
            if (size < 0) throw std::runtime_error("Cannot read input");
            if (size == 0) return -1;
        }
        return buffer[offset++];
    }

public:
    CommandReader() {
#ifdef _WIN32
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Invalid input handle");
#endif
    }
    std::optional<int> nextCommand() {
        int byte = nextByte();
        while (byte >= 0 && byte <= 32) byte = nextByte();
        if (byte < 0) return std::nullopt;
        int sign = 1;
        if (byte == '-' || byte == '+') {
            if (byte == '-') sign = -1;
            byte = nextByte();
        }
        if (byte < '0' || byte > '9') throw std::invalid_argument("Expected an integer command");
        int value = 0;
        while (byte >= '0' && byte <= '9') {
            value = value * 10 + byte - '0';
            if (value > MAX_PIPES) throw std::invalid_argument("Command exceeds PID/length limits");
            byte = nextByte();
        }
        if (byte > 32) throw std::invalid_argument("Expected whitespace after a command");
        return sign * value;
    }
};

class ReplyWriter {
#ifdef _WIN32
    const HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
#endif
    void writeAll(const char* buffer, int size) const {
        int offset = 0;
        while (offset < size) {
#ifdef _WIN32
            DWORD written = 0;
            if (!::WriteFile(handle, buffer + offset, static_cast<DWORD>(size - offset), &written, nullptr)) {
                throw std::runtime_error("Cannot write the interactive reply");
            }
#else
            const auto written = ::write(1, buffer + offset, static_cast<std::size_t>(size - offset));
            if (written < 0 && errno == EINTR) continue;
#endif
            if (written <= 0) throw std::runtime_error("Cannot write the interactive reply");
            offset += static_cast<int>(written);
        }
    }

    static void appendNumber(char*& target, unsigned value) {
        char reversed[10];
        int count = 0;
        do {
            reversed[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count > 0) *target++ = reversed[--count];
    }

public:
    ReplyWriter() {
#ifdef _WIN32
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Invalid output handle");
#endif
    }
    void placement(int digs, int position) const {
        char buffer[32];
        char* end = buffer;
        appendNumber(end, static_cast<unsigned>(digs));
        *end++ = ' ';
        appendNumber(end, static_cast<unsigned>(position));
        *end++ = '\n';
        writeAll(buffer, static_cast<int>(end - buffer));
    }
    void removal(int position) const {
        char buffer[32];
        char* end = buffer;
        appendNumber(end, static_cast<unsigned>(position));
        *end++ = '\n';
        writeAll(buffer, static_cast<int>(end - buffer));
    }
};

} // namespace pipeline

#ifndef PIPELINE_NO_MAIN
int main() {
    try {
        pipeline::Pipeline engine;
        pipeline::CommandReader input;
        pipeline::ReplyWriter output;
        while (const auto command = input.nextCommand()) {
            if (*command == 0) return 0;
            if (*command > 0) {
                const auto answer = engine.addPipe(*command);
                output.placement(answer.first, answer.second);
            } else {
                output.removal(engine.removePipe(-*command));
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Pipeline error: %s\n", error.what());
        return 1;
    }
}
#endif
