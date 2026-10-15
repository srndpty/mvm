"""部分式の位置と動きを、編集可能な既存の数式 clip に変換する。

Manim の API や時間、描画結果には依存しない。式の中央揃えは行わず、
画面内の同一の座標系を全状態で使う。位置変更には明示的な許可が必要。

一つの配置は、自分の clip ID と所有記録に書いたマーカーだけを置き換える。
所有者の無い数式、他の配置の数式、空の track、利用者のマーカーは消さない。
所有者を決められない Project は、推測で移行せず失敗する。
"""

import argparse
import copy
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile


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


OWNERSHIP_KEY = 'math_score_ownership'
OWNERSHIP_FORMAT = 'mvm-math-score-ownership/1'
CLIP_PREFIX = 'mvm-math-score:'


def identifier_token(value, name):
    """clip ID へ埋め込む識別子。コロンは所有の区切りなので含めない。"""
    require(isinstance(value, str) and value.strip() == value and value and ':' not in value,
            f'{name}は空でなく、前後の空白とコロンを含まない文字列が必要です')
    return value


def id_list(value, name):
    require(isinstance(value, list), f'{name}は配列が必要です')
    require(all(isinstance(item, str) and item for item in value), f'{name}は空でないIDが必要です')
    require(len(value) == len(set(value)), f'{name}が重複しています')
    return value


def owned_clip_id(score_id, part, stage, highlight=False):
    clip_id = f'{CLIP_PREFIX}{score_id}:{part}:{stage}'
    if highlight:
        clip_id += ':highlight'
    return clip_id


def clip_score_owner(clip_id):
    """この compiler の所有 ID なら配置 ID を返す。形式が壊れていれば失敗する。"""
    if not isinstance(clip_id, str) or not clip_id.startswith(CLIP_PREFIX):
        return None
    pieces = clip_id[len(CLIP_PREFIX):].split(':')
    if len(pieces) == 3 and all(pieces):
        return pieces[0]
    if len(pieces) == 4 and all(pieces[:3]) and pieces[3] == 'highlight':
        return pieces[0]
    raise ScoreError(f'数式clipの所有IDが解釈できません: {clip_id}')


def parse_ownership(project):
    raw = project.get(OWNERSHIP_KEY)
    if raw is None:
        return {}
    require(isinstance(raw, dict) and set(raw) == {'format', 'scores'} and
            raw.get('format') == OWNERSHIP_FORMAT and isinstance(raw.get('scores'), dict),
            '数式配置の所有記録が不正です')
    parsed = {}
    for score_id, record in raw['scores'].items():
        identifier_token(score_id, '所有記録の配置ID')
        require(isinstance(record, dict) and
                set(record) == {'clip_ids', 'marker_frames', 'track_names'},
                f'{score_id}: 所有記録の項目が不正です')
        clip_ids, markers, track_names = record['clip_ids'], record['marker_frames'], record['track_names']
        require(isinstance(clip_ids, list) and isinstance(markers, list) and isinstance(track_names, list),
                f'{score_id}: 所有記録の一覧が不正です')
        require(all(isinstance(item, str) and item for item in clip_ids) and
                len(clip_ids) == len(set(clip_ids)),
                f'{score_id}: 所有するclip IDが空または重複しています')
        require(all(type(frame) is int and frame >= 0 for frame in markers) and
                markers == sorted(set(markers)),
                f'{score_id}: 所有するマーカーは0以上で、重複せず昇順である必要があります')
        require(all(isinstance(name, str) and name for name in track_names) and
                len(track_names) == len(set(track_names)),
                f'{score_id}: 所有するtrack名が空または重複しています')
        parsed[score_id] = dict(clip_ids=list(clip_ids), marker_frames=list(markers),
                                track_names=list(track_names))
    return parsed


def blank_video_track(name):
    return dict(name=name, muted=False, solo=False, mixer_name='', mixer_gain_db=0.0, mixer_pan=0.0)


def validate_score(score):
    require(score.get('format') == 'mvm-math-score/1', '数式配置の形式が不正です')
    identifier_token(score.get('id'), '配置ID')
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
        identifier_token(identifier, '部分式ID')
        require(isinstance(item.get('source'), str) and item['source'].strip(),
                f'{identifier}: 空でないLaTeXが必要です')
        integer(item.get('font_size', 96), 1, '文字サイズ')
    require(isinstance(score['states'], list) and score['states'], '状態が必要です')
    previous, seen, used = {}, set(), set()
    start = 0
    for index, state in enumerate(score['states']):
        identifier = state['id']
        identifier_token(identifier, '状態ID')
        require(identifier not in seen, '状態IDが重複しています')
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
    score_id = score['id']
    ownership = parse_ownership(result)
    require(isinstance(result.get('video_tracks'), list) and result['video_tracks'], '映像trackが必要です')
    require(isinstance(result.get('timeline_clips'), list), 'clipの一覧が必要です')
    require(isinstance(result.get('timeline_transitions'), list), 'トランジションの一覧が必要です')
    replace_ids = set(id_list(score.get('replace_clip_ids', []), '置き換える数式'))
    preserve_ids = set(id_list(score.get('preserve_clip_ids', []), '残す数式'))
    require(not (replace_ids & preserve_ids), '同じ数式を残すと置き換えるの両方に指定できません')
    by_id = {}
    for clip in result['timeline_clips']:
        require(isinstance(clip.get('id'), str) and clip['id'] and clip['id'] not in by_id,
                'clip IDが空または重複しています')
        by_id[clip['id']] = clip

    def require_math(clip_id, label):
        require(clip_id in by_id and by_id[clip_id].get('kind') == 'math',
                f'{label}の数式clipがありません: {clip_id}')

    owned_ids = {clip['id'] for clip in result['timeline_clips']
                 if clip.get('kind') == 'math' and clip_score_owner(clip['id']) == score_id}
    # 所有記録が無ければ、以前に足したtrackとマーカーを特定できない。マーカーが0件でも
    # trackの記録は失われているので、clip IDだけを根拠に推測で再生成しない。
    require(score_id in ownership or not owned_ids,
            'この配置の数式はありますが、所有記録が無いため再生成できません')
    regenerating = score_id in ownership
    for clip_id in preserve_ids:
        require_math(clip_id, '残す')
    for clip_id in replace_ids:
        if clip_id not in by_id:
            require(regenerating, f'置き換える数式clipがありません: {clip_id}')
            continue
        require_math(clip_id, '置き換える')
    recorded = ownership.get(score_id)
    if recorded:
        for clip_id in recorded['clip_ids']:
            clip = by_id.get(clip_id)
            if clip is None:
                continue
            require(clip.get('kind') == 'math' and clip_score_owner(clip['id']) == score_id,
                    f'所有記録とclipが食い違います: {clip_id}')
            owned_ids.add(clip['id'])
    for clip_id in owned_ids:
        require(clip_id not in preserve_ids and clip_id not in replace_ids,
                f'この配置が所有する数式は残す・置き換えるに指定できません: {clip_id}')
    for clip_id in replace_ids:
        require(clip_score_owner(clip_id) is None, f'他の配置が所有する数式は置き換えられません: {clip_id}')
    unclassified = []
    for clip in result['timeline_clips']:
        if clip.get('kind') != 'math':
            continue
        clip_id = clip['id']
        if clip_id in owned_ids or clip_id in replace_ids or clip_id in preserve_ids:
            continue
        owner = clip_score_owner(clip_id)
        if owner is not None and owner != score_id:
            continue
        unclassified.append(clip_id)
    require(not unclassified,
            '所有者が無い数式clipがあります。残すか置き換えるかを指定してください: ' + ', '.join(unclassified))
    removed = set(owned_ids) | replace_ids
    template_id = score.get('template_clip_id')
    if template_id is not None:
        require_math(template_id, 'ひな型')
        template = by_id[template_id]
    else:
        candidates = [clip for clip in result['timeline_clips']
                      if clip.get('kind') == 'math' and
                      clip['id'] in (preserve_ids | replace_ids | owned_ids)]
        require(candidates, '数式clipのひな型が必要です。template_clip_idを指定してください')
        template = candidates[0]
    kept_transitions = []
    for transition in result['timeline_transitions']:
        ends = {transition['incoming_clip_id'], transition['outgoing_clip_id']}
        hit = ends & removed
        if not hit:
            kept_transitions.append(transition)
        else:
            require(hit == ends, '残るclipと削除するclipの両方に掛かるトランジションは自動では外せません')
    result['timeline_transitions'] = kept_transitions
    subtitles_now = result.get('subtitles')
    cues = []
    if subtitles_now:
        require(isinstance(subtitles_now, dict) and isinstance(subtitles_now.get('cues', []), list),
                '字幕データが不正です')
        cues = subtitles_now.get('cues', [])
    bound_cue_ids = {state['subtitle_cue_id'] for state in score['states'] if state.get('subtitle_cue_id')}
    for cue in cues:
        link = cue.get('link_clip_id')
        if link in removed and cue.get('id') not in bound_cue_ids:
            raise ScoreError('削除する数式に結び付いた字幕は、状態へ明示して付け直す必要があります: ' +
                             str(cue.get('id', '')))
    result['timeline_clips'] = [clip for clip in result['timeline_clips'] if clip['id'] not in removed]
    video_count = len(result['video_tracks'])
    for clip in result['timeline_clips']:
        if clip.get('track_kind') == 'video':
            index = clip.get('track_index')
            require(type(index) is int and 0 <= index < video_count, '残したclipの映像trackが範囲外です')
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
            clip_id = owned_clip_id(score_id, part, owner)
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
                overlay['id'] = owned_clip_id(score_id, part, owner, highlight=True)
                overlay['name'] += '（変更箇所の強調）'
                overlay['math']['color'] = score.get('highlight_color', '#FFCC2222')
                overlay['effects']['opacity_percent'], overlay['effects']['opacity_keys'] = curve(
                    highlight_events, 'opacity', begin, duration, lambda opacity: opacity)
                intervals.append((begin, end, overlay, part + '/highlight'))
    # 同時に見えるclipだけを別trackに置く。元のtrackは空でも残し、番号を詰めない。
    # 再生成では、この配置が以前に足したtrackを使い、同じ名前を重ねて足さない。
    recorded_tracks = list(ownership.get(score_id, {}).get('track_names', []))
    reusable = []
    for track_name in recorded_tracks:
        indexes = [index for index, track in enumerate(result['video_tracks'])
                   if track.get('name') == track_name]
        require(len(indexes) == 1, f'この配置のtrackが見つからないか重複しています: {track_name}')
        index = indexes[0]
        require(not any(clip.get('track_kind') == 'video' and clip.get('track_index') == index
                        for clip in result['timeline_clips']),
                f'この配置のtrackに残るclipがあるため再生成できません: {track_name}')
        reusable.append(index)
    created_tracks = []
    lane_tracks, track_ends, allocation = [], [], {}
    for begin, end, c, part in sorted(intervals, key=lambda row: (row[0], row[1], row[3])):
        lane = next((i for i, finish in enumerate(track_ends) if finish <= begin), len(track_ends))
        if lane == len(track_ends):
            track_ends.append(end)
            if lane < len(reusable):
                lane_tracks.append(reusable[lane])
            else:
                track_name = f'部分式 {len(recorded_tracks) + len(created_tracks) + 1}'
                used_names = [track.get('name') for track in result['video_tracks']]
                used_names += [track.get('name') for track in result.get('audio_tracks') or []]
                require(track_name not in used_names, f'追加するtrack名が既存と重複します: {track_name}')
                result['video_tracks'].append(blank_video_track(track_name))
                created_tracks.append(track_name)
                lane_tracks.append(len(result['video_tracks']) - 1)
        else:
            track_ends[lane] = end
        c['track_index'] = lane_tracks[lane]
        result['timeline_clips'].append(c)
        allocation.setdefault(part, []).append(c['id'])
    subtitles = result.get('subtitles') or {}
    cue_by_id = {}
    if subtitles:
        for cue in subtitles.get('cues', []):
            require(isinstance(cue.get('id'), str) and cue['id'] and cue['id'] not in cue_by_id,
                    '字幕IDが空または重複しています')
            cue_by_id[cue['id']] = cue
    bound_cues = set()
    for state, stage in zip(score['states'], stages):
        cue_id = state.get('subtitle_cue_id')
        if not cue_id:
            continue
        require(cue_id in cue_by_id and cue_id not in bound_cues,
                '字幕の対応先が存在しない、または複数の状態が同じ字幕を参照しています')
        bound_cues.add(cue_id)
        cue = cue_by_id[cue_id]
        link = cue.get('link_clip_id')
        if link and link not in removed:
            raise ScoreError('残るclipに結び付いた字幕の時刻は配置から変更できません: ' + cue_id)
        # 状態が字幕を明示して引き取るときだけ、消える数式との結び付きを外す。
        if link in removed:
            cue.pop('link_clip_id')
        cue['start_frame'] = stage['start'] + state.get('subtitle_start_offset_frames', 0)
        cue['end_frame'] = stage['start'] + stage['duration'] - state.get('subtitle_end_padding_frames', 0)
    generated_markers = sorted(
        set(stage['start'] for stage in stages) |
        set(stage['start'] + min(stage['transition_frames'] + 90, stage['duration'] - 1)
            for stage in stages))
    existing_markers = result.get('timeline_markers', [])
    require(isinstance(existing_markers, list) and
            all(type(frame) is int and frame >= 0 for frame in existing_markers) and
            existing_markers == sorted(set(existing_markers)),
            'Projectのマーカーは0以上で、重複せず昇順である必要があります')
    owned_markers = set(ownership.get(score_id, {}).get('marker_frames', []))
    kept_markers = [frame for frame in existing_markers if frame not in owned_markers]
    overlap = sorted(set(generated_markers) & set(kept_markers))
    require(not overlap,
            '残すマーカーと配置の停止点が同じframeです: ' + ', '.join(str(frame) for frame in overlap))
    result['timeline_markers'] = sorted(set(kept_markers) | set(generated_markers))
    ownership[score_id] = dict(clip_ids=sorted(clip['id'] for _, _, clip, _ in intervals),
                               marker_frames=generated_markers,
                               track_names=recorded_tracks + created_tracks)
    result[OWNERSHIP_KEY] = dict(format=OWNERSHIP_FORMAT,
                                 scores={key: ownership[key] for key in sorted(ownership)})
    report = dict(format='mvm-math-score-report/1', frames=total, stages=stages,
                  native_clips=len(intervals), native_tracks=len(track_ends), objects=allocation)
    return result, report


def stage(destination, content, suffix, staged):
    """destination と同じ directory の一時ファイルに書く (replace を同じ volume 内の改名にする)。"""
    with tempfile.NamedTemporaryFile(dir=destination.parent, suffix=suffix, delete=False) as temporary:
        path = Path(temporary.name)
    staged.append(path)
    if isinstance(content, bytes):
        path.write_bytes(content)
    else:
        path.write_text(content, encoding='utf-8')
    return path


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
        # 検証結果も先に一時ファイルへ書き切る。書けないまま Project だけを公開しない。
        staged = []
        try:
            candidate = stage(output_path, json.dumps(result, ensure_ascii=False, indent=2), '.mvm', staged)
            report_candidate = stage(report_path, json.dumps(report, ensure_ascii=False, indent=2), '.json', staged)
            checked = subprocess.run([args.validator, str(candidate)], capture_output=True)
            require(checked.returncode == 0,
                    '製品のProject検証が失敗しました: ' + checked.stderr.decode('utf-8', errors='replace'))
            previous = output_path.read_bytes() if output_path.exists() else None
            candidate.replace(output_path)
            try:
                report_candidate.replace(report_path)
            except OSError:
                # 失敗と報告する実行で Project だけを更新しない。元の内容へ戻す。
                if previous is None:
                    output_path.unlink(missing_ok=True)
                else:
                    stage(output_path, previous, '.mvm', staged).replace(output_path)
                raise
        finally:
            for path in staged:
                path.unlink(missing_ok=True)
    except (ScoreError, KeyError, TypeError, ValueError, OSError) as error:
        parser.exit(1, f'数式配置を変換できません: {error}\n')
    print(f"部分式を変換しました: {report['native_clips']}clip、{report['frames']}frame")


if __name__ == '__main__':
    main()
