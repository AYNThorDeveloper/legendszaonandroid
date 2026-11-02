#!/usr/bin/env python3
"""Package the game's own Switch icon (Artwork/icon-switch.jpg, from the control NCA) as Android launcher icons.

The icon is square art with the logo near the top. Launchers mask adaptive icons to a circle or squircle, so the
whole icon sits inside the 72 dp visible square of the 108 dp canvas, on a background in the colour of its border."""
from pathlib import Path
from PIL import Image
root=Path(__file__).resolve().parents[1]
source=root.parent/'Artwork/icon-switch.jpg'
res=root/'android/game_activity/build/res'
art=Image.open(source).convert('RGBA')

def border_colour(image):
    w,h=image.size
    pixels=[image.getpixel((x,y)) for x in range(w) for y in (0,h-1)]+[image.getpixel((x,y)) for y in range(h) for x in (0,w-1)]
    return '#%02x%02x%02x'%tuple(sum(p[i] for p in pixels)//len(pixels) for i in range(3))

for density,factor in {'mdpi':1,'hdpi':1.5,'xhdpi':2,'xxhdpi':3,'xxxhdpi':4}.items():
    folder=res/f'mipmap-{density}'
    folder.mkdir(parents=True,exist_ok=True)
    for stale in ('ic_launcher_logo.png',):
        (folder/stale).unlink(missing_ok=True)
    legacy=round(48*factor)
    art.resize((legacy,legacy),Image.LANCZOS).save(folder/'ic_launcher.png')
    canvas,inner=round(108*factor),round(72*factor)
    foreground=Image.new('RGBA',(canvas,canvas),(0,0,0,0))
    foreground.alpha_composite(art.resize((inner,inner),Image.LANCZOS),((canvas-inner)//2,(canvas-inner)//2))
    foreground.save(folder/'ic_launcher_foreground.png')
values=res/'values'
values.mkdir(parents=True,exist_ok=True)
(values/'ic_launcher_colors.xml').write_text('''<?xml version="1.0" encoding="utf-8"?>
<resources><color name="ic_launcher_background">%s</color></resources>
''' % border_colour(art))
folder=res/'mipmap-anydpi-v26'
folder.mkdir(parents=True,exist_ok=True)
(folder/'ic_launcher.xml').write_text('''<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@color/ic_launcher_background" />
    <foreground android:drawable="@mipmap/ic_launcher_foreground" />
</adaptive-icon>
''')
(res/'drawable'/'ic_launcher_foreground.xml').unlink(missing_ok=True)
print('Game icon resources prepared.')
