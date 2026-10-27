#!/usr/bin/env python3
"""Replace the direct checkpoint save (apply_save_hooks.py) with the in-level quick menu (idempotent, CRLF-safe).

Run after apply_save_hooks.py: python3 java/lionheart-quest/tools/apply_quickmenu_hooks.py
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
    if t.count(old) != 1:
        sys.exit(f'ANCHOR FOUND {t.count(old)}x in {rel}:\n{old}')
    t = t.replace(old, new, 1)
    if crlf:
        t = t.replace('\n', '\r\n')
    p.write_bytes(t.encode('utf-8'))
    print(f'edit  {rel}')


edit('World.java',
     '''    /** Save confirmation remaining frames (VR port). */
    private int savedMessage;
    /** Save confirmation text. */
    private String savedText = "";
''',
     '''    /** In-level quick save / load menu (VR port). */
    private final QuickMenu quickMenu = new QuickMenu();
    /** Quick menu access to this level. */
    private final QuickMenu.Host quickHost = new QuickMenu.Host()
    {
        @Override
        public GameConfig getGame()
        {
            return game;
        }

        @Override
        public InitConfig snapshot()
        {
            return checkpointSnapshot();
        }

        @Override
        public void load(InitConfig init)
        {
            sequencer.end(SceneBlack.class, game.with(init));
        }
    };
''')

edit('World.java',
     '''        if (SaveRequest.consume())
        {
            saveAtCheckpoint();
        }
        if (savedMessage > 0)
        {
            savedMessage--;
        }
        updateSpawn(extrp);''',
     '''        if (quickMenu.update(quickHost, source.getRate()))
        {
            return;
        }
        updateSpawn(extrp);''')

edit('World.java',
     '''    private void saveAtCheckpoint()
    {
        if (!game.getType().is(GameType.STORY) || game.getStages().isEmpty() || player == null)
        {
            savedText = "SAVE: STORY MODE ONLY";
            savedMessage = source.getRate() * 2;
            return;
        }
        final Coord check = checkpoints.getCurrent(player.getFeature(Transformable.class));
        final Coord tile = new Coord(Math.floor(check.getX() / map.getTileWidth()),
                                     Math.floor(check.getY() / map.getTileHeight()));
        Util.saveProgress(game.with(Util.getInitConfig(services.get(Media.class),
                                                       player,
                                                       difficulty,
                                                       cheats.isEnabled(),
                                                       Optional.of(tile))));
        LOGGER.info("Saved at checkpoint tile {} {}", tile.getX(), tile.getY());
        savedText = "GAME SAVED";
        savedMessage = source.getRate() * 2;
    }''',
     '''    private InitConfig checkpointSnapshot()
    {
        if (!game.getType().is(GameType.STORY) || game.getStages().isEmpty() || player == null)
        {
            return null;
        }
        final Coord check = checkpoints.getCurrent(player.getFeature(Transformable.class));
        final Coord tile = new Coord(Math.floor(check.getX() / map.getTileWidth()),
                                     Math.floor(check.getY() / map.getTileHeight()));
        LOGGER.info("Checkpoint snapshot at tile {} {}", tile.getX(), tile.getY());
        return Util.getInitConfig(services.get(Media.class),
                                  player,
                                  difficulty,
                                  cheats.isEnabled(),
                                  Optional.of(tile));
    }''')

edit('World.java',
     '''            if (savedMessage > 0)
            {
                text.draw(g, source.getWidth() / 2, 24, Align.CENTER, savedText);
            }''',
     '''            quickMenu.render(g, text, source.getWidth(), source.getHeight());''')
