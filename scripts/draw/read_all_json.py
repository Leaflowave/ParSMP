import os
import json
from typing import Any, List
import orjson

def get_files(dir_path):
    with os.scandir(dir_path) as it:
        for entry in it:
            if entry.is_file():
                yield entry.path

def read_json(file_path):
    with open(file_path, 'r') as f:
        return orjson.loads(f.read())

def get_dicts(dir_path) -> dict[str, List[dict]]:
    dicts: dict[str, List[dict]]= {}
    for file_path in get_files(dir_path):
        js = read_json(file_path)
        # 匹配 .json 前到第一个下划线之间的字符串
        algorithm_name = file_path.split(".json")[0].split("_")[-1]
        if algorithm_name not in dicts:
            dicts[algorithm_name] = []
        dicts[algorithm_name].append(js)
        
    return dicts
