# SPDX-License-Identifier: MIT
"""Canvas.polyline: the line a history graph is drawn with."""
import tiny_paper_display as tpd


def dark(c):
    return {(i % c.w, i // c.w) for i, v in enumerate(c.px) if v == 0}


def test_horizontal_and_diagonal_line_touch_every_column():
    c = tpd.Canvas(20, 10)
    c.polyline([(0, 2), (19, 2)], 0)
    assert dark(c) == {(x, 2) for x in range(20)}
    c = tpd.Canvas(20, 10)
    c.polyline([(0, 0), (9, 9)], 0)
    assert dark(c) == {(i, i) for i in range(10)}


def test_thickness_and_corner_joins():
    c = tpd.Canvas(20, 20)
    c.polyline([(2, 2), (10, 2), (10, 10)], 0, 3)
    d = dark(c)
    assert (10, 10) in d and (2, 2) in d and (10, 6) in d
    assert all(0 <= x < 20 and 0 <= y < 20 for x, y in d)


def test_points_outside_are_clipped_and_one_point_is_a_dot():
    c = tpd.Canvas(8, 8)
    c.polyline([(-50, 4), (60, 4)], 0)
    assert dark(c) == {(x, 4) for x in range(8)}
    c = tpd.Canvas(8, 8)
    c.polyline([(3, 3)], 0)
    assert dark(c) == {(3, 3)}
    c.polyline([], 0)  # nothing, no error
