"""P2-B の benchmark (math-export-p2b-matrix.ps1) を、区間 × 分け方ごとの中央値と幅にまとめる。

値は report から読むだけ。上限で打ち切った run も失敗した run も数に入れて表示する。
pre-YUV の digest は、同じ区間の「分けない」run (run1) と frame ごとに比べる。

使い方: python math-export-p2b-summary.py <run directory> [--json <出力>]
"""

import csv
import glob
import json
import os
import statistics
import sys


def digests(directory):
    path = os.path.join(directory, 'v4-full-pre-yuv.tsv')
    if not os.path.exists(path):
        return {}
    return {row[0]: row[2] for row in csv.reader(open(path, encoding='utf-8'), delimiter='\t')}


def main():
    root = sys.argv[1]
    outcomes = json.load(open(os.path.join(root, 'outcomes.json'), encoding='utf-8-sig'))
    cells = {}
    for outcome in outcomes:
        directory = os.path.join(root, outcome['window'], outcome['variant'], f"run{outcome['run']}")
        entry = dict(exit_code=outcome['exit_code'], sequence=outcome['sequence'], directory=directory)
        path = os.path.join(directory, 'v4-full-report.json')
        if os.path.exists(path):
            report = json.load(open(path, encoding='utf-8'))
            probe = report['property_probe']
            stages = {item['stage']: item['t'] for item in report['mlt_stages']}
            render = stages.get('consumer_close', stages.get('failed', 0)) - stages.get('render', 0)
            encoded = next(p['count'] for p in report['points'] if p['point'] == 'encoder_submit')
            rect_calls = probe['mlt_properties_anim_get_rect']['rect']['calls']
            entry.update(
                verdict=report['verdict'], images=probe['composited_images'], encoded=encoded,
                transitions=report['overlay_transitions'], keyframes=probe['animation_keyframes'],
                compose_ms=probe['compose_ms_per_image'],
                string_ms=probe['animated_string_get_ms_per_image'],
                frame_request_ms=report['frame_request_ms_per_image'],
                # affine の呼び出し回数 (= その frame で有効な transition の数) の目安。
                affine_calls_per_image=rect_calls / max(1, probe['composited_images']),
                render_s=render, wall_s=report['wall_seconds'], cpu_s=report['cpu_seconds'],
                render_s_per_image=render / max(1, probe['composited_images']),
                cpu_per_wall=report['cpu_seconds'] / report['wall_seconds'] if report['wall_seconds'] else 0,
                graph_build_s=probe['graph_build_seconds'])
        cells.setdefault((outcome['window'], outcome['variant']), []).append(entry)

    rows = []
    for (window, variant), runs in cells.items():
        measured = [run for run in runs if 'compose_ms' in run]
        row = dict(window=window, variant=variant, runs=len(runs), measured=len(measured),
                   exit_codes=[run['exit_code'] for run in runs])
        for key in ('compose_ms', 'string_ms', 'frame_request_ms', 'render_s_per_image', 'cpu_per_wall',
                    'affine_calls_per_image', 'graph_build_s'):
            values = [run[key] for run in measured]
            if values:
                row[key] = dict(median=statistics.median(values), min=min(values), max=max(values))
        if measured:
            row['transitions'] = measured[0]['transitions']
            row['keyframes'] = measured[0]['keyframes']
            row['images'] = [run['images'] for run in measured]
        # digest: 同じ区間の none/run1 と比べる。比べた frame が 0 なら一致とは言わない。
        reference = digests(os.path.join(root, window, 'none', 'run1'))
        compared = differing = 0
        for run in runs:
            own = digests(run['directory'])
            common = set(own) & set(reference)
            compared += len(common)
            differing += sum(own[frame] != reference[frame] for frame in common)
        row['digest_frames_compared'] = compared
        row['digest_frames_differing'] = differing
        rows.append(row)

    order = {name: index for index, name in enumerate(
        ['none', '64-8', '128-16', '256-32', '512-64', '64-8-chunk240', '64-8-chunk60'])}
    rows.sort(key=lambda row: (['early', 'middle', 'late'].index(row['window']), order.get(row['variant'], 99)))
    span = lambda cell: f"{cell['median']:7.1f} [{cell['min']:6.1f}..{cell['max']:6.1f}]"
    for row in rows:
        if 'compose_ms' not in row:
            print(f"{row['window']:7} {row['variant']:14} 計測なし exit={row['exit_codes']}")
            continue
        print(f"{row['window']:7} {row['variant']:14} n={row['measured']}/{row['runs']} exit={row['exit_codes']} "
              f"tr={row['transitions']:5} key={row['keyframes']:6} | 合成 {span(row['compose_ms'])} "
              f"文字列化 {span(row['string_ms'])} frame取得 {span(row['frame_request_ms'])} ms | "
              f"render/img {row['render_s_per_image']['median']:.3f} "
              f"[{row['render_s_per_image']['min']:.3f}..{row['render_s_per_image']['max']:.3f}] s "
              f"cpu/wall {row['cpu_per_wall']['median']:.2f} | digest {row['digest_frames_compared']} frame "
              f"不一致 {row['digest_frames_differing']}")
    if '--json' in sys.argv:
        with open(sys.argv[sys.argv.index('--json') + 1], 'w', encoding='utf-8') as out:
            json.dump(rows, out, ensure_ascii=False, indent=1)


if __name__ == '__main__':
    main()
