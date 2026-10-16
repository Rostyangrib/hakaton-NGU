// Offline experiment only. The submitted solution_hybrid.cpp reads no files.
#define PIPELINE_NO_MAIN
#include "../solution_hybrid.cpp"
#include <fstream>
#include <iomanip>
#include <string>

struct Scenario {
    double weight;
    std::vector<int> commands;
    long long finalOccupied = 0;
};

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::vector<Scenario> tests;
    for (int id = 1; id <= 15; ++id) {
        std::ifstream input(std::string(argv[1]) + "/" + std::to_string(id) + ".in");
        Scenario test;
        int count;
        if (!(input >> test.weight >> count)) return 3;
        std::vector<int> lengths{0};
        for (int i = 0, command; i < count; ++i) {
            if (!(input >> command) || command == 0) return 4;
            test.commands.push_back(command);
            if (command > 0) {
                lengths.push_back(command);
                test.finalOccupied += command;
            } else {
                test.finalOccupied -= lengths.at(-command);
            }
        }
        tests.push_back(std::move(test));
    }
    std::cout << "guard,extra_gap,test,trench_m,kpi,weighted_score\n" << std::setprecision(15);
    for (const bool guard : {false, true}) {
      for (const int extra : {0, 1, 2, 4, 8, 16, 40, 100000000}) {
        int id = 0;
        for (const auto& test : tests) {
            HybridPipeline pipeline(extra, guard);
            int trench = 0;
            for (const int command : test.commands) {
                if (command > 0) trench += 20 * pipeline.addPipe(command).first;
                else pipeline.removePipe(-command);
            }
            const double kpi = static_cast<double>(test.finalOccupied) / trench;
            std::cout << guard << ',' << extra << ',' << ++id << ',' << trench << ',' << kpi
                      << ',' << test.weight * kpi << '\n';
        }
      }
    }
}
