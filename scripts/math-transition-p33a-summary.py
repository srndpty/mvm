"""P3-3A: math-transition-p33a-verify.ps1 の複数の run の summary.json を 1 つの表にまとめる。

使い方: python scripts/math-transition-p33a-summary.py <run directory> [<run directory> ...]

時間は fixture ごとに、各 run の値と中央値を出す。baseline は同じ process の中で 2 回書き出して
いる (candidate の前と後)。1 回目は 2 回目より常に遅い (起動直後の影響) ので、速度の比は
candidate と 2 回目の baseline の比で出す (candidate に不利な側)。
"""
import json
import statistics
import sys
from pathlib import Path


def load(directory):
    return json.loads((Path(directory) / 'summary.json').read_text(encoding='utf-8-sig'))


def main():
    runs = [load(directory) for directory in sys.argv[1:]]
    if not runs:
        raise SystemExit(__doc__)
    names = []
    for run in runs:
        for row in run['fixtures']:
            if row['name'] not in names:
                names.append(row['name'])
    out = []
    for name in names:
        rows = [row for run in runs for row in run['fixtures'] if row['name'] == name]
        first = rows[0]

        def values(key):
            return [row[key] for row in rows if row.get(key) is not None]

        def median(key):
            data = values(key)
            return statistics.median(data) if data else None

        candidate = median('candidate_render_seconds')
        repeat = median('repeat_render_seconds')
        entry = {
            'name': name,
            'runs': len(rows),
            'verdicts': [row['verdict'] for row in rows],
            'frames': first['expected_frames'],
            'frames_exact': [row['frames_exact'] for row in rows],
            'grouped_clips': first['grouped_clips'],
            'retained_clips': first['retained_clips'],
            'retained_geometry': first.get('retained_geometry'),
            'retained_above': first.get('retained_above'),
            'affine_transitions': first['affine_transitions'],
            'raster_megabytes': first['raster_megabytes'],
            'prepare_ms': median('prepare_ms'),
            'copy_ms_per_frame': median('copy_ms_per_frame'),
            'compose_ms_per_frame': median('compose_ms_per_frame'),
            'callback_ms_per_frame': median('callback_ms_per_frame'),
            'baseline_first_render_seconds': values('baseline_render_seconds'),
            'candidate_render_seconds': values('candidate_render_seconds'),
            'baseline_repeat_render_seconds': values('repeat_render_seconds'),
            'candidate_median': candidate,
            'baseline_repeat_median': repeat,
            'speedup_vs_repeat_median': repeat / candidate if candidate and repeat else None,
            'speedup_vs_repeat_each': [
                row['repeat_render_seconds'] / row['candidate_render_seconds'] for row in rows
                if row.get('candidate_render_seconds') and row.get('repeat_render_seconds')
            ],
            'baseline_compose_ms_per_image': median('repeat_compose_ms_per_image'),
            'candidate_compose_ms_per_image': median('candidate_compose_ms_per_image'),
            'live_after_export': [row['live_after_export'] for row in rows],
        }
        out.append(entry)
        print('%-18s %s 一致 %s/%s まとめた %s 残した %s (配置 %s / 下に従来の layer %s) '
              'render 秒 candidate %s / baseline(2 回目) %s 比 %s' % (
                  name, ','.join(entry['verdicts']), entry['frames_exact'], entry['frames'],
                  entry['grouped_clips'], entry['retained_clips'], entry['retained_geometry'],
                  entry['retained_above'],
                  ['%.1f' % v for v in entry['candidate_render_seconds']],
                  ['%.1f' % v for v in entry['baseline_repeat_render_seconds']],
                  ['%.2f' % v for v in entry['speedup_vs_repeat_each']]))
        print('    1 frame: 複製 %.3f ms、合成 %.3f ms、callback 全体 %.3f ms / 画像 %s MB、準備 %.0f ms / '
              '合成全体 (1 画像) baseline %.1f ms -> candidate %.1f ms' % (
                  entry['copy_ms_per_frame'] or 0, entry['compose_ms_per_frame'] or 0,
                  entry['callback_ms_per_frame'] or 0, entry['raster_megabytes'],
                  entry['prepare_ms'] or 0, entry['baseline_compose_ms_per_image'] or 0,
                  entry['candidate_compose_ms_per_image'] or 0))
    controls = [row for run in runs for row in run.get('controls', [])]
    for row in controls:
        print('control %-18s exit %s %s 検出 %s' % (row['name'], row['exit_code'], row.get('verdict'),
                                                 row.get('detected')))
    # run の directory の中へは書かない。隣へ新しい名前で置き、既にあれば上書きしない。
    target = Path(sys.argv[-1]).parent / ('combined-' + '+'.join(
        Path(directory).name for directory in sys.argv[1:]) + '.json')
    if target.exists():
        raise SystemExit('既にあります: %s' % target)
    target.write_text(json.dumps({'runs': sys.argv[1:], 'fixtures': out, 'controls': controls},
                                 ensure_ascii=False, indent=1), encoding='utf-8')
    print('wrote', target)


if __name__ == '__main__':
    main()
