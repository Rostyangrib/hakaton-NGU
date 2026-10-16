#define PIPELINE_NO_MAIN
#include "solution.cpp"
#include <cmath>
#include <functional>
#include <string>

using namespace pipeline;

#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error("Check failed at line " + std::to_string(__LINE__) + ": " #__VA_ARGS__); } while (false)

static bool closeEnough(double a, double b) { return std::abs(a - b) < 1e-10; }

// Independent occupancy model: one cell per meter, no interval indexes.
class MeterOracle {
    std::vector<int> cells;
    std::vector<int> positions{-1}, lengths{0};
public:
    void add(int length, int digs, int position) {
        CHECK(digs >= 0 && position >= 0);
        cells.resize(cells.size() + digs * 20, 0);
        CHECK(position + length <= static_cast<int>(cells.size()));
        const int pid = static_cast<int>(positions.size());
        for (int i = position; i < position + length; ++i) {
            CHECK(cells[i] == 0);
            cells[i] = pid;
        }
        positions.push_back(position);
        lengths.push_back(length);
    }
    void remove(int pid, int position) {
        CHECK(positions.at(pid) == position && lengths.at(pid) > 0);
        for (int i = position; i < position + lengths[pid]; ++i) {
            CHECK(cells[i] == pid);
            cells[i] = 0;
        }
        lengths[pid] = 0;
    }
    void check(const Pipeline& engine) const {
        CHECK(engine.checkIndexes());
        CHECK(static_cast<int>(cells.size()) == engine.trench);
        std::vector<Gap> expected;
        std::int64_t occupied = 0;
        for (int i = 0; i < static_cast<int>(cells.size());) {
            if (cells[i] != 0) { ++occupied; ++i; }
            else {
                const int start = i;
                while (i < static_cast<int>(cells.size()) && cells[i] == 0) ++i;
                expected.push_back({start, i - start});
            }
        }
        CHECK(expected == engine.freeGaps());
        CHECK(occupied == engine.occupied);
    }
};

static std::optional<Gap> fullScan(Pipeline& p, int length) {
    p.history.prepare();
    std::optional<Gap> best;
    double bestScore = 0.0;
    for (const auto& gap : p.freeGaps()) {
        if (gap.length < length) continue;
        const double score = p.history.score(gap.length, length);
        if (!best || score < bestScore - SCORE_EPS ||
            (score <= bestScore + SCORE_EPS && (gap.length < best->length ||
             (gap.length == best->length && gap.start < best->start)))) {
            best = gap;
            bestScore = score;
        }
    }
    return best;
}

static void randomScenario(int seed, int commands) {
    Pipeline p;
    MeterOracle oracle;
    std::vector<int> live;
    std::int64_t state = seed + 1;
    auto next = [&](int bound) {
        state = state * 48271 % 2147483647;
        return static_cast<int>(state % bound);
    };
    for (int step = 0; step < commands; ++step) {
        if (live.empty() || next(100) < 58) {
            const int length = next(40) + 1;
            if (step % 17 == 0) {
                CHECK(p.chooseGap(length) == fullScan(p, length));
                CHECK(p.lastCandidateCount <= 41);
            }
            const auto answer = p.addPipe(length);
            oracle.add(length, answer.first, answer.second);
            live.push_back(p.pipeCount);
        } else {
            const int index = next(static_cast<int>(live.size()));
            const int pid = live[index];
            oracle.remove(pid, p.removePipe(pid));
            live[index] = live.back();
            live.pop_back();
        }
        oracle.check(p);
    }
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"originalExample", [] {
            Pipeline p;
            CHECK(p.addPipe(1) == std::make_pair(1, 0));
            CHECK(p.addPipe(4) == std::make_pair(0, 1));
            CHECK(p.addPipe(15) == std::make_pair(0, 5));
            CHECK(p.removePipe(2) == 1);
            CHECK(p.addPipe(3) == std::make_pair(0, 1));
            CHECK(p.removePipe(1) == 0);
            CHECK(p.trench == 20 && p.occupied == 18 && p.checkIndexes());
        }},
        {"tailExpansionAndFortyMeters", [] {
            Pipeline p;
            CHECK(p.addPipe(19) == std::make_pair(1, 0));
            CHECK(p.addPipe(2) == std::make_pair(1, 19));
            CHECK(p.addPipe(40) == std::make_pair(2, 21));
            CHECK(p.trench == 80 && p.checkIndexes());
            Pipeline fresh;
            CHECK(fresh.addPipe(40) == std::make_pair(2, 0));
            CHECK(fresh.freeGaps().empty());
        }},
        {"exactFitAndPidAfterRemoval", [] {
            Pipeline p;
            p.addPipe(10); p.addPipe(10); p.removePipe(1);
            CHECK(p.addPipe(10) == std::make_pair(0, 0));
            CHECK(p.pipeCount == 3 && p.pipeLength(1) == 0 && p.pipeLength(3) == 10);
            CHECK(p.freeGaps().empty() && p.checkIndexes());
        }},
        {"mergeBothNeighborsAndTail", [] {
            Pipeline p;
            p.addPipe(5); p.addPipe(5); p.addPipe(5);
            p.removePipe(1); p.removePipe(3); p.removePipe(2);
            CHECK(p.freeGaps() == std::vector<Gap>({{0, 20}}));
            CHECK(p.history.seen == 3 && p.history.counts[5] == 3);
            CHECK(p.occupied == 0 && p.trench == 20 && p.checkIndexes());
        }},
        {"warmupAndRampBoundaries", [] {
            ArrivalHistory h;
            for (int i = 0; i < 199; ++i) h.observe(2);
            h.prepare(); CHECK(h.influence == 0.0);
            h.observe(2); h.prepare(); CHECK(closeEnough(h.influence, .005));
            for (int i = 0; i < 199; ++i) h.observe(2);
            h.prepare(); CHECK(h.influence == 1.0);
        }},
        {"slidingWindowForgetsOldLengths", [] {
            ArrivalHistory h;
            for (int i = 0; i < WINDOW_SIZE; ++i) h.observe(1);
            for (int i = 0; i < WINDOW_SIZE; ++i) h.observe(40);
            CHECK(h.seen == 2 * WINDOW_SIZE && h.size == WINDOW_SIZE);
            CHECK(h.counts[1] == 0 && h.counts[40] == WINDOW_SIZE);
            h.prepare(); double mass = 0;
            for (int l = 1; l <= 40; ++l) { CHECK(h.probabilities[l] > 0); mass += h.probabilities[l]; }
            CHECK(closeEnough(mass, 1.0));
            CHECK(h.penalty(0) == 0 && h.penalty(40) == 0 && h.penalty(10000) == 0);
        }},
        {"scoringMatchesFormula", [] {
            ArrivalHistory h;
            for (int i = 0; i < 900; ++i) h.observe(1);
            for (int i = 0; i < 100; ++i) h.observe(40);
            h.prepare(); std::array<double, 41> raw{}; double mean = 0, cumulative = 0;
            for (int l = 1; l <= 40; ++l) { raw[l] = (h.counts[l] + 1.0 / 40) / 1001; mean += l * raw[l]; }
            for (int l = 1; l < 40; ++l) {
                const double q = .75 * raw[l] + .25 * l * raw[l] / mean;
                CHECK(closeEnough(q, h.probabilities[l])); cumulative += q;
                CHECK(closeEnough(h.penalty(l), l * (1 - cumulative)));
            }
            CHECK(closeEnough(h.score(6, 3), h.penalty(3) - h.penalty(6)));
        }},
        {"bestFitCounterexampleIsValid", [] {
            Pipeline p;
            p.addPipe(4); p.addPipe(10); p.addPipe(6);
            p.removePipe(1); p.removePipe(3);
            p.addPipe(3); p.addPipe(4); p.addPipe(3);
            CHECK(p.trench == 40 && p.occupied == 20 && p.checkIndexes());
        }},
        {"optimizedCandidatesCoverAllLengths", [] {
            Pipeline p; std::vector<int> removable;
            for (int hole = 1; hole <= 90; ++hole) {
                for (int remaining = hole; remaining > 0;) {
                    const int part = std::min(remaining, 40);
                    p.addPipe(part); removable.push_back(p.pipeCount); remaining -= part;
                }
                p.addPipe(1);
            }
            for (int pid : removable) p.removePipe(pid);
            for (int i = 0; i < 500; ++i) p.history.observe(3);
            CHECK(p.checkIndexes());
            for (int l = 1; l <= 40; ++l) {
                CHECK(p.chooseGap(l) == fullScan(p, l)); CHECK(p.lastCandidateCount <= 41);
            }
        }},
        {"positionsReuseSameGap", [] {
            Pipeline p;
            p.addPipe(10); p.addPipe(1); p.addPipe(10); p.addPipe(1);
            p.removePipe(1); p.removePipe(3);
            for (int i = 0; i < 1000; ++i) {
                CHECK(p.addPipe(10) == std::make_pair(0, 0));
                CHECK(p.removePipe(p.pipeCount) == 0 && p.checkIndexes());
            }
            p.addPipe(10); CHECK(p.addPipe(10) == std::make_pair(0, 11)); CHECK(p.checkIndexes());
        }},
        {"rejectsInvalidPipeAndPid", [] {
            Pipeline p; int rejected = 0;
            for (const int length : {0, 41}) {
                try { p.addPipe(length); } catch (const std::invalid_argument&) { ++rejected; }
            }
            try { p.removePipe(1); } catch (const std::invalid_argument&) { ++rejected; }
            p.addPipe(1); p.removePipe(1);
            try { p.removePipe(1); } catch (const std::invalid_argument&) { ++rejected; }
            CHECK(rejected == 4 && p.checkIndexes());
        }},
        {"hundredSmallRandomScenarios", [] { for (int seed = 0; seed < 100; ++seed) randomScenario(seed, 200); }},
        {"adaptiveRandomScenarios", [] { for (int seed = 100; seed < 106; ++seed) randomScenario(seed, 1200); }},
    };
    int passed = 0;
    for (const auto& test : tests) {
        try { test.second(); ++passed; std::printf("PASS %s\n", test.first); }
        catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s: %s\n", test.first, error.what()); return 1; }
    }
    std::printf("TOTAL %d; PASSED %d; FAILED 0\n", static_cast<int>(tests.size()), passed);
}
