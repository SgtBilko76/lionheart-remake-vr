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

import com.b3dgs.lionengine.graphic.Graphic;

/**
 * Stereo layer hooks (VR port). The scene marks where each depth group starts; a router (set by the VR
 * front-end) redirects the drawing of each group into its own layer. Without router, nothing happens.
 */
public final class StereoLayers
{
    /** Background component 0 (farthest). */
    public static final int BG0 = 0;
    /** Background component 1. */
    public static final int BG1 = 1;
    /** Background component 2 (nearest background). */
    public static final int BG2 = 2;
    /** Playfield: back foreground, map, objects (engine frame buffer). */
    public static final int PLAYFIELD = 4;
    /** Front foreground (water, lava). */
    public static final int FOREGROUND = 5;
    /** Hud and overlays. */
    public static final int HUD = 6;
    /** Background components count. */
    public static final int BG_MAX = 3;

    /** Flat layer. */
    public static final int KIND_PLANE = 0;
    /** Ground parallax (depth grows towards the top). */
    public static final int KIND_FLOOR = 1;

    /**
     * Layer router.
     */
    public interface Router
    {
        /**
         * Start a layer: following drawing goes to this layer.
         * 
         * @param g The graphic output.
         * @param layer The layer id.
         * @param kind The layer kind.
         */
        void layer(Graphic g, int layer, int kind);

        /**
         * End of layered rendering, restore the frame buffer output.
         * 
         * @param g The graphic output.
         */
        void end(Graphic g);
    }

    private static volatile Router router;

    /**
     * Set the router.
     * 
     * @param router The router (<code>null</code> to disable).
     */
    public static void setRouter(Router router)
    {
        StereoLayers.router = router;
    }

    /**
     * Start a layer.
     * 
     * @param g The graphic output.
     * @param layer The layer id.
     * @param kind The layer kind.
     */
    public static void layer(Graphic g, int layer, int kind)
    {
        final Router r = router;
        if (r != null)
        {
            r.layer(g, layer, kind);
        }
    }

    /**
     * End layered rendering.
     * 
     * @param g The graphic output.
     */
    public static void end(Graphic g)
    {
        final Router r = router;
        if (r != null)
        {
            r.end(g);
        }
    }

    private StereoLayers()
    {
        // Static
    }
}
