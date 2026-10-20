import os
from sys import argv

import matplotlib.pyplot as plt
from typing import Any, Dict, List
from read_all_json import get_dicts

# 画图的样式
styles: list[dict[str, Any]] = [
    {
        "marker" : 'v',
        "color" : 'tab:red'
    },
    {
        "marker" : 'd',
        "color" : 'tab:blue'
    },
    {
        "marker" : 'o',
        "color" : 'tab:purple'
    },
    {
        "marker" : 's',
        "color" : 'tab:orange'
    },
    {
        "marker" : 'P',
        "color" : 'tab:green'
    },
    {
        "marker" : '*',
        "color" : 'tab:brown'
    }
]
MARKERSIZE = 8
for style in styles:
    style["markersize"] = MARKERSIZE
    style["linestyle"] = '-'
    style["linewidth"] = 1.5
    style["alpha"] = 0.7


def plot_instances_nodes(dicts: List[Dict], instances_num: int = 200, **kwargs):
    STEP = 10
    X_THRESHOLD = 5
    dicts = [dict for dict in filter(lambda dict: dict["early_stopped"] == False, dicts)]
    dicts.sort(key= lambda dict: dict["nodes_visited"])

    x = [x for x in range(X_THRESHOLD, instances_num + 1, STEP)]
    y = []
    for p in x:
        if p <= len(dicts):
            y.append(dicts[p-1]["nodes_visited"])
        else:
            y.append(float('inf'))
    for p in range(len(y)):
        if y[p] <= 0.01:
            y[p] = None

    # plt.xticks(x)
    # plt.xlabel("# Solved Instances")
    # plt.ylabel("Time")

    return plt.semilogx(y[:], x[:], **kwargs)

algorithms = ["basic-k", "basic-rs", "dual-rs","basic-mg","basic-mg-rs","dual-mg-rs"]
LABELS = ["basic-k", "basic-rs", "dual-rs","basic-mg","basic-mg-rs","dual-mg-rs"]
input_dir = "./result/COV"
if len(argv) >= 2:
    input_dir = argv[1]
graph_name = "COV"
if len(argv) >= 3:
    graph_name = argv[2]

dicts = get_dicts(input_dir)

def plot_node_graph(graph:str):
    os.makedirs("./figure/nodes", exist_ok=True)
    fig, ax = plt.subplots(figsize=(5.2, 5.2))
    for eid, algorithm_name in enumerate(algorithms):
        plot_instances_nodes(dicts[algorithm_name], label=algorithm_name, **styles[eid])
    plt.grid()
    plt.subplots_adjust(left=0.1, right=0.95, bottom=0.1, top=0.95)
    plt.savefig(f"./figure/nodes/{graph}.svg")

    handles, labels = ax.get_legend_handles_labels()
    legend_fig, legend_ax = plt.subplots(figsize=(10, 0.5))
    legend_ax.axis("off")
    legend_ax.legend(
        handles,
        labels,
        loc="center",
        ncol=len(labels),
        frameon=False,
        handlelength=2,
        columnspacing=5,
    )
    legend_fig.savefig("./figure/nodes/legend.svg", bbox_inches="tight")
    plt.close(legend_fig)

    plt.close(fig)

plot_node_graph(graph_name)