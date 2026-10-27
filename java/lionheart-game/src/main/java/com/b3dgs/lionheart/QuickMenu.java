/*
 * Copyright (C) 2013-2026 Byron 3D Games Studio (www.b3dgs.com) Pierre-Alexandre (contact@b3dgs.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
package com.b3dgs.lionheart;

import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.Optional;
import java.util.Queue;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.b3dgs.lionengine.Align;
import com.b3dgs.lionengine.Media;
import com.b3dgs.lionengine.Medias;
import com.b3dgs.lionengine.UtilConversion;
import com.b3dgs.lionengine.geom.Coord;
import com.b3dgs.lionengine.graphic.ColorRgba;
import com.b3dgs.lionengine.graphic.Graphic;
import com.b3dgs.lionengine.graphic.Graphics;
import com.b3dgs.lionengine.graphic.Text;
import com.b3dgs.lionengine.io.FileReading;
import com.b3dgs.lionengine.io.FileWriting;
import com.b3dgs.lionheart.constant.Folder;

/**
 * In-level quick menu (VR port): save into one of several slots at the last checkpoint, or load a slot or the
 * automatic level-start save. Driven by events from the front-end (any thread); the level pauses while it is open.
 */
public final class QuickMenu
{
    /** Open / close the menu. */
    public static final int EVENT_TOGGLE = 0;
    /** Selection up. */
    public static final int EVENT_UP = 1;
    /** Selection down. */
    public static final int EVENT_DOWN = 2;
    /** Switch to the other action (load / save). */
    public static final int EVENT_SWITCH = 3;
    /** Execute. */
    public static final int EVENT_CONFIRM = 4;
    /** Close. */
    public static final int EVENT_BACK = 5;

    /** Save slots (in addition to the automatic save). */
    public static final int SLOTS = 5;

    /**
     * Level access.
     */
    public interface Host
    {
        /**
         * Get the current game configuration.
         * 
         * @return The game configuration.
         */
        GameConfig getGame();

        /**
         * Snapshot of the player at the last checkpoint.
         * 
         * @return The init config, <code>null</code> if saving is not possible here.
         */
        InitConfig snapshot();

        /**
         * Restart from the init config.
         * 
         * @param init The init config.
         */
        void load(InitConfig init);
    }

    private static final Logger LOGGER = LoggerFactory.getLogger(QuickMenu.class);
    private static final Queue<Integer> EVENTS = new ConcurrentLinkedQueue<>();
    private static final Pattern STAGE_NUMBER = Pattern.compile("(\\d+)");
    private static final ColorRgba BACKGROUND = new ColorRgba(0, 0, 0, 235);
    /** Menu font size (game pixels); drawn at high resolution in the VR text layer. */
    private static final int FONT_SIZE = 14;
    private static final ColorRgba FRAME = new ColorRgba(200, 160, 60);
    private static final ColorRgba HIGHLIGHT = new ColorRgba(255, 220, 90);
    private static final ColorRgba DIM = new ColorRgba(150, 150, 150);
    private static volatile boolean open;

    /**
     * Post a menu event (any thread).
     * 
     * @param event The event.
     */
    public static void post(int event)
    {
        EVENTS.add(Integer.valueOf(event));
    }

    /**
     * Check if the menu is open.
     * 
     * @return <code>true</code> if open.
     */
    public static boolean isOpen()
    {
        return open;
    }

    private final String[] labels = new String[SLOTS + 1];
    private final Text font = Graphics.createText(FONT_SIZE);
    private boolean saveMode;
    private int choice;
    private String message = "";
    private int messageFrames;

    /**
     * Create menu.
     */
    public QuickMenu()
    {
        super();

        EVENTS.clear();
        open = false;
    }

    /**
     * Update the menu (game thread).
     * 
     * @param host The level.
     * @param rate The frame rate.
     * @return <code>true</code> while the menu is open (the level must not update).
     */
    public boolean update(Host host, int rate)
    {
        if (messageFrames > 0)
        {
            messageFrames--;
        }
        Integer event;
        while ((event = EVENTS.poll()) != null)
        {
            handle(host, event.intValue(), rate);
        }
        return open;
    }

    private void handle(Host host, int event, int rate)
    {
        if (!open)
        {
            if (event == EVENT_TOGGLE)
            {
                if (!isStory(host.getGame()))
                {
                    showMessage("QUICK MENU: STORY MODE ONLY", rate);
                    return;
                }
                refresh(host.getGame());
                saveMode = false;
                choice = firstUsed();
                open = true;
            }
            return;
        }
        switch (event)
        {
            case EVENT_TOGGLE, EVENT_BACK -> open = false;
            case EVENT_UP -> choice = (choice + SLOTS) % (SLOTS + 1);
            case EVENT_DOWN -> choice = (choice + 1) % (SLOTS + 1);
            case EVENT_SWITCH ->
            {
                saveMode = !saveMode;
                if (saveMode && choice == 0)
                {
                    choice = 1;
                }
            }
            case EVENT_CONFIRM -> confirm(host, rate);
            default -> LOGGER.warn("Unknown quick menu event {}", Integer.valueOf(event));
        }
    }

    private void confirm(Host host, int rate)
    {
        final GameConfig game = host.getGame();
        if (saveMode)
        {
            if (choice == 0)
            {
                return;
            }
            final InitConfig init = host.snapshot();
            if (init != null && write(slot(game, choice), init))
            {
                refresh(game);
                open = false;
                showMessage("SAVED TO SLOT " + choice, rate);
            }
            else
            {
                showMessage("SAVE FAILED", rate);
            }
        }
        else
        {
            final InitConfig init = choice == 0 ? readAuto(game) : read(slot(game, choice));
            if (init != null)
            {
                open = false;
                host.load(init);
            }
        }
    }

    private int firstUsed()
    {
        for (int i = 0; i <= SLOTS; i++)
        {
            if (labels[i] != null)
            {
                return i;
            }
        }
        return 0;
    }

    private void showMessage(String text, int rate)
    {
        message = text;
        messageFrames = rate * 2;
    }

    private void refresh(GameConfig game)
    {
        labels[0] = labelAuto(game);
        for (int i = 1; i <= SLOTS; i++)
        {
            labels[i] = label(slot(game, i));
        }
    }

    /**
     * Render the menu and messages (game thread, HUD layer).
     * 
     * @param g The graphic output.
     * @param text The text renderer.
     * @param width The screen width.
     * @param height The screen height.
     */
    public void render(Graphic g, Text levelText, int width, int height)
    {
        final Text text = font;
        if (messageFrames > 0)
        {
            text.setColor(ColorRgba.WHITE);
            text.draw(g, width / 2, 24, Align.CENTER, message);
        }
        if (!open)
        {
            return;
        }
        final int line = text.getSize() + 4;
        final int w = Math.min(width - 8, 300);
        final int h = line * (SLOTS + 5);
        final int x = (width - w) / 2;
        final int y = (height - h) / 2;

        g.setColor(BACKGROUND);
        g.drawRect(x, y, w, h, true);
        g.setColor(FRAME);
        g.drawRect(x, y, w - 1, h - 1, false);

        int ty = y + 4;
        text.setColor(saveMode ? DIM : HIGHLIGHT);
        text.draw(g, x + w / 4, ty, Align.CENTER, "LOAD");
        text.setColor(saveMode ? HIGHLIGHT : DIM);
        text.draw(g, x + w * 3 / 4, ty, Align.CENTER, "SAVE");
        ty += line + 4;

        for (int i = 0; i <= SLOTS; i++)
        {
            final boolean usable = saveMode ? i > 0 : labels[i] != null;
            final String name = i == 0 ? "AUTO" : "SLOT " + i;
            final String info = labels[i] != null ? labels[i] : "EMPTY";
            text.setColor(i == choice ? HIGHLIGHT : usable ? ColorRgba.WHITE : DIM);
            text.draw(g, x + 8, ty, Align.LEFT, (i == choice ? "> " : "  ") + name);
            text.draw(g, x + w - 8, ty, Align.RIGHT, info);
            ty += line;
        }
        ty += 4;
        text.setColor(DIM);
        text.draw(g, width / 2, ty, Align.CENTER, "STICK: CHOOSE  A: OK  B: CLOSE");
        text.setColor(ColorRgba.WHITE);
    }

    private static boolean isStory(GameConfig game)
    {
        return game.getType().is(GameType.STORY) && game.getStages().isPresent();
    }

    private static Media slot(GameConfig game, int index)
    {
        return Medias.create(Folder.STAGE, Folder.STORY, game.getStages().get(), "quick" + index + ".lhr");
    }

    private static String stageName(InitConfig init)
    {
        final String path = init.getStage().getPath();
        final String file = path.substring(path.lastIndexOf('/') + 1);
        final Matcher matcher = STAGE_NUMBER.matcher(file);
        return matcher.find() ? "STAGE " + matcher.group(1) : file.toUpperCase(Locale.ENGLISH);
    }

    private static String labelAuto(GameConfig game)
    {
        final InitConfig init = readAuto(game);
        return init != null ? stageName(init) : null;
    }

    private static InitConfig readAuto(GameConfig game)
    {
        try
        {
            if (Medias.create(Folder.STAGE, Folder.STORY, game.getStages().get(), Constant.FILE_PROGRESS).exists())
            {
                return Util.loadProgress(game);
            }
        }
        catch (final IOException | RuntimeException exception)
        {
            LOGGER.warn("Auto save unreadable", exception);
        }
        return null;
    }

    private static String label(Media media)
    {
        if (!media.exists())
        {
            return null;
        }
        try (FileReading reading = new FileReading(media))
        {
            final String when = reading.readString();
            final InitConfig init = readInit(reading);
            return stageName(init) + "  " + when;
        }
        catch (final IOException | RuntimeException exception)
        {
            LOGGER.warn("Quick save unreadable: {}", media, exception);
            return null;
        }
    }

    private static InitConfig read(Media media)
    {
        if (!media.exists())
        {
            return null;
        }
        try (FileReading reading = new FileReading(media))
        {
            reading.readString();
            return readInit(reading);
        }
        catch (final IOException | RuntimeException exception)
        {
            LOGGER.error("Quick load failed: {}", media, exception);
            return null;
        }
    }

    private static InitConfig readInit(FileReading reading) throws IOException
    {
        final Media stage = Medias.create(reading.readString());
        final int healthMax = UtilConversion.toUnsignedByte(reading.readByte());
        final int talisment = UtilConversion.toUnsignedByte(reading.readByte());
        final int life = UtilConversion.toUnsignedByte(reading.readByte());
        final int sword = UtilConversion.toUnsignedByte(reading.readByte());
        final boolean amulet = reading.readBoolean();
        final int credits = UtilConversion.toUnsignedByte(reading.readByte());
        final Difficulty difficulty = Difficulty.valueOf(reading.readString());
        final boolean cheats = reading.readBoolean();
        Optional<Coord> spawn = Optional.empty();
        if (reading.readBoolean())
        {
            spawn = Optional.of(new Coord(reading.readInteger(), reading.readInteger()));
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
                              spawn);
    }

    private static boolean write(Media media, InitConfig init)
    {
        try (FileWriting writing = new FileWriting(media))
        {
            writing.writeString(new SimpleDateFormat("dd.MM. HH:mm", Locale.ENGLISH).format(new Date()));
            writing.writeString(init.getStage().getPath());
            writing.writeByte(UtilConversion.fromUnsignedByte(init.getHealthMax()));
            writing.writeByte(UtilConversion.fromUnsignedByte(init.getTalisment()));
            writing.writeByte(UtilConversion.fromUnsignedByte(init.getLife()));
            writing.writeByte(UtilConversion.fromUnsignedByte(init.getSword()));
            writing.writeBoolean(init.isAmulet());
            writing.writeByte(UtilConversion.fromUnsignedByte(init.getCredits()));
            writing.writeString(init.getDifficulty().name());
            writing.writeBoolean(init.isCheats());
            writing.writeBoolean(init.getSpawn().isPresent());
            if (init.getSpawn().isPresent())
            {
                writing.writeInteger((int) init.getSpawn().get().getX());
                writing.writeInteger((int) init.getSpawn().get().getY());
            }
            LOGGER.info("Quick saved {} to {}", init.getStage().getPath(), media);
            return true;
        }
        catch (final IOException | RuntimeException exception)
        {
            LOGGER.error("Quick save failed: {}", media, exception);
            return false;
        }
    }
}
