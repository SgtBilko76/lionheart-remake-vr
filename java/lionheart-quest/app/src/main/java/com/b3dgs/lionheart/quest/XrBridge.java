/*
 * Copyright (C) 2026 - GPL-3.0-or-later (same licence as Lionheart Remake).
 */
package com.b3dgs.lionheart.quest;

import android.graphics.SurfaceTexture;
import android.view.KeyEvent;

import com.b3dgs.lionengine.android.Gamepad;

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
        for (int i = 0; i < BUTTON_KEYCODES.length; i++)
        {
            final int bit = 1 << i;
            if ((changed & bit) != 0)
            {
                if ((buttons & bit) != 0)
                {
                    pad.onKeyDown(BUTTON_KEYCODES[i], null);
                }
                else
                {
                    pad.onKeyUp(BUTTON_KEYCODES[i], null);
                }
            }
        }
        previous = buttons;

        // Either stick moves; Android axis convention has y pointing down.
        final float x = Math.abs(lx) >= Math.abs(rx) ? lx : rx;
        final float y = Math.abs(ly) >= Math.abs(ry) ? ly : ry;
        pad.setAxes(x, -y);
    }

    private static native void nativeStart(ActivityLionheartVR activity,
                                           SurfaceTexture texture,
                                           int width,
                                           int height,
                                           String filesDir);

    private static native void nativeStop();
}
