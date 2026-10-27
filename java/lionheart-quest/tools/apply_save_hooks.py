#!/usr/bin/env python3
"""Apply the save hooks to the Lionheart game sources (idempotent, CRLF-safe).

- Util.saveProgress also writes on Android (resources inside the APK, the media layer writes to app storage)
  and stores the spawn tile; loadProgress reads it back (old files without it still load).
- World saves at the last checkpoint on SaveRequest (VR menu button) and shows "GAME SAVED".

Run from anywhere: python3 java/lionheart-quest/tools/apply_save_hooks.py
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


# ---- Util.saveProgress: write through the media layer even without a resources folder (Android).
edit('Util.java',
     '''        if (config.getStages().isPresent() && new File(Medias.getResourcesDirectory()).isDirectory())
        {
            final Media media = Medias.create(Folder.STAGE,
                                              Folder.STORY,
                                              config.getStages().get(),
                                              Constant.FILE_PROGRESS);
            try
            {
                final File file = new File(Medias.getResourcesDirectory(), media.getPath());
                if (!file.getParentFile().isDirectory())
                {
                    file.getParentFile().mkdirs();
                }
                if (!file.isFile() && !file.createNewFile())
                {
                    LOGGER.warn("Unable to create progress: {}", file);
                }
''',
     '''        if (config.getStages().isPresent())
        {
            final Media media = Medias.create(Folder.STAGE,
                                              Folder.STORY,
                                              config.getStages().get(),
                                              Constant.FILE_PROGRESS);
            try
            {
                // Android/VR port: no resources folder (assets in the APK), the media layer writes to app storage.
                if (new File(Medias.getResourcesDirectory()).isDirectory())
                {
                    final File file = new File(Medias.getResourcesDirectory(), media.getPath());
                    if (!file.getParentFile().isDirectory())
                    {
                        file.getParentFile().mkdirs();
                    }
                    if (!file.isFile() && !file.createNewFile())
                    {
                        LOGGER.warn("Unable to create progress: {}", file);
                    }
                }
''')

edit('Util.java',
     '''                    writing.writeString(init.getDifficulty().name());
                    writing.writeBoolean(init.isCheats());
                }''',
     '''                    writing.writeString(init.getDifficulty().name());
                    writing.writeBoolean(init.isCheats());
                    // Spawn tile (checkpoint save), optional for older files.
                    writing.writeBoolean(init.getSpawn().isPresent());
                    if (init.getSpawn().isPresent())
                    {
                        writing.writeInteger((int) init.getSpawn().get().getX());
                        writing.writeInteger((int) init.getSpawn().get().getY());
                    }
                }''')

edit('Util.java',
     '''                return new InitConfig(Medias.create(reading.readString()),
                                      UtilConversion.toUnsignedByte(reading.readByte()),
                                      UtilConversion.toUnsignedByte(reading.readByte()),
                                      UtilConversion.toUnsignedByte(reading.readByte()),
                                      UtilConversion.toUnsignedByte(reading.readByte()),
                                      reading.readBoolean(),
                                      UtilConversion.toUnsignedByte(reading.readByte()),
                                      Difficulty.valueOf(reading.readString()),
                                      reading.readBoolean(),
                                      Optional.empty());''',
     '''                final Media stage = Medias.create(reading.readString());
                final int healthMax = UtilConversion.toUnsignedByte(reading.readByte());
                final int talisment = UtilConversion.toUnsignedByte(reading.readByte());
                final int life = UtilConversion.toUnsignedByte(reading.readByte());
                final int sword = UtilConversion.toUnsignedByte(reading.readByte());
                final boolean amulet = reading.readBoolean();
                final int credits = UtilConversion.toUnsignedByte(reading.readByte());
                final Difficulty difficulty = Difficulty.valueOf(reading.readString());
                final boolean cheats = reading.readBoolean();
                Optional<Coord> spawn = Optional.empty();
                try
                {
                    if (reading.readBoolean())
                    {
                        spawn = Optional.of(new Coord(reading.readInteger(), reading.readInteger()));
                    }
                }
                catch (@SuppressWarnings("unused") final IOException exception)
                {
                    // Older progress file without spawn
                }
                return new InitConfig(stage,
                                      healthMax,
                                      talisment,
                                      life,
                                      sword,
                                      amulet,
                                      credits,
                                      difficulty,
                                      cheats,
                                      spawn);''')

# ---- World: save at the last checkpoint on request, show a confirmation.
edit('World.java',
     '''    private Difficulty difficulty;
''',
     '''    private Difficulty difficulty;
    /** Save confirmation remaining frames (VR port). */
    private int savedMessage;
    /** Save confirmation text. */
    private String savedText = "";
''')

edit('World.java',
     '''    public void update(double extrp)
    {
        updateSpawn(extrp);
''',
     '''    public void update(double extrp)
    {
        if (SaveRequest.consume())
        {
            saveAtCheckpoint();
        }
        if (savedMessage > 0)
        {
            savedMessage--;
        }
        updateSpawn(extrp);
''')

edit('World.java',
     '''    @Override
    public void loadNextStage(String next, int delayMs, Optional<Coord> spawn)
    {''',
     '''    /**
     * Save the progress at the last reached checkpoint (story mode).
     */
    private void saveAtCheckpoint()
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
    }

    @Override
    public void loadNextStage(String next, int delayMs, Optional<Coord> spawn)
    {''')

edit('World.java',
     '''            cheats.render(g);
            StereoLayers.end(g);''',
     '''            cheats.render(g);
            if (savedMessage > 0)
            {
                text.draw(g, source.getWidth() / 2, 24, Align.CENTER, savedText);
            }
            StereoLayers.end(g);''')
