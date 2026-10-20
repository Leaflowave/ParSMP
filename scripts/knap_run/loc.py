import os
import subprocess
import sys
import itertools
import json
from concurrent.futures import ProcessPoolExecutor

if len(sys.argv) <= 1:
    raise ValueError("Please provide the path to the executable as a command line argument.")
if len(sys.argv) <= 2:
    raise ValueError("Please provide the path to the result directory as a command line argument.")

EXE_PATH = sys.argv[1]
RESULT_DIR = sys.argv[2]
PAR_THREADS = sys.argv[3] if len(sys.argv) >= 4 else "1"
INPUT_FILES = ["data/FacilityLocation/L.20.5.1.csv",
             "data/FacilityLocation/L.20.8.1.csv",
             "data/FacilityLocation/L.30.5.1.csv",
             "data/FacilityLocation/L.30.8.1.csv",
             "data/FacilityLocation/L.40.5.1.csv",
             "data/FacilityLocation/L.40.8.1.csv",
             "data/FacilityLocation/L.50.5.1.csv",
             "data/FacilityLocation/L.50.8.1.csv",
             "data/FacilityLocation/L.60.5.1.csv",
             "data/FacilityLocation/L.60.8.1.csv"]
if not os.path.exists(RESULT_DIR):
    os.makedirs(RESULT_DIR)

W = [x for x in range(1, 21)]

ALGORITHMS = ["ParSMP"]

TIME_LIMIT = 1800
MAX_WORKERS = 32 // int(PAR_THREADS)

def run_experiment(w, input_file, algorithm):
    """运行单个实验"""
    output_file = os.path.join(
        RESULT_DIR,
        f"{os.path.basename(input_file)}_{w}_{algorithm}_{PAR_THREADS}.json",
    )
    print(f"Running experiment: {output_file}")
    if os.path.exists(output_file):
        return 0  # 如果输出文件已经存在，则跳过该实验
    # 查看之前的实验是否被提前终止
    for pre_w in range(1, w):
        pre_output_file = os.path.join(
            RESULT_DIR,
            f"{os.path.basename(input_file)}_{pre_w}_{algorithm}_{PAR_THREADS}.json",
        )
        if os.path.exists(pre_output_file):
            with open(pre_output_file) as file:
                js = json.load(file)
                if js["early_stopped"] == True:
                    return 0
    cmd = [
        EXE_PATH,
        "-i",
        input_file,
        "-o",
        output_file,
        "-w",
        str(w),
        "-t",
        str(TIME_LIMIT),
    ]
    print("Running command:", " ".join(cmd))
    env = os.environ.copy()
    env["PARLAY_NUM_THREADS"] = PAR_THREADS
    # 使用 subprocess.run 执行命令并捕获退出代码
    try:
        result = subprocess.run(cmd, check=True, text=True, capture_output=True, env=env)
        print(f"Experiment completed: {output_file}")
        print(result.stdout)  # 打印标准输出（如果需要）
    except subprocess.CalledProcessError as e:
        print(f"Error running command: {' '.join(cmd)}", file=sys.stderr)
        print(e.stderr)  # 打印错误输出
        return e.returncode
    return 0


experiments = list(itertools.product(W, INPUT_FILES, ALGORITHMS))
print(f"Total experiments: {len(experiments)}")

with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
    futures = [
        executor.submit(run_experiment, *args) for args in experiments
    ]

    for future in futures:
        try:
            exit_code = future.result()  # 获取任务结果
            if exit_code != 0:
                print(f"Experiment failed with exit code: {exit_code}", file=sys.stderr)
                sys.exit(exit_code)
        except Exception as e:
            print(f"Experiment raised an exception: {e}", file=sys.stderr)
            sys.exit(1)
