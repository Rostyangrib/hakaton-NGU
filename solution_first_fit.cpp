// Standalone First Fit solution. Submit with GCC C++20.
#include <algorithm>
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

    int nextByte() {
        if (offset == size) {
#ifdef _WIN32
            DWORD received = 0;
            if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer.data(),
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

class Pipeline {
    struct Pipe {
        int position;
        int length;
    };

    // The same free intervals in two indexes.
    std::map<int, int> byPosition;          // start -> length
    // Lengths are at most 40. Bucket 40 also contains all longer gaps.
    // The smallest start across eligible buckets is exactly First Fit.
    std::array<std::set<int>, 41> byLength;
    std::vector<Pipe> pipes{{-1, 0}};       // PID starts at 1
    int trenchLength = 0;

    void insertGap(int start, int length) {
        if (length == 0) return;
        byPosition.emplace(start, length);
        byLength[std::min(length, 40)].insert(start);
    }

    void eraseGap(int start, int length) {
        byPosition.erase(start);
        byLength[std::min(length, 40)].erase(start);
    }

public:
    // Return (number of 20-meter sections to dig, placement position).
    std::pair<int, int> addPipe(int length) {
        if (length < 1 || length > 40 || pipes.size() > 200000) {
            throw std::invalid_argument("Invalid pipe length or too many pipes");
        }
        int digs = 0;
        int position;
        int firstStart = -1;
        for (int bucket = length; bucket <= 40; ++bucket) {
            if (!byLength[bucket].empty()) {
                const int start = *byLength[bucket].begin();
                if (firstStart < 0 || start < firstStart) firstStart = start;
            }
        }
        if (firstStart >= 0) {
            const int start = firstStart;
            const int gapLength = byPosition.at(start);
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

int main() {
#ifdef _WIN32
    // Avoid the CRT's per-line LF-to-CRLF translation on the protocol pipe.
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    try {
        Pipeline pipeline;
        CommandReader input;
        int command;
        // The interactor sends commands directly, without a test-file header.
        while (input.read(command) && command != 0) {
            if (command > 0) {
                const auto [digs, position] = pipeline.addPipe(command);
                std::cout << digs << ' ' << position << '\n' << std::flush;
            } else {
                std::cout << pipeline.removePipe(-command) << '\n' << std::flush;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Pipeline error: " << error.what() << '\n';
        return 1;
    }
}
