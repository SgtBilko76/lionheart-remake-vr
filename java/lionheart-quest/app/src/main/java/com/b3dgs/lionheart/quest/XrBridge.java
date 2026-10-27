/*
 * Copyright (C) 2026 - GPL-3.0-or-later (same licence as Lionheart Remake).
 */
package com.b3dgs.lionheart.quest;

import android.graphics.SurfaceTexture;
import android.view.KeyEvent;

import com.b3dgs.lionengine.android.Gamepad;
import com.b3dgs.lionheart.QuickMenu;

/**
 * Bridge to the native OpenXR host (liblionheartxr.so).
 * Touch controllers are mirrored 1:1 onto the Android gamepad the game already supports (input.xml).
 */
public final class XrBridge
{
    /** Button bits, keep in sync with lionheart_xr.c. */
    private static final int[] BUTTON_KEYCODES =
    {
        KeyEvent.KEYCODE_BUTTON_A, // A
        KeyEvent.KEYCODE_BUTTON_B, // B
        KeyEvent.KEYCODE_BUTTON_X, // X
        KeyEvent.KEYCODE_BUTTON_Y, // Y
        KeyEvent.KEYCODE_BUTTON_L2, // left trigger
        KeyEvent.KEYCODE_BUTTON_R2, // right trigger
        KeyEvent.KEYCODE_BUTTON_L1, // left grip
        KeyEvent.KEYCODE_BUTTON_R1, // right grip
        KeyEvent.KEYCODE_BUTTON_START, // menu
        KeyEvent.KEYCODE_BUTTON_THUMBL, // left stick click
        KeyEvent.KEYCODE_BUTTON_THUMBR, // right stick click
    };

    /** Button bits (lionheart_xr.c). */
    private static final int BTN_A = 1 << 0;
    private static final int BTN_B = 1 << 1;
    private static final int BTN_MENU = 1 << 8;
    private static final float STICK_PUSH = 0.6f;
    private static final float STICK_REST = 0.3f;

    /** Keys currently pressed on the game's pad. */
    private static int padHeld;
    private static boolean stickArmed = true;

    private static volatile Gamepad gamepad;
    private static int previous;

    static
    {
        System.loadLibrary("lionheartxr");
    }

    private XrBridge()
    {
        // Static
    }

    /**
     * Start the OpenXR thread.
     *
     * @param activity The activity.
     * @param texture The game frame target.
     * @param width The game frame width.
     * @param height The game frame height.
     * @param filesDir The directory holding the optional vr.cfg.
     */
    static void start(ActivityLionheartVR activity, SurfaceTexture texture, int width, int height, String filesDir)
    {
        gamepad = activity.getGamepad();
        nativeStart(activity, texture, width, height, filesDir);
    }

    static void stop()
    {
        nativeStop();
    }

    /**
     * Called from the OpenXR thread every frame.
     *
     * @param buttons The button bits.
     * @param lx Left stick x (right = positive).
     * @param ly Left stick y (up = positive).
     * @param rx Right stick x.
     * @param ry Right stick y.
     */
    static void onInput(int buttons, float lx, float ly, float rx, float ry)
    {
        final Gamepad pad = gamepad;
        if (pad == null)
        {
            return;
        }
        final int changed = buttons ^ previous;
        final int pressed = changed & buttons;
        previous = buttons;

        // Menu button: quick save / load menu, only while a level is running.
        if ((pressed & BTN_MENU) != 0 && StereoRouter.isScene())
        {
            QuickMenu.post(QuickMenu.EVENT_TOGGLE);
        }
        if (QuickMenu.isOpen())
        {
            menuInput(pad, pressed, lx, ly);
            return;
        }
        for (int i = 0; i < BUTTON_KEYCODES.length; i++)
        {
            final int bit = 1 << i;
            if ((changed & bit) != 0)
            {
                if ((buttons & bit) != 0 && (padHeld & bit) == 0)
                {
                    padHeld |= bit;
                    pad.onKeyDown(BUTTON_KEYCODES[i], null);
                }
                else if ((buttons & bit) == 0 && (padHeld & bit) != 0)
                {
                    padHeld &= ~bit;
                    pad.onKeyUp(BUTTON_KEYCODES[i], null);
                }
            }
        }

        // Only the left stick moves (menus and levels); the right stick is ignored.
        // Android axis convention has y pointing down.
        pad.setAxes(lx, -ly);
    }

    /**
     * Touch drives the quick menu; the game gets no input meanwhile (held keys released, stick neutral). Buttons
     * that were held when the menu opened only reach the game again after a fresh press.
     */
    private static void menuInput(Gamepad pad, int pressed, float lx, float ly)
    {
        for (int i = 0; i < BUTTON_KEYCODES.length; i++)
        {
            final int bit = 1 << i;
            if ((padHeld & bit) != 0)
            {
                padHeld &= ~bit;
                pad.onKeyUp(BUTTON_KEYCODES[i], null);
            }
        }
        pad.setAxes(0.0, 0.0);

        if ((pressed & BTN_A) != 0)
        {
            QuickMenu.post(QuickMenu.EVENT_CONFIRM);
        }
        if ((pressed & BTN_B) != 0)
        {
            QuickMenu.post(QuickMenu.EVENT_BACK);
        }

        // Stick flicks with re-arm, so one push moves one entry.
        if (stickArmed)
        {
            if (ly > STICK_PUSH)
            {
                QuickMenu.post(QuickMenu.EVENT_UP);
                stickArmed = false;
            }
            else if (ly < -STICK_PUSH)
            {
                QuickMenu.post(QuickMenu.EVENT_DOWN);
                stickArmed = false;
            }
            else if (Math.abs(lx) > STICK_PUSH)
            {
                QuickMenu.post(QuickMenu.EVENT_SWITCH);
                stickArmed = false;
            }
        }
        else if (Math.abs(lx) < STICK_REST && Math.abs(ly) < STICK_REST)
        {
            stickArmed = true;
        }
    }

    private static native void nativeStart(ActivityLionheartVR activity,
                                           SurfaceTexture texture,
                                           int width,
                                           int height,
                                           String filesDir);

    /**
     * Describe the frame about to be posted (matched with the buffer timestamp on the native side).
     *
     * @param time The time before posting (System.nanoTime).
     * @param mask The layers present.
     * @param kinds The floor layers.
     * @param width The content width of each cell.
     * @param height The content height of each cell.
     */
    static void frameInfo(long time, int mask, int kinds, int width, int height)
    {
        nativeFrameInfo(time, mask, kinds, width, height);
    }

    private static native void nativeFrameInfo(long time, int mask, int kinds, int width, int height);

    private static native void nativeStop();
}
