"""P3-0 の構造・対応・部分強調を実 toolchain で調べる。製品には組み込まない。"""

import argparse
import hashlib
import json
import logging
import pathlib
import subprocess

import numpy as np
from PIL import Image
import manim
from manim import (AnimationGroup, Circumscribe, FadeIn, FadeOut, Indicate,
                   MathTex, ReplacementTransform, Scene, WHITE, tempconfig, logger)


class FallbackLog(logging.Handler):
    def __init__(self):
        super().__init__(logging.ERROR)
        self.messages = []

    def emit(self, record):
        if "Could not find SVG group" in record.getMessage():
            self.messages.append(record.getMessage())


def validate_parts(tex, expected, fallback):
    if fallback:
        raise ValueError("SVG group が代用されました")
    if len(tex.submobjects) != len(expected):
        raise ValueError("部分数が一致しません")
    for part, text in zip(tex.submobjects, expected):
        if type(part).__name__ != "MathTexPart" or part.tex_string != text:
            raise ValueError("部分の型または文字列が一致しません")
    return [len(part.submobjects) for part in tex.submobjects]


def target_part(tex, index):
    part = tex[index]
    if not len(part.submobjects) or not len(part.get_all_points()):
        raise ValueError("強調対象に glyph がありません")
    return part


def pixels(scene, objects):
    scene.camera.reset()
    scene.camera.capture_mobjects(objects)
    return np.array(scene.camera.pixel_array)


def snapshot(scene, objects, path):
    value = pixels(scene, objects)
    Image.fromarray(value).save(path)
    return {"file": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "nonzero_alpha": int(np.count_nonzero(value[:, :, 3]))}


def main():
    parser = argparse.ArgumentParser(description="P3-0 の独立 renderer 検証")
    parser.add_argument("output", type=pathlib.Path)
    opts = parser.parse_args()
    out = opts.output.resolve()
    if out.exists():
        parser.error("出力先が既にあります。新しい directory を指定してください")
    out.mkdir(parents=True)
    report = {"schema": "mvm-math-p30-spike/1", "manim": manim.__version__,
              "toolchain": {}, "structures": [], "negative": [], "transitions": [],
              "emphasis": [], "failures": []}
    for tool in ("latex", "dvisvgm"):
        report["toolchain"][tool] = subprocess.check_output(
            [tool, "--version"], text=True, encoding="utf-8").splitlines()[0]
    if manim.__version__ != "0.21.0":
        raise RuntimeError("Manim 0.21.0 が必要です")
    capture = FallbackLog()
    logger.addHandler(capture)
    states = [
        ["ax^2", "+", "bx", "+", "c", "=", "0"],
        ["x^2", "+", r"\frac{b}{a}x", "=", r"-\frac{c}{a}"],
        ["x^2", "+", r"\frac{b}{a}x", "+", r"\left(\frac{b}{2a}\right)^2",
         "=", r"-\frac{c}{a}", "+", r"\left(\frac{b}{2a}\right)^2"],
        [r"\left(x+\frac{b}{2a}\right)^2", "=", r"\frac{", "b^2-4ac", "}{4a^2}"],
        [r"x+\frac{b}{2a}", "=", r"\pm\frac{\sqrt{", "b^2-4ac", "}}{2a}"],
        ["x", "=", r"\frac{-b\pm\sqrt{", "b^2-4ac", "}}{2a}"],
    ]
    cases = [(f"S{i}", parts) for i, parts in enumerate(states)] + [
        ("duplicates", ["x", "+", "x", "=", "2x"]),
        ("left-right", [r"\left(", "x", "+", r"\frac{b}{2a}", r"\right)^2"]),
        ("no-glyph", [r"\frac{", "b", "}{a}"]),
    ]
    with tempconfig({"tex_dir": str(out / "Tex"), "media_dir": str(out / "media"),
                     "pixel_width": 1280, "pixel_height": 360, "frame_width": 24,
                     "frame_height": 6.75, "background_opacity": 0,
                     "disable_caching": True}):
        scene = Scene()
        for name, segments in cases:
            for size in (64, 96):
                reference = MathTex("".join(segments), font_size=size * 12 / 17).set_color(WHITE)
                for strategy in ("args", "braces", "isolate"):
                    capture.messages.clear()
                    try:
                        if strategy == "args":
                            tex = MathTex(*segments, font_size=size * 12 / 17)
                        elif strategy == "braces":
                            tex = MathTex(" ".join("{{ " + p + " }}" for p in segments),
                                          font_size=size * 12 / 17)
                        else:
                            tex = MathTex("".join(segments), substrings_to_isolate=segments,
                                          font_size=size * 12 / 17)
                        tex.set_color(WHITE)
                        actual = [getattr(p, "tex_string", None) for p in tex.submobjects]
                        row = {"case": name, "font_size": size, "strategy": strategy,
                               "parts": actual, "glyphs": [len(p.submobjects) for p in tex],
                               "fallback": list(capture.messages),
                               "alpha_difference": int(np.count_nonzero(
                                   pixels(scene, [reference])[:, :, 3] != pixels(scene, [tex])[:, :, 3]))}
                        if strategy == "args":
                            validate_parts(tex, segments, capture.messages)
                            if row["alpha_difference"]:
                                raise ValueError("静止参照と画素が一致しません")
                        report["structures"].append(row)
                    except Exception as exc:
                        report["structures"].append({"case": name, "font_size": size,
                            "strategy": strategy, "error": str(exc), "fallback": list(capture.messages)})
                        if strategy == "args":
                            report["failures"].append(name + ": " + str(exc))
        # mvm が所有する対応表。重複項は文字列検索でなく番号を明示する。
        pairs = [[[5, 3]], [[0, 0], [1, 1], [2, 2], [3, 5], [4, 6]],
                 [[5, 1]], [[1, 1], [3, 3]], [[1, 1], [3, 3]]]
        transition_cases = [(f"S{i}-S{i+1}", states[i], states[i+1], pairs[i]) for i in range(5)]
        transition_cases.append(("duplicate-explicit", ["x", "+", "x", "=", "2x"],
                                 ["x", "+", "x", "+", "x", "=", "3x"],
                                 [[0, 2], [1, 1], [2, 0], [3, 5]]))
        for name, source, target, mapping in transition_cases:
            capture.messages.clear()
            a = MathTex(*source, font_size=96 * 12 / 17).set_color(WHITE)
            b = MathTex(*target, font_size=64 * 12 / 17).set_color(WHITE)
            validate_parts(a, source, capture.messages)
            validate_parts(b, target, capture.messages)
            source_alpha = pixels(scene, [a])[:, :, 3].copy()
            target_alpha = pixels(scene, [b])[:, :, 3].copy()
            used_a, used_b = {i for i, j in mapping}, {j for i, j in mapping}
            animations = [ReplacementTransform(a[i], b[j]) for i, j in mapping]
            animations += [FadeOut(a[i]) for i in range(len(a)) if i not in used_a]
            animations += [FadeIn(b[j]) for j in range(len(b)) if j not in used_b]
            group = AnimationGroup(*animations)
            group.begin()
            samples = []
            objects = [anim.mobject for anim in animations]
            for frame in range(13):
                group.interpolate(frame / 12)
                samples.append(snapshot(scene, objects, out / f"{name}-{frame:02d}.png"))
            first = np.array(Image.open(out / f"{name}-00.png"))[:, :, 3]
            last = np.array(Image.open(out / f"{name}-12.png"))[:, :, 3]
            differences = [int(np.count_nonzero(first != source_alpha)),
                           int(np.count_nonzero(last != target_alpha))]
            if any(differences):
                report["failures"].append("変形端点が一致しません: " + name)
            group.finish()
            report["transitions"].append({"case": name, "pairs": mapping, "frames": samples,
                                          "endpoint_alpha_difference": differences,
                                          "source_font_size": 96, "target_font_size": 64})
        for size in (64, 96):
            for kind in ("outline", "pulse"):
                tex = MathTex(*states[5], font_size=size * 12 / 17).set_color(WHITE)
                part = target_part(tex, 3)
                untouched = [p.get_all_points().copy() for i, p in enumerate(tex) if i != 3]
                animation = Circumscribe(part) if kind == "outline" else Indicate(part)
                animation._setup_scene(scene)
                animation.begin()
                samples = []
                for frame in range(13):
                    animation.interpolate(frame / 12)
                    active = animation.active_animation if kind == "outline" else animation
                    objects = [tex] if active is None else [tex, active.mobject]
                    samples.append(snapshot(scene, objects,
                                            out / f"{kind}-{size}-{frame:02d}.png"))
                animation.finish()
                changed = len({sample["sha256"] for sample in samples}) > 1
                unchanged = all(np.array_equal(before, p.get_all_points())
                                for before, p in zip(untouched, [p for i, p in enumerate(tex) if i != 3]))
                report["emphasis"].append({"kind": kind, "font_size": size, "part_index": 3,
                    "text": part.tex_string, "glyphs": len(part.submobjects),
                    "other_parts_unchanged": unchanged, "pixels_changed": changed, "frames": samples})
                if not unchanged or not changed:
                    report["failures"].append("強調の対象外不変または画素の変化を確認できません")
        tex = MathTex(*states[5])
        tests = [("wrong-count", lambda: validate_parts(tex, states[5][:-1], [])),
                 ("wrong-text", lambda: validate_parts(tex, ["y"] + states[5][1:], [])),
                 ("fallback", lambda: validate_parts(tex, states[5], ["代用の負の対照"])),
                 ("empty-target", lambda: target_part(MathTex(r"\frac{", "b", "}{a}"), 0))]
        for name, test in tests:
            try:
                test()
                report["negative"].append({"case": name, "rejected": False})
                report["failures"].append("負例を拒否しません: " + name)
            except ValueError as exc:
                report["negative"].append({"case": name, "rejected": True, "message": str(exc)})
    (out / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    lines = ["# P3-0 renderer 検証の自動集計", "", "生データ: `results.json`", "",
             f"- Manim: {report['manim']}",
             f"- 構造比較: {len(report['structures'])} 件",
             f"- 明示対応の変形: {len(report['transitions'])} 件",
             f"- 部分強調: {len(report['emphasis'])} 件",
             f"- 負例の拒否: {sum(r['rejected'] for r in report['negative'])} / {len(report['negative'])}",
             f"- 必須検査の失敗: {len(report['failures'])}", "",
             "|方式|比較件数|描画例外|SVG 代用あり|alpha 全画素一致|", "|---|---:|---:|---:|---:|"]
    for strategy in ("args", "braces", "isolate"):
        rows = [r for r in report["structures"] if r["strategy"] == strategy]
        lines.append(f"|{strategy}|{len(rows)}|{sum('error' in r for r in rows)}|"
                     f"{sum(bool(r.get('fallback')) for r in rows)}|"
                     f"{sum(r.get('alpha_difference') == 0 for r in rows)}|")
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("検証結果: " + str(out / "summary.md"))
    return 1 if report["failures"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
