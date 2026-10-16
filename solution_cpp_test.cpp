#define PIPELINE_NO_MAIN
#include "solution.cpp"
#include <random>
#include <string>

#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error("Check failed at line " + std::to_string(__LINE__) + ": " #__VA_ARGS__); } while (false)

// Independent meter-by-meter occupancy checker, with no interval indexes.
class MeterOracle {
    std::vector<int> cells, positions{-1}, lengths{0};
    int occupied = 0;
public:
    void add(int length, std::pair<int, int> reply) {
        const auto [digs, start] = reply;
        CHECK(digs >= 0 && start >= 0);
        cells.resize(cells.size() + digs * 20, 0);
        CHECK(start + length <= static_cast<int>(cells.size()));
        for (int i = start; i < start + length; ++i) {
            CHECK(cells[i] == 0); cells[i] = static_cast<int>(positions.size());
        }
        positions.push_back(start); lengths.push_back(length); occupied += length;
    }
    void remove(int pid, int start) {
        CHECK(positions.at(pid) == start && lengths.at(pid) > 0);
        for (int i = start; i < start + lengths[pid]; ++i) {
            CHECK(cells[i] == pid); cells[i] = 0;
        }
        occupied -= lengths[pid]; lengths[pid] = 0;
    }
    // A separate exhaustive Best Fit implementation for disabled heuristics.
    std::pair<int, int> bestFit(int length) const {
        int bestLength = pipeline::MAX_TRENCH, bestStart = -1;
        for (int i = 0; i < static_cast<int>(cells.size());) {
            if (cells[i]) { ++i; continue; }
            const int start = i;
            while (i < static_cast<int>(cells.size()) && !cells[i]) ++i;
            if (i - start >= length && i - start < bestLength) {
                bestLength = i - start; bestStart = start;
            }
        }
        if (bestStart >= 0) return {0, bestStart};
        int start = static_cast<int>(cells.size());
        while (start && !cells[start - 1]) --start;
        return {(length - (static_cast<int>(cells.size()) - start) + 19) / 20, start};
    }
    void verify(const pipeline::Pipeline& engine) const {
        CHECK(engine.trench() == static_cast<int>(cells.size()));
        CHECK(engine.occupiedMeters() == occupied);
        CHECK(engine.checkInvariants());
    }
};

static int run(const std::vector<int>& commands, pipeline::Options options = {}) {
    pipeline::Pipeline engine(options);
    MeterOracle oracle;
    for (int command : commands) {
        if (command > 0) {
            const auto reply = engine.addPipe(command);
            if (!options.cohorts && !options.coalescence) CHECK(reply == oracle.bestFit(command));
            oracle.add(command, reply);
        } else oracle.remove(-command, engine.removePipe(-command));
        oracle.verify(engine);
    }
    return engine.trench();
}

static void mustFail(auto action) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    CHECK(failed);
}

int main() {
    try {
        // Exact filling, both-neighbor merges, tail reuse, repeated removals.
        CHECK(run({1,4,15,-2,3,-1}) == 20);
        CHECK(run({19,2,40}) == 80);
        CHECK(run({5,5,5,-1,-3,-2,20}) == 20);
        CHECK(run({10,10,-1,10,-2,10}) == 20);
        const std::vector<int> trap{4,4,3,4,2,3,-1,-3,-5,2,-6,5};
        CHECK(run(trap, {false,false}) == 40);
        CHECK(run(trap) == 20);
        // Exact Best Fit is still needed here: First Fit would dig twice.
        CHECK(run({3,1,2,1,1,12,-1,-3,-5,1,3}) == 20);
        for (unsigned seed = 0; seed < 40; ++seed) {
            std::mt19937 random(seed);
            std::vector<int> commands, live;
            int pid = 0;
            for (int i = 0; i < 1500; ++i) {
                if (live.empty() || random() % 100 < 58) {
                    commands.push_back(1 + random() % 40); live.push_back(++pid);
                } else {
                    const int index = random() % live.size();
                    commands.push_back(-live[index]); live[index] = live.back(); live.pop_back();
                }
            }
            run(commands); run(commands, {false,false});
        }
        // Unseen batch lengths, either parity removed, then an abrupt regime shift.
        for (int count : {33,65,130,263}) for (int parity : {0,1}) {
            std::vector<int> commands;
            int pid = 0;
            for (int length : {2,5,9,17,31,40}) {
                const int first = pid + 1;
                for (int i = 0; i < count; ++i) { commands.push_back(length); ++pid; }
                for (int i = parity; i < count; i += 2) commands.push_back(-(first + i));
            }
            commands.push_back(3); ++pid; // Stop the predicted next batch early.
            for (int i = 0; i < 99; ++i) { commands.push_back(1 + i % 40); ++pid; }
            run(commands);
        }
        // Full command/PID limit, maximum length, zero speculative growth.
        pipeline::Pipeline large;
        for (int i = 0; i < 200000; ++i) CHECK(large.addPipe(40) == std::make_pair(2,40*i));
        CHECK(large.trench() == 8000000 && large.checkInvariants());
        mustFail([&] { large.addPipe(1); });
        pipeline::Pipeline invalid;
        mustFail([&] { invalid.addPipe(0); });
        mustFail([&] { invalid.addPipe(41); });
        mustFail([&] { invalid.removePipe(1); });
        invalid.addPipe(4); invalid.removePipe(1);
        mustFail([&] { invalid.removePipe(1); });
        std::puts("PASS: meter oracle; 40 random streams x 1500 commands, improved and Best Fit; 8 batch/shift streams; 200000-command limit; invalid inputs");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
