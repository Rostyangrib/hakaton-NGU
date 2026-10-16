// Standalone bounded First Fit / Best Fit hybrid. Submit with GCC C++20.
#include <array>
#include <cerrno>
#include <iostream>
#include <iterator>
#include <map>
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
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

// Read only currently available bytes. Waiting for a full input buffer would
// deadlock the interactor, which sends the next command only after a reply.
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
            if (!ReadFile(handle, buffer.data(),
                          static_cast<DWORD>(buffer.size()), &received, nullptr)) {
                if (GetLastError() != ERROR_BROKEN_PIPE) {
                    throw std::runtime_error("Cannot read input");
                }
                received = 0;
            }
            size = static_cast<int>(received);
#else
            do {
                size = static_cast<int>(::read(0, buffer.data(), buffer.size()));
            } while (size < 0 && errno == EINTR);
            if (size < 0) throw std::runtime_error("Cannot read input");
#endif
            offset = 0;
            if (size == 0) return -1;
        }
        return buffer[offset++];
    }

public:
    bool read(int& command) {
        int ch = nextByte();
        while (ch >= 0 && ch <= 32) ch = nextByte();
        if (ch < 0) return false;
        int sign = 1;
        if (ch == '-' || ch == '+') {
            if (ch == '-') sign = -1;
            ch = nextByte();
        }
        if (ch < '0' || ch > '9') throw std::invalid_argument("Expected integer command");
        int value = 0;
        while (ch >= '0' && ch <= '9') {
            value = value * 10 + ch - '0';
            if (value > 200000) throw std::invalid_argument("Command exceeds limits");
            ch = nextByte();
        }
        if (ch > 32) throw std::invalid_argument("Expected whitespace after command");
        command = sign * value;
        return true;
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

class Pipeline {
    struct Pipe {
        int position;
        int length;
    };

    // The same free intervals in two indexes.
    std::map<int, int> byPosition;          // start -> length
    std::map<int, std::set<int>> byLength; // length -> sorted starts
    std::vector<Pipe> pipes{{-1, 0}};       // PID starts at 1
    int trenchLength = 0;
    int maxExtraGap;

    void insertGap(int start, int length) {
        if (length == 0) return;
        byPosition.emplace(start, length);
        byLength[length].insert(start);
    }

    void eraseGap(int start, int length) {
        byPosition.erase(start);
        const auto bucket = byLength.find(length);
        bucket->second.erase(start);
        if (bucket->second.empty()) byLength.erase(bucket);
    }

public:
    explicit Pipeline(int extraGap = 1) : maxExtraGap(extraGap) {
        if (extraGap < 0) throw std::invalid_argument("Negative gap tolerance");
    }

    // Return (number of 20-meter sections to dig, placement position).
    std::pair<int, int> addPipe(int length, bool allowFirstFit = true) {
        if (length < 1 || length > 40 || pipes.size() > 200000) {
            throw std::invalid_argument("Invalid pipe length or too many pipes");
        }
        int digs = 0;
        int position;
        const auto best = byLength.lower_bound(length);
        if (best != byLength.end()) {
            // Prefer the leftmost gap, but only within a small additive
            // tolerance of Best Fit. With tolerance zero this is Best Fit.
            const int limit = best->first + (allowFirstFit ? maxExtraGap : 0);
            int gapLength = best->first;
            int start = *best->second.begin();
            for (auto candidate = std::next(best);
                 candidate != byLength.end() && candidate->first <= limit; ++candidate) {
                if (*candidate->second.begin() < start) {
                    gapLength = candidate->first;
                    start = *candidate->second.begin();
                }
            }
            position = start;
            eraseGap(start, gapLength);
            insertGap(start + length, gapLength - length);
        } else {
            // Reuse the free tail before digging additional sections.
            position = trenchLength;
            if (!byPosition.empty()) {
                const auto tail = std::prev(byPosition.end());
                if (tail->first + tail->second == trenchLength) {
                    position = tail->first;
                    eraseGap(tail->first, tail->second);
                }
            }
            const int tailLength = trenchLength - position;
            digs = (length - tailLength + 19) / 20;
            trenchLength += digs * 20;
            insertGap(position + length, trenchLength - position - length);
        }
        pipes.push_back({position, length});
        return {digs, position};
    }

    int removePipe(int pid) {
        if (pid <= 0 || pid >= static_cast<int>(pipes.size()) || pipes[pid].length == 0) {
            throw std::invalid_argument("Unknown or already removed PID");
        }
        const int position = pipes[pid].position;
        int start = position;
        int length = pipes[pid].length;
        auto right = byPosition.lower_bound(start);
        if (right != byPosition.begin()) {
            const auto left = std::prev(right);
            if (left->first + left->second == start) {
                start = left->first;
                length += left->second;
                eraseGap(left->first, left->second);
            }
        }
        // Removing the left neighbor preserves the right iterator.
        if (right != byPosition.end() && start + length == right->first) {
            length += right->second;
            eraseGap(right->first, right->second);
        }
        insertGap(start, length);
        pipes[pid].length = 0;
        return position;
    }
};

// Replay a virtual Best Fit using the same already received commands. Its
// placements are never emitted. If the actual trench has grown beyond this
// reference, stop spending space on First Fit preferences until it catches up.
class HybridPipeline {
    Pipeline actual;
    Pipeline reference{0};
    int actualTrench = 0;
    int referenceTrench = 0;
    bool useGuard;

public:
    explicit HybridPipeline(int extraGap = 1, bool guard = true)
        : actual(extraGap), useGuard(guard) {}

    std::pair<int, int> addPipe(int length) {
        referenceTrench += reference.addPipe(length).first * 20;
        const auto answer = actual.addPipe(length, !useGuard || actualTrench <= referenceTrench);
        actualTrench += answer.first * 20;
        return answer;
    }

    int removePipe(int pid) {
        reference.removePipe(pid);
        return actual.removePipe(pid);
    }
};

#ifndef PIPELINE_NO_MAIN
int main() {
#ifdef _WIN32
    // Avoid the CRT's per-line LF-to-CRLF translation on the protocol pipe.
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    try {
        HybridPipeline pipeline;
        CommandReader input;
        ReplyWriter output;
        int command;
        // The interactor sends commands directly, without a test-file header.
        while (input.read(command) && command != 0) {
            if (command > 0) {
                const auto [digs, position] = pipeline.addPipe(command);
                output.placement(digs, position);
            } else {
                output.removal(pipeline.removePipe(-command));
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Pipeline error: " << error.what() << '\n';
        return 1;
    }
}
#endif
