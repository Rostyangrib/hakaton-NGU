// Standalone interactive solution. Compatible with GCC C++20 (and C++17).
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdint>
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
constexpr int WINDOW_SIZE = 1024;
constexpr int WARMUP = 200;
constexpr int RAMP = 200;
constexpr double PRIOR_MASS = 1.0;
constexpr double LENGTH_BIAS = 0.25;
constexpr double SCORE_EPS = 1e-9;

struct Gap {
    int start;
    int length;
    bool operator==(const Gap& other) const {
        return start == other.start && length == other.length;
    }
};

class ArrivalHistory {
    std::array<int, WINDOW_SIZE> ring{};
    int cursor = 0;

public:
    std::array<int, 41> counts{};
    std::array<double, 41> probabilities{};
    std::array<double, 41> penalties{};
    int seen = 0;
    int size = 0;
    double influence = 0.0;

    void observe(int length) {
        if (length < 1 || length > 40) {
            throw std::invalid_argument("Pipe length must be in 1..40");
        }
        if (size == WINDOW_SIZE) {
            --counts[ring[cursor]];
        } else {
            ++size;
        }
        ring[cursor] = length;
        cursor = (cursor + 1) % WINDOW_SIZE;
        ++counts[length];
        ++seen;
    }

    // The current arrival is not observed until after its position is chosen.
    void prepare() {
        const int t = seen + 1;
        influence = t <= WARMUP ? 0.0 :
                    (t >= WARMUP + RAMP ? 1.0 : double(t - WARMUP) / RAMP);
        if (influence == 0.0) return;

        const double denominator = size + PRIOR_MASS;
        double mean = 0.0;
        for (int length = 1; length <= 40; ++length) {
            const double p = (counts[length] + PRIOR_MASS / 40.0) / denominator;
            probabilities[length] = p;
            mean += length * p;
        }
        for (int length = 1; length <= 40; ++length) {
            const double p = probabilities[length];
            probabilities[length] = (1.0 - LENGTH_BIAS) * p +
                                    LENGTH_BIAS * length * p / mean;
        }
        double tail = 0.0;
        penalties[40] = 0.0;
        for (int g = 39; g >= 1; --g) {
            tail += probabilities[g + 1];
            penalties[g] = g * tail;
        }
        penalties[0] = 0.0;
    }

    double penalty(int length) const {
        return length <= 0 || length >= 40 ? 0.0 : penalties[length];
    }

    double score(int hole, int pipe) const {
        const int rest = hole - pipe;
        return (1.0 - influence) * rest + influence * (penalty(rest) - penalty(hole));
    }
};

class Pipeline {
    // Both indexes contain exactly the same free intervals.
    std::map<int, int> byStart;
    std::map<int, std::set<int>> byLength;
    std::vector<int> positions = std::vector<int>(MAX_PIPES + 1, -1);
    std::vector<int> lengths = std::vector<int>(MAX_PIPES + 1, 0);

    void insertGap(int start, int length) {
        if (length <= 0) return;
        byStart.emplace(start, length);
        byLength[length].insert(start);
    }

    void eraseGap(int start, int length) {
        byStart.erase(start);
        auto bucket = byLength.find(length);
        bucket->second.erase(start);
        if (bucket->second.empty()) byLength.erase(bucket);
    }

public:
    ArrivalHistory history;
    int trench = 0;
    int pipeCount = 0;
    std::int64_t occupied = 0;
    int lastCandidateCount = 0;

    std::optional<Gap> chooseGap(int length) {
        history.prepare();
        lastCandidateCount = 0;
        std::optional<Gap> best;
        double bestScore = 0.0;
        for (auto bucket = byLength.lower_bound(length); bucket != byLength.end(); ++bucket) {
            const int hole = bucket->first;
            const int start = *bucket->second.begin();
            const double score = history.score(hole, length);
            ++lastCandidateCount;
            if (!best || score < bestScore - SCORE_EPS ||
                (score <= bestScore + SCORE_EPS &&
                 (hole < best->length || (hole == best->length && start < best->start)))) {
                best = Gap{start, hole};
                bestScore = score;
            }
            // Larger holes all have Phi(hole)=Phi(hole-length)=0.
            // Only the smallest such length can win the tie or the BF term.
            if (history.influence == 0.0 || hole >= length + 40) break;
        }
        return best;
    }

    std::pair<int, int> addPipe(int length) {
        if (length < 1 || length > 40 || pipeCount >= MAX_PIPES) {
            throw std::invalid_argument("Invalid pipe length or too many pipes");
        }
        const auto gap = chooseGap(length);
        int digs = 0;
        int position = 0;
        if (gap) {
            position = gap->start;
            eraseGap(gap->start, gap->length);
            insertGap(position + length, gap->length - length);
        } else {
            int tailLength = 0;
            if (!byStart.empty()) {
                const auto tail = std::prev(byStart.end());
                if (tail->first + tail->second == trench) tailLength = tail->second;
            }
            digs = (length - tailLength + 19) / 20;
            const int newTrench = trench + digs * 20;
            if (newTrench > MAX_TRENCH) {
                throw std::runtime_error("Trench length exceeds protocol limit");
            }
            position = trench - tailLength;
            if (tailLength > 0) eraseGap(position, tailLength);
            trench = newTrench;
            insertGap(position + length, trench - position - length);
        }
        ++pipeCount;
        positions[pipeCount] = position;
        lengths[pipeCount] = length;
        occupied += length;
        history.observe(length);
        return {digs, position};
    }

    int removePipe(int pid) {
        if (pid < 1 || pid > pipeCount || lengths[pid] == 0) {
            throw std::invalid_argument("Unknown or already removed PID");
        }
        const int position = positions[pid];
        int start = position;
        int length = lengths[pid];
        auto right = byStart.lower_bound(start);
        if (right != byStart.begin()) {
            const auto left = std::prev(right);
            if (left->first + left->second == start) {
                start = left->first;
                length += left->second;
                eraseGap(left->first, left->second);
            }
        }
        // Erasing the left neighbor does not invalidate the right iterator.
        if (right != byStart.end() && right->first == start + length) {
            length += right->second;
            eraseGap(right->first, right->second);
        }
        insertGap(start, length);
        occupied -= lengths[pid];
        lengths[pid] = 0;
        return position;
    }

    // Inspection methods are used by the separate unit tests.
    int pipePosition(int pid) const { return positions.at(pid); }
    int pipeLength(int pid) const { return lengths.at(pid); }
    std::vector<Gap> freeGaps() const {
        std::vector<Gap> result;
        for (const auto& gap : byStart) result.push_back({gap.first, gap.second});
        return result;
    }
    bool checkIndexes() const {
        int previousEnd = -1;
        std::int64_t freeLength = 0;
        std::size_t indexedCount = 0;
        for (const auto& gap : byStart) {
            const int start = gap.first, length = gap.second;
            if (length <= 0 || start < 0 || start <= previousEnd || start + length > trench) return false;
            const auto bucket = byLength.find(length);
            if (bucket == byLength.end() || bucket->second.count(start) != 1) return false;
            previousEnd = start + length;
            freeLength += length;
        }
        for (const auto& bucket : byLength) {
            if (bucket.second.empty()) return false;
            indexedCount += bucket.second.size();
        }
        return indexedCount == byStart.size() && freeLength + occupied == trench;
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
