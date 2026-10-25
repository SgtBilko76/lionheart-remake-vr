#!/usr/bin/env python3
"""Apply the stereo-layer hooks to the Lionheart game sources (idempotent, CRLF-safe).

Run from anywhere: python3 java/lionheart-quest/tools/apply_stereo_hooks.py
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3] / 'java/lionheart-game/src/main/java/com/b3dgs/lionheart'


def edit(rel, old, new):
    p = ROOT / rel
    raw = p.read_bytes().decode('utf-8')
    crlf = '\r\n' in raw
    t = raw.replace('\r\n', '\n')
    if new in t and (old not in t or new not in old):
        print(f'skip  {rel}')
        return
    if old not in t:
        sys.exit(f'ANCHOR NOT FOUND in {rel}:\n{old}')
    t = t.replace(old, new, 1)
    if crlf:
        t = t.replace('\n', '\r\n')
    p.write_bytes(t.encode('utf-8'))
    print(f'edit  {rel}')


for name in ['Airship', 'Black', 'Dragonfly', 'Gradient', 'Lava', 'Norka', 'Swamp']:
    edit(f'landscape/{name}.java', ' extends BackgroundAbstract\n', ' extends BackgroundStereo\n')

edit('landscape/Landscape.java',
     '''        background.render(g);
        foreground.renderBack(g);''',
     '''        background.render(g);
        StereoLayers.layer(g, StereoLayers.PLAYFIELD, StereoLayers.KIND_PLANE);
        foreground.renderBack(g);''')
edit('landscape/Landscape.java',
     'import com.b3dgs.lionengine.graphic.Graphic;\n',
     'import com.b3dgs.lionengine.graphic.Graphic;\nimport com.b3dgs.lionheart.StereoLayers;\n')

edit('World.java',
     '''            rasterRenderer.execute();
            landscape.renderForeground(g);
            hud.render(g);
''',
     '''            rasterRenderer.execute();
            StereoLayers.layer(g, StereoLayers.FOREGROUND, StereoLayers.KIND_PLANE);
            landscape.renderForeground(g);
            StereoLayers.layer(g, StereoLayers.HUD, StereoLayers.KIND_PLANE);
            hud.render(g);
''')
edit('World.java',
     '''            cheats.render(g);
        }
    }
''',
     '''            cheats.render(g);
            StereoLayers.end(g);
        }
    }
''')
