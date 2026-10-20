#pragma once
#include <utility>

#include "argparse/argparse.hpp"
#include "submodlib/constraint.hpp"
#include "submodlib/knapsack_solver.hpp"
#include "submodlib/solver.hpp"

inline argparse::ArgumentParser global_parser("Submodular Optimization Solver");

auto get_solver_config(int argc, char** argv) {
    using submodlib::ReductionStrategy;
    using submodlib::UpdateStrategy;
    using submodlib::UpperBoundStrategy;
    global_parser.add_argument("-i", "--input").help("Input file path").required();
    global_parser.add_argument("-o", "--output")
        .help("Output file path (.json)")
        .default_value(std::string("result.json"));
    global_parser.add_argument("-k", "--cardinality").help("Cardinality constraint k").required().scan<'u', size_t>();
    global_parser.add_argument("-r", "--reduction_strategy")
        .help("Reduction strategy to use (None, TopK)")
        .default_value(ReductionStrategy::TopK)
        .action([](const std::string& value) {
            if (value == "None") return ReductionStrategy::None;
            if (value == "TopK") return ReductionStrategy::TopK;
            throw std::invalid_argument("Invalid reduction strategy: " + value);
        });
    global_parser.add_argument("-ub", "--upper_bound_strategy")
        .help("Upper bound strategy to use (None, TopK)")
        .default_value(UpperBoundStrategy::TopK)
        .action([](const std::string& value) {
            if (value == "None") return UpperBoundStrategy::None;
            if (value == "TopK") return UpperBoundStrategy::TopK;
            throw std::invalid_argument("Invalid upper bound strategy: " + value);
        });
    global_parser.add_argument("-u", "--update_strategy")
        .help("Update strategy to use (Threshold or All)")
        .default_value(UpdateStrategy::Threshold)
        .action([](const std::string& value) {
            if (value == "Threshold") return UpdateStrategy::Threshold;
            if (value == "All") return UpdateStrategy::All;
            throw std::invalid_argument("Invalid update strategy: " + value);
        });
    global_parser.add_argument("-t", "--max_time")
        .help("Maximum running time in seconds")
        .default_value(std::numeric_limits<double>::infinity())
        .scan<'g', double>();
    try {
        global_parser.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::cerr << global_parser;
        std::exit(1);
    }
    auto config = submodlib::SolverConfig()
                      .set_reduction_strategy(global_parser.get<ReductionStrategy>("--reduction_strategy"))
                      .set_upper_bound_strategy(global_parser.get<UpperBoundStrategy>("--upper_bound_strategy"))
                      .set_update_strategy(global_parser.get<UpdateStrategy>("--update_strategy"))
                      .set_max_time(global_parser.get<double>("--max_time"));
    return std::make_tuple(global_parser.get<size_t>("--cardinality"), config);
}

auto get_knapsack_solver_config(int argc, char** argv) {
    using submodlib::ReductionStrategy;
    using submodlib::UpdateStrategy;
    using submodlib::UpperBoundStrategy;
    global_parser.add_argument("-i", "--input").help("Input file path").required();
    global_parser.add_argument("-o", "--output")
        .help("Output file path (.json)")
        .default_value(std::string("result.json"));
    global_parser.add_argument("-w", "--total_weight")
        .help("Total weight (capacity) for the knapsack constraint")
        .required()
        .scan<'g', double>();
    global_parser.add_argument("-r", "--reduction_strategy")
        .help("Reduction strategy to use (None or TopK)")
        .default_value(ReductionStrategy::TopK)
        .action([](const std::string& value) {
            if (value == "None") return ReductionStrategy::None;
            if (value == "TopK") return ReductionStrategy::TopK;
            throw std::invalid_argument("Invalid reduction strategy: " + value);
        });
    global_parser.add_argument("-ub", "--upper_bound_strategy")
        .help("Upper bound strategy to use (None or TopK)")
        .default_value(UpperBoundStrategy::TopK)
        .action([](const std::string& value) {
            if (value == "None") return UpperBoundStrategy::None;
            if (value == "TopK") return UpperBoundStrategy::TopK;
            throw std::invalid_argument("Invalid upper bound strategy: " + value);
        });
    global_parser.add_argument("-u", "--update_strategy")
        .help("Update strategy to use (Threshold or All)")
        .default_value(UpdateStrategy::Threshold)
        .action([](const std::string& value) {
            if (value == "Threshold") return UpdateStrategy::Threshold;
            if (value == "All") return UpdateStrategy::All;
            throw std::invalid_argument("Invalid update strategy: " + value);
        });
    global_parser.add_argument("-t", "--max_time")
        .help("Maximum running time in seconds")
        .default_value(std::numeric_limits<double>::infinity())
        .scan<'g', double>();
    try {
        global_parser.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::cerr << global_parser;
        std::exit(1);
    }
    auto config = submodlib::KnapsackSolverConfig()
                      .set_reduction_strategy(global_parser.get<ReductionStrategy>("--reduction_strategy"))
                      .set_upper_bound_strategy(global_parser.get<UpperBoundStrategy>("--upper_bound_strategy"))
                      .set_update_strategy(global_parser.get<UpdateStrategy>("--update_strategy"))
                      .set_max_time(global_parser.get<double>("--max_time"));
    return std::make_pair(global_parser.get<double>("--total_weight"), config);
}
