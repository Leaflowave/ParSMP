# ParSMP

ParSMP is a parallel branch-and-bound implementation for exact monotone submodular maximization under cardinality and knapsack constraints.

The implementation supports weighted coverage (COV), bipartite influence (INF), facility location (LOC), partial dominating set (DOM), and group closeness centrality (GCC).

## Requirements

- Linux on x86-64; the paper's experiments used Ubuntu 22.04.
- A C++23 compiler and standard library supporting features such as `std::print` (the experiments used g++ 14.2.0).
- [Xmake](https://xmake.io/) to build the executables.
- Python 3 for the benchmark runners.
- Git LFS to download the datasets.

ParlayLib, argparse, nlohmann/json, and magic_enum headers are included in `include/`. Gurobi is not required for the ParSMP targets. Windows users can run the Linux commands below in WSL with the required tools installed there.

## Download

```bash
git lfs install
git clone https://github.com/Leaflowave/ParSMP.git
cd ParSMP
git lfs pull
```

Run the following commands from the directory containing `xmake.lua`.

## Build

Configure a release build and compile all cardinality and knapsack targets:

```bash
xmake f -m release --toolchain=gcc
xmake
```

To build a single target instead:

```bash
xmake build card_loc
xmake build knap_loc
```

| Problem | Cardinality executable | Knapsack executable | Dataset directory |
| --- | --- | --- | --- |
| Weighted coverage | `build/card/card_cov` | `build/knap/knap_cov` | `dataset/WeightedCoverage/` |
| Bipartite influence | `build/card/card_inf` | `build/knap/knap_inf` | `dataset/BipartiteInfluence/` |
| Facility location | `build/card/card_loc` | `build/knap/knap_loc` | `dataset/FacilityLocation/` |
| Partial dominating set | `build/card/card_dom` | `build/knap/knap_dom` | `dataset/Graph/` |
| Group closeness centrality | `build/card/card_gcc` | `build/knap/knap_gcc` | `dataset/Graph/` |

## Run one instance

Create the output directory first. Set `PARLAY_NUM_THREADS` to the number of worker threads before launching each process.

### Cardinality constraint

Solve a facility-location instance with at most 5 selected facilities, one worker thread, and a 1800-second search time limit:

```bash
mkdir -p results
PARLAY_NUM_THREADS=1 ./build/card/card_loc \
  -i dataset/FacilityLocation/L.20.5.1.csv \
  -k 5 -t 1800 -o results/card_loc_1.json
```

Run the same instance using 32 threads:

```bash
PARLAY_NUM_THREADS=32 ./build/card/card_loc \
  -i dataset/FacilityLocation/L.20.5.1.csv \
  -k 5 -t 1800 -o results/card_loc_32.json
```

### Knapsack constraint

Use `-w` for the capacity instead of `-k`:

```bash
PARLAY_NUM_THREADS=32 ./build/knap/knap_loc \
  -i dataset/FacilityLocation/L.20.5.1.csv \
  -w 5 -t 1800 -o results/knap_loc_32.json
```

The current knapsack executables generate candidate selection weights internally using `std::mt19937` with seed 0. Weights are sampled with mean 1 and standard deviation 0.2, then clamped to `[0.1, 1.9]`. These selection weights are separate from the item values, activation probabilities, or facility benefits stored in the input files. There is currently no command-line option for supplying a custom weight file.

## Command-line options

| Option | Meaning | Default |
| --- | --- | --- |
| `-i`, `--input` | Input dataset path | Required |
| `-o`, `--output` | Output JSON path | `result.json` |
| `-k`, `--cardinality` | Cardinality bound; cardinality targets only | Required |
| `-w`, `--total_weight` | Knapsack capacity; knapsack targets only | Required |
| `-t`, `--max_time` | Search time limit in seconds | Unlimited |
| `-r`, `--reduction_strategy` | `TopK` or `None` | `TopK` |
| `-ub`, `--upper_bound_strategy` | `TopK` or `None` | `TopK` |
| `-u`, `--update_strategy` | `Threshold` or `All` | `Threshold` |

The program prints the best solution value and selected element identifiers and writes a JSON result containing fields such as `best_value`, `best_solution`, `total_time`, `nodes_visited`, and `early_stopped`. If `early_stopped` is true, the search stopped at the time limit and the returned solution is not certified optimal.

## Run experiments with `experiment.sh`

The supplied `experiment.sh` runs **knapsack-constrained experiments only**. Run it in Bash on Linux or WSL, from the directory containing `experiment.sh` and `xmake.lua`. It does not compile the programs, so build them first.

The Python runners use paths beginning with `data/`, while this repository supplies the datasets in `dataset/`. Before running the script, create a symbolic link from the repository root if `data/` does not already exist:

```bash
ln -s dataset data
```

Alternatively, edit `INPUT_FILES` in the relevant Python scripts to use `dataset/` paths.

### Quick start

```bash
xmake f -m release --toolchain=gcc
xmake
# Run once, only if data/ does not already exist:
ln -s dataset data
# Run LOC with 1 and 32 threads:
bash experiment.sh loc 1,32
```

### Arguments and defaults

```bash
bash experiment.sh [problem_list] [thread_list]
```

Both arguments are optional comma-separated lists, without spaces.

| Argument | Accepted values | Default |
| --- | --- | --- |
| First: problem classes | `cov`, `dom`, `inf`, `loc`, `gcc` | `cov,dom,inf,loc,gcc` |
| Second: threads per solver process | `1`, `2`, `4`, `8`, `16`, `32` | `1,2,4,8,16,32` |

Examples:

```bash
# All five classes at all six thread counts:
bash experiment.sh
# LOC at all six thread counts:
bash experiment.sh loc
# LOC at 1 and 32 threads:
bash experiment.sh loc 1,32
# COV, INF, and LOC at 32 threads:
bash experiment.sh cov,inf,loc 32
```

The script loops over problem classes and then thread counts, executing this command for each pair:

```bash
python3 scripts/knap_run/${func}.py \
  build/knap/knap_${func} results/knap/${func} ${thread}
```

For example, `bash experiment.sh loc 32` invokes:

```bash
python3 scripts/knap_run/loc.py build/knap/knap_loc results/knap/loc 32
```

The Python runner creates `results/knap/loc/`, sets `PARLAY_NUM_THREADS=32` for the solver process, and enumerates 10 datasets with capacities `W=1,...,20`. Each run has a 1800-second search time limit. JSON filenames include the input filename, capacity, algorithm name, and thread count, for example:

```text
results/knap/loc/L.20.5.1.csv_5_ParSMP_32.json
```

Existing result files are skipped. Higher-capacity runs may also be skipped when an earlier timeout result is already available. Thus, not every enumerated run necessarily produces a new result file. For a fresh experiment, move the previous result directory aside or change the output directory passed to the Python runner. The shell script exits if a runner returns a nonzero status.

The runners use `32 // thread_count` concurrent processes: 32 solver processes with one thread each for `thread_count=1`, and one solver process with 32 threads for `thread_count=32`. Adjust `MAX_WORKERS` in the Python runners for a different machine. For isolated runtime and speedup measurements, use the single-instance commands above or set `MAX_WORKERS = 1`.

**Status-message note:** the current shell script prints a command containing `scripts/run/`, but the next line actually executes `scripts/knap_run/`. The latter is the correct path. This discrepancy affects only the printed message; it can be corrected by replacing `scripts/run/` with `scripts/knap_run/` in the `echo` line.

### Cardinality-constrained batches

`experiment.sh` does not run cardinality-constrained experiments. Invoke the corresponding Python runners directly:

```bash
python3 scripts/card_run/loc.py build/card/card_loc results/card/loc 1
python3 scripts/card_run/loc.py build/card/card_loc results/card/loc 32
```

These runners use `-k` rather than `-w`, enumerate `k=1,...,20`, and use the same dataset-path setup, time limit, and process-count policy. Replace `loc` with `cov`, `inf`, `dom`, or `gcc` for another problem class.

## Troubleshooting

- **Missing C++23 headers or `std::print`:** use a compiler and matching standard library with the required C++23 support. Check the compiler selected by Xmake.
- **Input file not found:** run from the repository root, check the dataset path, and run `git lfs pull`. Batch scripts additionally require the `data/` path described above.
- **No output JSON:** create the output file's parent directory before running an executable directly.
- **Only one worker:** set `PARLAY_NUM_THREADS` before starting the process and do not compile with `PARLAY_SEQUENTIAL`.
- **Gurobi errors:** the optional ILP targets are disabled in `xmake.lua`. Enabling them requires a separate Gurobi installation and updating the installation-specific include and library paths.

Dataset provenance is documented in `dataset/README.md`. Some acquisition instructions in that inherited file refer to utility scripts that are not included here; the examples above use the supplied dataset files directly.
