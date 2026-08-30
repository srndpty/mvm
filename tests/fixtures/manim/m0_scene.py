from manim import Create, Scene, Square


class MvmM0Scene(Scene):
    def construct(self):
        square = Square().set_fill("#4C8BF5", opacity=0.8)
        self.play(Create(square), run_time=0.4)
