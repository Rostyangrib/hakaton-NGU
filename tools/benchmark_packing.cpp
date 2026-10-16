// Offline KPI measurement. The allocator sees exactly one command at a time.
#define PIPELINE_NO_MAIN
#include "../solution.cpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>

struct Scenario { std::string name; double weight; std::vector<int> commands; };

static Scenario load(int test) {
    Scenario s{std::to_string(test), 0, {}};
    std::ifstream input("case2_materials/tests/" + s.name + ".in");
    int count = 0;
    if (!(input >> s.weight >> count)) throw std::runtime_error("Cannot read test " + s.name);
    s.commands.resize(count);
    for (int& command : s.commands)
        if (!(input >> command) || command == 0) throw std::runtime_error("Invalid test");
    int zero = -1;
    if (!(input >> zero) || zero != 0) throw std::runtime_error("Missing terminator");
    return s;
}

static Scenario synthetic(int count, int parity, bool changeRegime) {
    Scenario s{"synthetic_" + std::to_string(count) + "_" + std::to_string(parity) +
               (changeRegime ? "_shift" : ""), 0, {}};
    int pid = 0;
    for (int length : {2, 3, 7, 11, 17, 23, 29, 37}) {
        const int first = pid + 1;
        for (int i = 0; i < count; ++i) { s.commands.push_back(length); ++pid; }
        for (int i = parity; i < count; i += 2) s.commands.push_back(-(first + i));
    }
    if (changeRegime) {
        // Deliberately stop repeating the learned series, then mix lengths.
        std::mt19937 random(401u + count + parity);
        std::vector<int> newLive;
        for (int i = 0; i < 2000; ++i) {
            if (!newLive.empty() && random() % 100 < 45) {
                const int index = random() % newLive.size();
                s.commands.push_back(-newLive[index]);
                newLive[index] = newLive.back(); newLive.pop_back();
            } else {
                s.commands.push_back(1 + random() % 40); newLive.push_back(++pid);
            }
        }
    }
    return s;
}

static Scenario stoppedForecast(int nextCount) {
    Scenario s{"forecast_stop_" + std::to_string(nextCount), 0, {}};
    for (int i = 0; i < 65; ++i) s.commands.push_back(1);
    for (int pid = 1; pid <= 65; pid += 2) s.commands.push_back(-pid);
    for (int i = 0; i < nextCount; ++i) s.commands.push_back(40);
    return s;
}

static void measure(const Scenario& s, pipeline::Options options, const char* strategy) {
    pipeline::Pipeline engine(options);
    std::vector<int> lengths{0};
    int occupied = 0, peak = 0;
    for (int command : s.commands) {
        if (command > 0) {
            engine.addPipe(command); lengths.push_back(command); occupied += command;
            peak = std::max(peak, occupied);
        } else {
            engine.removePipe(-command); occupied -= lengths.at(-command);
        }
    }
    if (!engine.checkInvariants() || engine.occupiedMeters() != occupied)
        throw std::runtime_error("Final state mismatch");
    const double kpi = static_cast<double>(occupied) / engine.trench();
    std::cout << s.name << ',' << strategy << ',' << s.commands.size() << ',' << s.weight << ','
              << occupied << ',' << peak << ',' << engine.trench() << ',' << kpi << ','
              << kpi * s.weight << '\n';
}

int main() {
    try {
        std::cout << std::setprecision(15)
                  << "test,strategy,commands,weight,remaining_pipe_m,peak_pipe_m,trench_m,kpi,weighted_score\n";
        std::vector<Scenario> scenarios;
        for (int t = 1; t <= 15; ++t) scenarios.push_back(load(t));
        for (int count : {47, 126, 257, 511})
            for (int parity : {0, 1}) {
                scenarios.push_back(synthetic(count, parity, false));
                scenarios.push_back(synthetic(count, parity, true));
            }
        scenarios.push_back(stoppedForecast(1));
        scenarios.push_back(stoppedForecast(2));
        for (const auto& s : scenarios) {
            measure(s, {false, false}, "best_fit");
            measure(s, {true, false}, "cohorts_only");
            measure(s, {false, true}, "coalescence_only");
            measure(s, {}, "improved");
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Benchmark error: %s\n", error.what());
        return 1;
    }
}
