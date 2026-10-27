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

import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Save request from a front-end button (VR port): the running level saves the progress at the last checkpoint on
 * its next update. Without request, nothing happens.
 */
public final class SaveRequest
{
    private static final AtomicBoolean REQUESTED = new AtomicBoolean();

    /**
     * Request a save (any thread).
     */
    public static void request()
    {
        REQUESTED.set(true);
    }

    /**
     * Consume the request (game thread).
     * 
     * @return <code>true</code> if a save was requested since the last call.
     */
    public static boolean consume()
    {
        return REQUESTED.getAndSet(false);
    }

    private SaveRequest()
    {
        // Static
    }
}
