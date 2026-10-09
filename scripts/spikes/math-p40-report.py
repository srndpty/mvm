"""P4-0 の生 JSON から件数・計測値を再集計する。文書への手転記を避ける。"""

import argparse
import json
import pathlib


def main():
    parser = argparse.ArgumentParser(description='P4-0 の証拠の集計')
    parser.add_argument('report', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    opts = parser.parse_args()
    report = json.loads(opts.report.read_text(encoding='utf-8'))
    if report['failures'] or len(report['cases']) != 11 or not report['negative']:
        raise ValueError('失敗または空振りの report は閉鎖の集計に使えません')
    if [d['frames'] for d in report['draw']] != [1, 3] or not all(d['equal'] for d in report['draw']):
        raise ValueError('Draw の必須条件がありません')
    rows = ['# P4-0 実験の機械集計', '',
            '生成元: `' + opts.report.as_posix() + '`。`math-p40-report.py` が生成する。', '',
            f"式試作: {report['prototype']['passed']} 件通過。静止 matrix: {len(report['cases'])} 件通過。",
            f"負例: {len(report['negative'])} 件通過。記録された失敗: {len(report['failures'])} 件。", '',
            'toolchain: ' + report['manim'] + ' / ' + ' / '.join(report['toolchain'].values()), '',
            f"renderer 新規 process 起動＋import: {report['renderer_startup_seconds']:.6f} 秒（OS cache は制御していない）。", '',
            f"import 初期化: {report['import_seconds']:.6f} 秒（process 起動全体・cold start の測定ではない）。", '',
            '|条件|静止秒|PNG byte|RGBA frame byte|segment 数|未定義数|jump 切断数|中間 alpha 画素|透明画素 RGB 非零数|',
            '|---|---:|---:|---:|---|---|---|---:|---:|']
    for c in report['cases']:
        rows.append(f"|{c['name']}|{c['static_seconds']:.6f}|{c['bytes']}|{c['rgba_bytes']}|"
                    f"{c['segments']}|{c['undefined']}|{c['jumps']}|{c['partial_alpha']}|{c['transparent_rgb_nonzero']}|")
    rows += ['', '静止時間は式の子 process 起動・sampling・object 作成・PNG 保存を含む。',
             'RGBA frame byte は NumPy buffer の実サイズであり、process 全体の RAM と区別する。', '',
             '|Draw N|任意順 request|全画素一致|N 枚生成秒|検証全体秒|intro PNG 合計 byte|', '|---:|---|---|---:|---:|---:|']
    for d in report['draw']:
        rows.append(f"|{d['frames']}|{d['requests']}|{d['equal']}|{d['generation_seconds']:.6f}|{d['seconds']:.6f}|{d['bytes']}|")
    rows += ['', 'Draw 時間は静止 endpoint と任意順の再描画も含み、N 枚だけの生成時間ではない。',
             'N=3 は四つの異なる hash を要求し、N 端点と static も照合する。', '',
             'callback 比較: `' + json.dumps(report['backend_sampling'], ensure_ascii=False) + '`。', '',
             '解決済み色: `' + json.dumps(report['resolved_colors']) + '`。色ごとの不透明画素数: `' +
             json.dumps(report['opaque_curve_color_pixels']) + '`。', '',
             f"取得全体: {report['total_seconds']:.6f} 秒。取得ファイル合計: {report['artifact_bytes']} byte（report 自身を除く）。", '',
             '性能の PASS 閾値は設定しない。同一 run 内の計測であり、一般的な速度の保証ではない。', '']
    opts.output.write_text('\n'.join(rows), encoding='utf-8')
    print('集計を生成しました: ' + str(opts.output))


if __name__ == '__main__':
    main()
