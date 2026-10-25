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
package com.b3dgs.lionheart.landscape;

import com.b3dgs.lionengine.game.background.BackgroundAbstract;
import com.b3dgs.lionengine.game.background.BackgroundComponent;
import com.b3dgs.lionengine.game.background.Parallax;
import com.b3dgs.lionengine.graphic.Graphic;
import com.b3dgs.lionheart.StereoLayers;

/**
 * Background whose components are each rendered on their own stereo layer (VR port).
 */
abstract class BackgroundStereo extends BackgroundAbstract
{
    private int count;

    /**
     * Create background.
     * 
     * @param theme The theme name.
     * @param min The minimal y value for background.
     * @param max The maximal y value for background.
     */
    protected BackgroundStereo(String theme, int min, int max)
    {
        super(theme, min, max);
    }

    @Override
    protected void add(BackgroundComponent component)
    {
        final int layer = Math.min(count++, StereoLayers.BG_MAX - 1);
        final int kind = component instanceof Parallax ? StereoLayers.KIND_FLOOR : StereoLayers.KIND_PLANE;
        super.add(new BackgroundComponent()
        {
            @Override
            public void update(double extrp, int x, int y, double speed)
            {
                component.update(extrp, x, y, speed);
            }

            @Override
            public void render(Graphic g)
            {
                StereoLayers.layer(g, layer, kind);
                component.render(g);
            }
        });
    }
}
