#!/usr/bin/env python3
"""Draw a few original CC0 pixel icons into ../assets/pool_src/.

These simple geometric shapes are this repo own work (public domain), so they
ship committed and give a fresh clone a non-empty, legally clean icon pool even
without the gitignored third-party sprites in ../assets/pool_local/. make_pool.py
scans both folders, so on a real device these mix in with whatever local art
the owner dropped.
"""
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets", "pool_src")
S = 24   # native grid; make_pool.py upscales with nearest for a crisp look


def canvas():
    im = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    return im, ImageDraw.Draw(im)


def disc():
    im, d = canvas()
    d.ellipse([3, 3, S - 4, S - 4], outline=(0, 0, 0, 255), width=2)
    return im


def dot():
    im, d = canvas()
    d.ellipse([5, 5, S - 6, S - 6], fill=(0, 0, 0, 255))
    return im


def square():
    im, d = canvas()
    d.rectangle([4, 4, S - 5, S - 5], outline=(0, 0, 0, 255), width=2)
    return im


def diamond():
    im, d = canvas()
    c = S // 2
    d.polygon([(c, 3), (S - 4, c), (c, S - 4), (3, c)], outline=(0, 0, 0, 255), width=2)
    return im


def cross():
    im, d = canvas()
    d.rectangle([S // 2 - 2, 4, S // 2 + 1, S - 5], fill=(0, 0, 0, 255))
    d.rectangle([4, S // 2 - 2, S - 5, S // 2 + 1], fill=(0, 0, 0, 255))
    return im


def triangle():
    im, d = canvas()
    d.polygon([(S // 2, 3), (S - 4, S - 4), (3, S - 4)], outline=(0, 0, 0, 255), width=2)
    return im


def heart():
    im, d = canvas()
    d.polygon([(12, 20), (4, 11), (7, 6), (12, 9), (17, 6), (20, 11)],
              fill=(0, 0, 0, 255))
    return im


def star():
    im, d = canvas()
    pts = [(12, 3), (14, 9), (21, 9), (15, 13), (18, 20),
           (12, 16), (6, 20), (9, 13), (3, 9), (10, 9)]
    d.polygon(pts, fill=(0, 0, 0, 255))
    return im


def main():
    os.makedirs(OUT, exist_ok=True)
    shapes = {
        "base_disc": disc, "base_dot": dot, "base_square": square,
        "base_diamond": diamond, "base_cross": cross, "base_triangle": triangle,
        "base_heart": heart, "base_star": star,
    }
    for name, fn in shapes.items():
        fn().save(os.path.join(OUT, name + ".png"))
    print("wrote %d baseline icons to %s" % (len(shapes), os.path.normpath(OUT)))


if __name__ == "__main__":
    main()
