# GIMP 3.0 batch script (python-fu-eval), run in the directory g3/:
#   gimp-console-3.0 -i --batch-interpreter python-fu-eval -b 'exec(open("gimp3.py").read())' --quit
# Opens every .ora/.xcf there and exports GIMP's flattened rendering as PNG
# (interoperability checks), then saves one GIMP 3 XCF document.
import os
from gi.repository import Gimp, Gio

def load(path):
    return Gimp.file_load(Gimp.RunMode.NONINTERACTIVE, Gio.File.new_for_path(path))

def export_flat(image, path):
    dup = image.duplicate()
    dup.merge_visible_layers(Gimp.MergeType.CLIP_TO_IMAGE)
    if dup.get_precision() != Gimp.Precision.U8_NON_LINEAR:
        dup.convert_precision(Gimp.Precision.U8_NON_LINEAR)
    Gimp.file_save(Gimp.RunMode.NONINTERACTIVE, dup, Gio.File.new_for_path(path), None)
    dup.delete()

for name in os.listdir('.'):
    if name.endswith('.ora') or name.endswith('.xcf'):
        try:
            img = load(name)
            export_flat(img, name + '.gimp.png')
            print('loaded', name)
        except Exception as e:
            print('FAILED', name, e)

# A GIMP 3 document: background + 50 % layer + offset layer, saved as XCF.
img = load('bg.png')
top = Gimp.Layer.new(img, 'box', 20, 12, Gimp.ImageType.RGBA_IMAGE, 50.0, Gimp.LayerMode.NORMAL)
img.insert_layer(top, None, 0)
top.set_offsets(10, 9)
top.fill(Gimp.FillType.WHITE)
Gimp.file_save(Gimp.RunMode.NONINTERACTIVE, img, Gio.File.new_for_path('gimp3.xcf'), None)
export_flat(img, 'gimp3.png')
print('saved gimp3.xcf')
