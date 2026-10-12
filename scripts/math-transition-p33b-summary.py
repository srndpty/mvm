"""P3-3B: math-transition-p33b-verify.ps1 の summary.json を読みやすく出す。

使い方: python scripts/math-transition-p33b-summary.py <run directory> [<run directory> ...]

時間 (timing) は、区間ごとに製品の経路 (B) と candidate (C) を別々の process で B, C, C, B の順に
書き出したもの。複数の run を渡すと、同じ区間・同じ種類の値をまとめて中央値を出す。
"""
import json
import statistics
import sys
from pathlib import Path


def load(directory):
    return json.loads((Path(directory) / 'summary.json').read_text(encoding='utf-8-sig'))


def rows_of(runs, key):
    return [row for run in runs for row in (run.get(key) or [])]


def main():
    runs = [load(directory) for directory in sys.argv[1:]]
    if not runs:
        raise SystemExit(__doc__)
    for key in ('fixtures', 'alpha', 'windows'):
        for row in rows_of(runs, key):
            print('%-9s %-20s exit %s %-16s 一致 %s/%s 未比較 %s group %s まとめた %s 残した %s (配置 %s) '
                  '下地の最小 alpha %s (字形 %s; 254: %s, 253: %s, それ未満: %s) affine %s->%s '
                  'render B/C/B %.1f/%.1f/%.1f 画像 %s MB 準備 %.0f ms ws %.0f->%.0f MB callback %.2f ms' % (
                      key, row['name'], row['exit_code'], row['verdict'], row['frames_exact'],
                      row['expected_frames'], row['frames_uncompared'], row['groups'],
                      row['grouped_clips'], row['retained_clips'], row['retained_geometry'],
                      row['destination_minimum_alpha'], row['glyph_roi_minimum_alpha'],
                      row['destination_alpha_254'], row['destination_alpha_253'],
                      row['destination_alpha_below_253'], row['baseline_affine_transitions'],
                      row['affine_transitions'], row['baseline_render_seconds'] or 0,
                      row['candidate_render_seconds'] or 0, row['repeat_render_seconds'] or 0,
                      row['raster_megabytes'], row['prepare_ms'] or 0,
                      row['working_set_before_prepare_mb'] or 0,
                      row['working_set_after_prepare_mb'] or 0, row['callback_ms_per_frame'] or 0))
            if row.get('expectation') == 'rejected':
                print('          拒否 %s: %s' % (row.get('detected'), row.get('candidate_error')))
    for row in rows_of(runs, 'controls'):
        print('control   %-36s exit %s %-18s 検出 %s' % (row['name'], row['exit_code'], row.get('verdict'),
                                                      row.get('detected')))
    timing = rows_of(runs, 'timing')
    out = []
    for window in sorted({row['window'] for row in timing}):
        entry = {'window': window}
        for kind in ('B', 'C'):
            rows = [row for row in timing if row['window'] == window and row['kind'] == kind]

            def values(key):
                return [row[key] for row in rows if row.get(key) is not None]

            entry[kind] = {
                'runs': len(rows),
                'render_seconds': values('render_seconds'),
                'export_call_wall_seconds': values('export_call_wall_seconds'),
                'cpu_seconds': values('cpu_seconds'),
                'peak_working_set_mb': values('peak_working_set_mb'),
                'compose_ms_per_image': values('compose_ms_per_image'),
                'affine_transitions': values('affine_transitions'),
                'encoder_submitted': values('encoder_submitted'),
                'muxed_packets': values('muxed_packets'),
                'callback_seconds_total': values('callback_seconds_total'),
                'grouped_clips': values('grouped_clips'),
                'exit_codes': values('exit_code'),
            }
        b, c = entry['B'], entry['C']
        if b['render_seconds'] and c['render_seconds']:
            entry['render_ratio_median'] = (statistics.median(b['render_seconds']) /
                                            statistics.median(c['render_seconds']))
            entry['render_ratio_worst'] = min(b['render_seconds']) / max(c['render_seconds'])
            entry['cpu_ratio_median'] = (statistics.median(b['cpu_seconds']) /
                                         statistics.median(c['cpu_seconds']))
            entry['export_ratio_median'] = (statistics.median(b['export_call_wall_seconds']) /
                                            statistics.median(c['export_call_wall_seconds']))
        out.append(entry)
        fmt = lambda data: '/'.join('%.1f' % v for v in data)
        print('timing v4-full@%s render 秒 B %s C %s 比 中央値 %.2f (最悪 %.2f) | 書き出し全体 B %s C %s 比 %.2f | '
              'CPU 秒 B %s C %s 比 %.2f | peak MB B %s C %s | affine B %s C %s | encoder B %s C %s mux B %s C %s | '
              'group の callback 合計 %s 秒' % (
                  window, fmt(b['render_seconds']), fmt(c['render_seconds']),
                  entry.get('render_ratio_median', 0), entry.get('render_ratio_worst', 0),
                  fmt(b['export_call_wall_seconds']), fmt(c['export_call_wall_seconds']),
                  entry.get('export_ratio_median', 0), fmt(b['cpu_seconds']), fmt(c['cpu_seconds']),
                  entry.get('cpu_ratio_median', 0), fmt(b['peak_working_set_mb']),
                  fmt(c['peak_working_set_mb']), b['affine_transitions'], c['affine_transitions'],
                  b['encoder_submitted'], c['encoder_submitted'], b['muxed_packets'],
                  c['muxed_packets'], fmt(c['callback_seconds_total'])))
    # run の directory の中へは書かない。隣へ新しい名前で置き、既にあれば上書きしない。
    target = Path(sys.argv[-1]).parent / ('timing-' + '+'.join(
        Path(directory).name for directory in sys.argv[1:]) + '.json')
    if target.exists():
        raise SystemExit('既にあります: %s' % target)
    target.write_text(json.dumps(out, ensure_ascii=False, indent=1), encoding='utf-8')
    print('wrote', target)


if __name__ == '__main__':
    main()
