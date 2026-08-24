#!/usr/bin/env python3
"""再生 .bt DSL golden JSON（bernardx C++ 差分测试基线）。

用法:
    python3 tools/gen_bt_dsl_goldens.py --agent2 /path/to/bernard-agent2

读取 tests/data/bt_dsl/*.bt，用 bernard-agent2 的 ``compile_text`` 编译，
写 ``<name>.golden.json``（indent=2, ensure_ascii=False）。goldens 提交入库；
C++ 侧（tests/bt_dsl_test.cc）按键序+内容一致比对。改语法/语义时先改
agent2 侧 dsl.py，再生本基线，再同步 C++ 实现。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--agent2", required=True, help="bernard-agent2 仓库根目录")
    args = ap.parse_args()

    sys.path.insert(0, str(Path(args.agent2) / "src"))
    from bernard_agent2.bt_dsl import compile_text  # noqa: E402

    data_dir = Path(__file__).resolve().parent.parent / "tests" / "data" / "bt_dsl"
    bt_files = sorted(data_dir.glob("*.bt"))
    if not bt_files:
        print(f"no .bt corpus under {data_dir}", file=sys.stderr)
        return 1
    for bt in bt_files:
        tree = compile_text(bt.read_text(encoding="utf-8"))
        out = bt.with_suffix(".golden.json")
        out.write_text(json.dumps(tree, indent=2, ensure_ascii=False) + "\n",
                       encoding="utf-8")
        print(f"{bt.name} -> {out.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
