# Regenerates the GIF fixtures in this directory (see SOURCES.md).
# Run from this directory: python3 make_fixtures.py
# Needs Pillow and ImageMagick 7 (`magick`). Content is synthetic.
import subprocess

from PIL import Image


def ramp(position, count):
    return position * 255 // (count - 1) if count > 1 else 0


def run(*args):
    subprocess.run(list(args), check=True, stdout=subprocess.DEVNULL)


W, H = 32, 24

# Palette picture with 200 exact colours (Pillow, 89a).
pal = Image.new("P", (W, H))
pal.putpalette([v for i in range(256) for v in ((i * 7) % 256, (i * 13) % 256, (i * 29) % 256)])
pal.putdata([(x * 5 + y * 3) % 200 for y in range(H) for x in range(W)])
pal.save("palette.gif")

# The same picture, interlaced and as GIF87a (ImageMagick).
run("magick", "palette.gif", "-strip", "-interlace", "GIF", "interlaced.gif")
run("magick", "palette.gif", "-strip", "GIF87:palette87a.gif")

# Transparency: index 3 is transparent.
tr = Image.new("P", (W, H))
tr.putpalette([0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255] + [0] * 756)
tr.putdata([(x // 8) % 4 for y in range(H) for x in range(W)])
tr.save("transparent.gif", transparency=3)

# A smooth gradient (more than 256 colours) dithered by ImageMagick.
img = Image.new("RGB", (W, H))
img.putdata([(ramp(x, W), ramp(y, H), 255 - ramp(x + y, W + H - 1)) for y in range(H) for x in range(W)])
img.save("gradient_tmp.ppm")
run("magick", "gradient_tmp.ppm", "-strip", "gradient.gif")
run("rm", "gradient_tmp.ppm")

# Animation with partial frames and every disposal method (ImageMagick).
run("magick", "-dispose", "none", "-delay", "10", "-size", "32x24", "xc:red",
    "-dispose", "background", "-delay", "20", "(", "-size", "8x8", "xc:blue", "-repage", "32x24+8+4", ")",
    "-dispose", "previous", "-delay", "30", "(", "-size", "8x8", "xc:lime", "-repage", "32x24+16+8", ")",
    "-dispose", "none", "-delay", "40", "(", "-size", "4x4", "xc:white", "-repage", "32x24+0+0", ")",
    "-loop", "3", "-strip", "anim_disposal.gif")

# Pillow animation (it stores frame differences with transparency itself).
frames = []
for n in range(4):
    f = Image.new("RGB", (W, H), (20, 40, 60))
    for y in range(8, 16):
        for x in range(4 * n, 4 * n + 8):
            f.putpixel((x, y), (250, 200, 0))
    frames.append(f)
frames[0].save("anim_pillow.gif", save_all=True, append_images=frames[1:], duration=50, loop=0)
