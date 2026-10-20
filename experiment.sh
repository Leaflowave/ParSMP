#!/bin/bash

# 默认值
FUNCTION_NAMES=("cov" "dom" "inf" "loc" "gcc")
THREADS=(1 2 4 8 16 32)

# 可选：从命令行参数读取
if [ $# -ge 1 ]; then
    # 如果提供了第一个参数，用逗号分隔的函数名列表
    IFS=',' read -ra FUNCTION_NAMES <<< "$1"
fi

if [ $# -ge 2 ]; then
    # 如果提供了第二个参数，用逗号分隔的线程数列表
    IFS=',' read -ra THREADS <<< "$2"
fi

for func in "${FUNCTION_NAMES[@]}"; do
    for thread in "${THREADS[@]}"; do
        echo "Running: python3 scripts/run/${func}.py build/knap/knap_${func} results/knap/${func} ${thread}"
        python3 scripts/knap_run/${func}.py build/knap/knap_${func} results/knap/${func} ${thread}
        
        # 检查执行是否成功
        if [ $? -ne 0 ]; then
            echo "Error: Failed to run ${func} with ${thread} threads"
            exit 1
        fi
    done
done

echo "All tasks completed successfully!"