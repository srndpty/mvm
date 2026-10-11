"""部分式の位置と動きを、編集可能な既存の数式 clip に変換する。

Manim の API や時間、描画結果には依存しない。式の中央揃えは行わず、
画面内の同一の座標系を全状態で使う。位置変更には明示的な許可が必要。
"""

import argparse
import copy
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import uuid


class ScoreError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ScoreError(message)


def integer(value, minimum, name):
    require(type(value) is int and value >= minimum, f"{name}は{minimum}以上の整数が必要です")
    return value


def number(value, name):
    require(type(value) in (int, float) and math.isfinite(value), f"{name}は有限の数値が必要です")
    return value


def validate_score(score):
    require(score.get('format') == 'mvm-math-score/1', '数式配置の形式が不正です')
    require(score.get('motion_interpolation', 'ease_out') in
            ('linear', 'ease_in', 'ease_out', 'ease_in_out'), '移動の補間が不正です')
    width, height = score['canvas']
    integer(width, 1, '画面幅')
    integer(height, 1, '画面高さ')
    numerator, denominator = score['fps']
    integer(numerator, 1, 'fpsの分子')
    integer(denominator, 1, 'fpsの分母')
    objects = score['objects']
    require(isinstance(objects, dict) and objects, '部分式が必要です')
    for identifier, item in objects.items():
        require(isinstance(identifier, str) and identifier, '部分式IDが必要です')
        require(isinstance(item.get('source'), str) and item['source'].strip(),
                f'{identifier}: 空でないLaTeXが必要です')
        integer(item.get('font_size', 96), 1, '文字サイズ')
    require(isinstance(score['states'], list) and score['states'], '状態が必要です')
    previous, seen, used = {}, set(), set()
    start = 0
    for index, state in enumerate(score['states']):
        identifier = state['id']
        require(isinstance(identifier, str) and identifier and identifier not in seen,
                '状態IDが空、または重複しています')
        seen.add(identifier)
        duration = integer(state['duration_frames'], 1, '状態の尺')
        frames = integer(state['transition_frames'], 0, '変化の尺')
        exit_frames = integer(state.get('exit_frames', frames), 0, '消去の尺')
        require(exit_frames <= frames, '消去の尺が変化の尺を超えています')
        require(frames < duration and (index != 0 or frames == 0),
                f'{identifier}: 変化の尺は状態の尺未満、最初の状態は0が必要です')
        if state.get('subtitle_cue_id'):
            begin = integer(state.get('subtitle_start_offset_frames', 0), 0, '字幕開始の差分')
            padding = integer(state.get('subtitle_end_padding_frames', 0), 0, '字幕終了前の余白')
            require(begin + padding < duration, '字幕の区間が状態の尺に収まりません')
        parts = state['parts']
        require(isinstance(parts, dict) and parts, f'{identifier}: 表示する部分式が必要です')
        require(set(state.get('highlight', [])) <= parts.keys(), '強調する部分式が存在しません')
        moves, fixed = set(state.get('moves', [])), set(state.get('fixed', []))
        require(moves <= parts.keys() and fixed <= parts.keys(), '動き・固定の対象が存在しません')
        require(not (moves & fixed), '同じ部分式を移動と固定に指定できません')
        for part, position in parts.items():
            require(part in objects, f'{part}: 未定義の部分式です')
            used.add(part)
            x, y = number(position['x'], 'x座標'), number(position['y'], 'y座標')
            opacity = number(position.get('opacity', 100), '不透明度')
            for attribute in ('scale_x', 'scale_y'):
                require(number(position.get(attribute, 100), '拡大率') > 0, '拡大率は正の値が必要です')
            require(0 <= opacity <= 100, '不透明度は0〜100が必要です')
            require(0 <= x <= width and 0 <= y <= height, '部分式の中心が画面外です')
            old = previous.get(part)
            entrance = position.get('enter_from')
            if entrance:
                require(frames > 0 and (old is None or old.get('opacity', 100) == 0),
                        '滑り込む開始位置は非表示だった部品にだけ指定できます')
                require(0 <= number(entrance['x'], '開始x座標') <= width
                        and 0 <= number(entrance['y'], '開始y座標') <= height,
                        '滑り込む開始位置が画面外です')
            delay = integer(position.get('enter_delay_frames', 0), 0, '出現を遅らせるframe数')
            require(delay <= frames, '出現を遅らせるframe数が変化の尺を超えています')
            require(not delay or old is None or old.get('opacity', 100) == 0,
                    '出現の遅延は非表示だった部分式にだけ指定できます')
            moved = old and ((old['x'], old['y']) != (x, y) or any(
                old.get(attribute, 100) != position.get(attribute, 100)
                for attribute in ('scale_x', 'scale_y')))
            if moved and old.get('opacity', 100) > 0 and opacity > 0:
                require(part in moves and frames > 0,
                        f'{identifier}/{part}: 表示中の位置変更にはmovesと変化の尺が必要です')
            require(not (moved and part in fixed), f'{identifier}/{part}: 固定する項が移動しています')
        previous = parts
        start += duration
    require(used == objects.keys(), '一度も使用しない部分式があります')
    return start


def key(frame, value, interpolation='ease_in_out'):
    return dict(frame=frame, value=value, interpolation=interpolation, curve_start=0, curve_end=1)


def curve(events, attribute, start, duration, convert, interpolation='ease_in_out'):
    """同じ値の区間を除いて、端点と保持区間を明示した曲線を作る。"""
    values = {}
    for frame, placement in events:
        if start <= frame < start + duration:
            values[frame - start] = convert(placement.get(attribute, 100))
    ordered = sorted(values.items())
    # 定数はキーを作らず、基準値だけにする。
    if len({value for _, value in ordered}) == 1:
        return ordered[0][1], []
    reduced = []
    for index, item in enumerate(ordered):
        if 0 < index < len(ordered) - 1 and ordered[index - 1][1] == item[1] == ordered[index + 1][1]:
            continue
        reduced.append(key(*item, interpolation=interpolation))
    return ordered[0][1], reduced


def compile_score(score, project):
    """入力は変更しない。既存の字幕の内容・書式、背景、その他の素材を保持する。"""
    total = validate_score(score)
    require(score['canvas'] == [project['output_width'], project['output_height']],
            '数式配置とProjectの画面サイズが違います')
    require(score['fps'] == [project['timeline_fps_num'], project['timeline_fps_den']],
            '数式配置とProjectのfpsが違います')
    result = copy.deepcopy(project)
    removed = {c['id'] for c in result['timeline_clips']
               if c['kind'] == 'math' and c['id'] not in score.get('preserve_clip_ids', [])}
    templates = [c for c in result['timeline_clips'] if c['kind'] == 'math']
    require(bool(templates), 'Projectに数式clipのひな型が必要です')
    template = templates[0]
    result['timeline_clips'] = [c for c in result['timeline_clips'] if c['id'] not in removed]
    result['timeline_transitions'] = [t for t in result['timeline_transitions']
                                    if not ({t['incoming_clip_id'], t['outgoing_clip_id']} & removed)]
    used_tracks = sorted({c['track_index'] for c in result['timeline_clips'] if c['track_kind'] == 'video'})
    track_map = {old: new for new, old in enumerate(used_tracks)}
    result['video_tracks'] = [result['video_tracks'][i] for i in used_tracks]
    for c in result['timeline_clips']:
        if c['track_kind'] == 'video':
            c['track_index'] = track_map[c['track_index']]
    width, height = score['canvas']
    intervals, stages, start = [], [], 0
    for state in score['states']:
        stages.append(dict(id=state['id'], start=start, duration=state['duration_frames'],
                           transition_frames=state['transition_frames'],
                           exit_frames=state.get('exit_frames', state['transition_frames'])))
        start += state['duration_frames']
    for part, item in score['objects'].items():
        events, current, visible = [], None, []
        for state, stage in zip(score['states'], stages):
            target = state['parts'].get(part)
            at, frames = stage['start'], stage['transition_frames']
            if target is not None:
                target = dict(target, opacity=target.get('opacity', 100))
                if current is None:
                    current = dict(target, opacity=0 if at else target['opacity'])
                if target.get('enter_from'):
                    current = dict(target, **target['enter_from'], opacity=0)
                events.append((at, current))
                # 非表示だった部分は目的の座標で現れる。見えない移動をアニメーションしない。
                if current['opacity'] == 0:
                    if not target.get('enter_from'):
                        events[-1] = (at, dict(target, opacity=0 if frames else target['opacity']))
                    if target.get('enter_delay_frames', 0):
                        events.append((at + target['enter_delay_frames'], events[-1][1]))
                events.append((at + frames, target))
                visible.append((at, at + stage['duration']))
                current = target
            elif current is not None:
                events.append((at, current))
                current = dict(current, opacity=0)
                events.append((at + stage['exit_frames'], current))
        # 連続した表示だけを同じclipにする。非表示の長い空白をtrackの占有に数えない。
        runs = []
        for begin, end in visible:
            if runs and runs[-1][1] == begin:
                runs[-1] = (runs[-1][0], end)
            else:
                runs.append((begin, end))
        for begin, end in runs:
            next_stage = next((s for s in stages if s['start'] == end), None)
            if next_stage:
                end += next_stage['exit_frames'] + 1
            end = min(end, total)
            duration = end - begin
            c = copy.deepcopy(template)
            owner = next(s['id'] for s in stages if s['start'] == begin)
            clip_id = str(uuid.uuid5(uuid.NAMESPACE_URL, f"mvm-math-score:{score.get('id', '')}:{part}:{owner}"))
            c.update(id=clip_id, name=item.get('label', part), timeline_start_frame=begin,
                     source_in_frame=0, source_out_frame=duration, source_frame_count=duration,
                     source_fps_num=score['fps'][0], source_fps_den=score['fps'][1],
                     speed_num=1, speed_den=1, enabled=True, frame_hold=None, track_kind='video',
                     media_path='', media_item_id='', link_group_id='')
            c.pop('equation_sequence', None)
            c['math'] = dict(syntax='latex', source=item['source'], font_size=item.get('font_size', 96),
                             color=item.get('color', '#FF000000'), background_color='#00000000')
            c['math_animation'] = dict(intro='none', intro_frames=0)
            effects = c['effects']
            for name in effects:
                if name.endswith('_keys'):
                    effects[name] = []
            effects.update(position_x_percent=0, position_y_percent=0, scale_x_percent=100,
                           scale_y_percent=100, rotation_degrees=0, opacity_percent=100,
                           crop_left_percent=0, crop_right_percent=0, crop_top_percent=0,
                           crop_bottom_percent=0, fade_in_frames=0, fade_out_frames=0)
            for attribute, base, channel, convert in [
                ('x', 'position_x_percent', 'position_x_keys', lambda x: (x / width - 0.5) * 100),
                ('y', 'position_y_percent', 'position_y_keys', lambda y: (y / height - 0.5) * 100),
                ('scale_x', 'scale_x_percent', 'scale_x_keys', lambda scale: scale),
                ('scale_y', 'scale_y_percent', 'scale_y_keys', lambda scale: scale),
                ('opacity', 'opacity_percent', 'opacity_keys', lambda opacity: opacity),
            ]:
                interpolation = ('ease_in_out' if attribute == 'opacity' else
                                 score.get('motion_interpolation', 'ease_out'))
                effects[base], effects[channel] = curve(events, attribute, begin, duration, convert, interpolation)
            intervals.append((begin, end, c, part))
            # 赤い字形を同じ動きの上に重ねる。位置の曲線を作り直さず、色だけを一時強調する。
            highlight_events = [(begin, dict(opacity=0))]
            for state, stage in zip(score['states'], stages):
                at, frames = stage['start'], stage['transition_frames']
                if not (begin <= at < end) or part not in state.get('highlight', []):
                    continue
                target = state['parts'][part]
                rise = min(max(frames, 12), stage['duration'] - 1)
                finish = min(stage['duration'] - 1, rise + 126)
                hold = max(rise, finish - 36)
                for offset, opacity in [(0, 0), (target.get('enter_delay_frames', 0), 0),
                                        (min(rise, finish), target.get('opacity', 100)),
                                        (hold, target.get('opacity', 100)), (finish, 0)]:
                    highlight_events.append((at + offset, dict(opacity=opacity)))
            if len(highlight_events) > 1:
                overlay = copy.deepcopy(c)
                overlay['id'] = str(uuid.uuid5(uuid.NAMESPACE_URL, f'{clip_id}:highlight'))
                overlay['name'] += '（変更箇所の強調）'
                overlay['math']['color'] = score.get('highlight_color', '#FFCC2222')
                overlay['effects']['opacity_percent'], overlay['effects']['opacity_keys'] = curve(
                    highlight_events, 'opacity', begin, duration, lambda opacity: opacity)
                intervals.append((begin, end, overlay, part + '/highlight'))
    # 同時に見えるclipだけを別trackに置く。背景などの元のtrackは保持する。
    base_track = len(result['video_tracks'])
    track_ends, allocation = [], {}
    for begin, end, c, part in sorted(intervals, key=lambda row: (row[0], row[1], row[3])):
        lane = next((i for i, finish in enumerate(track_ends) if finish <= begin), len(track_ends))
        if lane == len(track_ends):
            track_ends.append(end)
            result['video_tracks'].append(dict(project['video_tracks'][0], name=f'部分式 {lane + 1}'))
        else:
            track_ends[lane] = end
        c['track_index'] = base_track + lane
        result['timeline_clips'].append(c)
        allocation.setdefault(part, []).append(c['id'])
    subtitles = result.get('subtitles') or {}
    for cue in subtitles.get('cues', []):
        if cue.get('link_clip_id') in removed:
            # 長い部分式に字幕を束ねると、移動時に全字幕が追従してしまう。リンクを外し時刻を保つ。
            cue.pop('link_clip_id')
    cue_by_id = {c['id']: c for c in subtitles.get('cues', [])}
    bound_cues = set()
    for state, stage in zip(score['states'], stages):
        cue_id = state.get('subtitle_cue_id')
        if cue_id:
            require(cue_id in cue_by_id and cue_id not in bound_cues,
                    '字幕の対応先が存在しない、または複数の状態が同じ字幕を参照しています')
            bound_cues.add(cue_id)
            cue = cue_by_id[cue_id]
            cue['start_frame'] = stage['start'] + state.get('subtitle_start_offset_frames', 0)
            cue['end_frame'] = stage['start'] + stage['duration'] - state.get('subtitle_end_padding_frames', 0)
    result['timeline_markers'] = sorted(set(s['start'] for s in stages) |
                                        set(s['start'] + min(s['transition_frames'] + 90, s['duration'] - 1)
                                            for s in stages))
    report = dict(format='mvm-math-score-report/1', frames=total, stages=stages,
                  native_clips=len(intervals), native_tracks=len(track_ends), objects=allocation)
    return result, report


def main():
    parser = argparse.ArgumentParser(description='部分式の配置と動きを編集可能な数式clipに変換します')
    parser.add_argument('--score', required=True, help='数式配置JSON')
    parser.add_argument('--project', required=True, help='字幕・背景を引き継ぐProject')
    parser.add_argument('--output', required=True, help='新しいProjectの保存先')
    parser.add_argument('--report', required=True, help='配置・時刻の検証結果')
    parser.add_argument('--validator', required=True, help='製品のProject検証CLI')
    args = parser.parse_args()
    try:
        inputs = {Path(args.project).resolve(), Path(args.score).resolve()}
        output_path, report_path = Path(args.output), Path(args.report)
        require(output_path.resolve() not in inputs and report_path.resolve() not in inputs
                and output_path.resolve() != report_path.resolve(),
                '入力・Project出力・検証結果には別のファイルを指定してください')
        score = json.loads(Path(args.score).read_text(encoding='utf-8'))
        project = json.loads(Path(args.project).read_text(encoding='utf-8'))
        result, report = compile_score(score, project)
        # 出力先を変えても、背景・素材の相対パスを元のファイルへ解決する。
        origin = Path(args.project).resolve().parent
        destination = Path(args.output).resolve().parent
        for item in result['timeline_clips'] + result.get('media_items', []):
            media = item.get('media_path', '')
            if media and not Path(media).is_absolute():
                item['media_path'] = os.path.relpath(origin / media, destination)
        # Projectの検査は製品の公開関数が正。候補を検証してから公開する。
        with tempfile.NamedTemporaryFile(dir=output_path.parent, suffix='.mvm', delete=False) as temporary:
            candidate = Path(temporary.name)
        try:
            candidate.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
            checked = subprocess.run([args.validator, str(candidate)], capture_output=True)
            require(checked.returncode == 0,
                    '製品のProject検証が失敗しました: ' + checked.stderr.decode('utf-8', errors='replace'))
            candidate.replace(output_path)
        finally:
            candidate.unlink(missing_ok=True)
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    except (ScoreError, KeyError, TypeError, ValueError, OSError) as error:
        parser.exit(1, f'数式配置を変換できません: {error}\n')
    print(f"部分式を変換しました: {report['native_clips']}clip、{report['frames']}frame")


if __name__ == '__main__':
    main()
