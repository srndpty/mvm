"""P3-4 の animated pulse の同値の参照。製品には組み込まない。

実 Manim の 1 つの scene (1 回の capture) で、状態の色の式の対象に Indicate (強調色、線形、
重みをそのまま alpha に使う) を掛けた frame を描き、Cairo の画素 (premultiplied RGBA) を
そのまま書く。canvas・配置・font・segment・重みは backend が書いた request.json と同じ値を使う
(Python は時間を計算しない)。

    python math-p34-pulse-reference.py <request.json> <出力 directory> <状態の色 #AARRGGBB> <強調色 #AARRGGBB>

variant
  scene    式全体を 1 回で描く (対象は式の中の元の順で描かれる)。実際の Manim の scene と同じ
  ordered  対象以外を描いてから対象を最後に描く (mvm の base の上に accent と同じ重なりの順)
出力: a<action>-<variant>-<frame>.rgba (height x width x 4 byte) と manifest.json。
"""

import json
import pathlib
import sys

from manim import ORIGIN, RIGHT, UP, Indicate, ManimColor, MathTex, config, linear
from manim.camera.camera import Camera

PX_PER_UNIT = 1080.0 / 8.0


def color_of(argb):
    if len(argb) != 9 or argb[0] != "#":
        raise ValueError("色は #AARRGGBB")
    alpha = int(argb[1:3], 16) / 255.0
    return ManimColor.from_hex("#" + argb[3:]), alpha


def camera_for(width, height):
    config.pixel_width = width
    config.pixel_height = height
    config.frame_width = width / PX_PER_UNIT
    config.frame_height = height / PX_PER_UNIT
    return Camera(background_opacity=0)


def placed(state, shift, color, opacity):
    tex = MathTex(*state["segments"], font_size=state["manim_font_size"])
    tex.set_color(color)
    tex.set_opacity(opacity)
    tex.move_to(ORIGIN)
    tex.shift(RIGHT * (shift[0] / PX_PER_UNIT) + UP * (shift[1] / PX_PER_UNIT))
    return tex


def main():
    request = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
    out = pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=False)
    state_color, state_opacity = color_of(sys.argv[3])
    accent_color, _ = color_of(sys.argv[4])
    manifest = {"actions": []}
    for number, item in enumerate(request["actions"]):
        if item["operation"] != "pulse":
            continue
        state = request["states"][item["state"]]
        width, height = item["canvas_px"]
        record = {"action": number, "width": width, "height": height,
                  "segment": item["segment"], "alpha": item["alpha"], "files": {}}
        for variant in ("scene", "ordered"):
            files = []
            for frame, (numerator, denominator) in enumerate(item["alpha"]):
                camera = camera_for(width, height)
                tex = placed(state, item["shift_px"], state_color, state_opacity)
                target = tex[item["segment"]]
                animation = Indicate(target, scale_factor=1.2, color=accent_color,
                                     rate_func=linear)
                animation.begin()
                animation.interpolate(numerator / denominator)
                if variant == "scene":
                    shown = [tex]
                else:
                    shown = [part for index, part in enumerate(tex.submobjects)
                             if index != item["segment"]] + [target]
                camera.reset()
                camera.capture_mobjects(shown)
                name = f"a{number}-{variant}-{frame:05d}.rgba"
                (out / name).write_bytes(camera.pixel_array.tobytes())
                files.append(name)
            record["files"][variant] = files
        manifest["actions"].append(record)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
