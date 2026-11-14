#!/usr/bin/env python3
"""bfmem_test perf のログ(両方のノード)から doc/spec/perf.json の測定値を作る。

    python3 doc/spec/perf_import.py <名前0>=<ログ0> <名前1>=<ログ1>

ログの「perf|項目|長さ|コア|IOPS|MB/s|平均の遅延 ns|誤り」の行と、「perf|項目|長さ|-|対象外…」の行を読む。
perf.json に前からある説明の文(intro・conditions・latency_note・notes)はそのまま残し、nodes・cores だけを置き換える。
仕様書の 20 章(gen.py の perf_chapter())は perf.json から作る。
"""
import json
import os
import sys

SPEC = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(SPEC, "perf.json")


def read_log(path):
    res = []
    with open(path, encoding="utf-8", errors="replace") as fp:
        for ln in fp:
            if not ln.startswith("perf|") or ln.startswith("perf|項目"):
                continue
            f = ln.rstrip("\n").split("|")
            if f[3] == "-":
                res.append({"item": f[1], "size": int(f[2]), "cores": 0, "skip": f[4]})
                continue
            res.append({"item": f[1], "size": int(f[2]), "cores": int(f[3]), "iops": float(f[4]),
                        "mbps": float(f[5]), "lat_ns": float(f[6]), "err": int(f[7])})
    return res


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    pd = {}
    if os.path.exists(OUT):
        with open(OUT, encoding="utf-8") as fp:
            pd = json.load(fp)
    nodes, cores = [], set()
    for arg in sys.argv[1:]:
        name, path = arg.split("=", 1)
        rs = read_log(path)
        # 対象外の行は、表とグラフで全部のコアの欄に「—」を出せるよう、コアごとに広げる
        real = [r for r in rs if not r.get("skip")]
        cores |= {r["cores"] for r in real}
        out = list(real)
        for r in rs:
            if r.get("skip"):
                for c in sorted({x["cores"] for x in real}):
                    out.append({"item": r["item"], "size": r["size"], "cores": c, "skip": r["skip"]})
        bad = sum(r["err"] for r in real)
        print("%s: %d 件(対象外 %d 件、誤りのあった測定 %d 件)" % (name, len(real), len(out) - len(real), bad))
        nodes.append({"name": name, "results": out})
    pd["nodes"] = nodes
    pd["cores"] = sorted(cores)
    for k in ("intro", "conditions", "latency_note", "notes"):
        pd.setdefault(k, "(書くこと)")
    with open(OUT, "w", encoding="utf-8") as fp:
        json.dump(pd, fp, ensure_ascii=False, indent=1)
        fp.write("\n")
    print("OK: %s" % os.path.relpath(OUT))


if __name__ == "__main__":
    main()
