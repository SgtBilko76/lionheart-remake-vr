/*
 * Copyright (C) 2026 - GPL-3.0-or-later (same licence as Lionheart Remake).
 */
package com.b3dgs.lionheart.quest;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.Rect;

import com.b3dgs.lionengine.android.graphic.ScreenAndroid;
import com.b3dgs.lionengine.graphic.Graphic;
import com.b3dgs.lionheart.StereoLayers;

/**
 * Routes each stereo layer of the scene into its own bitmap, then packs the engine frame buffer and the layers
 * into one atlas (2 x 4 cells of 640x480) that the OpenXR host renders at different depths.
 * Everything runs on the game thread.
 */
final class StereoRouter implements StereoLayers.Router, ScreenAndroid.VrCompositor
{
    static final int CELL_W = 640;
    static final int CELL_H = 480;
    static final int COLS = 2;
    static final int ROWS = 4;
    static final int ATLAS_W = CELL_W * COLS;
    static final int ATLAS_H = CELL_H * ROWS;
    private static final int LAYERS = COLS * ROWS;
    private static final int PLAYFIELD_BIT = 1 << StereoLayers.PLAYFIELD;

    /** True while a level (layered scene) is on screen, false in menus, intro and story pictures. */
    private static volatile boolean scene;

    /**
     * Check if a level is on screen.
     *
     * @return <code>true</code> in a level, <code>false</code> in menus.
     */
    static boolean isScene()
    {
        return scene;
    }

    private final Bitmap[] bitmaps = new Bitmap[LAYERS];
    private final Canvas[] canvases = new Canvas[LAYERS];
    private final Paint paint = new Paint();
    private final Rect src = new Rect();
    private final Rect dst = new Rect();

    /** Engine frame buffer canvas of the frame being rendered (null outside layered rendering). */
    private Canvas frameCanvas;
    private int mask;
    private int kinds;
    /** Layers of the last completed scene frame, consumed by compose. */
    private int doneMask;
    private int doneKinds;

    private int postMask;
    private int postKinds;
    private int postWidth;
    private int postHeight;

    StereoRouter()
    {
        paint.setFilterBitmap(false);
        paint.setAntiAlias(false);
    }

    private Canvas getLayerCanvas(int id, int width, int height)
    {
        final Bitmap current = bitmaps[id];
        if (current == null || current.getWidth() != width || current.getHeight() != height)
        {
            if (current != null)
            {
                current.recycle();
            }
            bitmaps[id] = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
            canvases[id] = new Canvas(bitmaps[id]);
        }
        return canvases[id];
    }

    /*
     * StereoLayers.Router
     */

    @Override
    public void layer(Graphic g, int layer, int kind)
    {
        if (layer < 0 || layer >= LAYERS)
        {
            return;
        }
        final Canvas current = (Canvas) g.getGraphic();
        if (frameCanvas == null)
        {
            frameCanvas = current;
        }
        final Canvas target;
        if (layer == StereoLayers.PLAYFIELD)
        {
            target = frameCanvas;
        }
        else
        {
            target = getLayerCanvas(layer, frameCanvas.getWidth(), frameCanvas.getHeight());
        }
        final int bit = 1 << layer;
        if ((mask & bit) == 0)
        {
            target.drawColor(0, PorterDuff.Mode.CLEAR);
            mask |= bit;
            if (kind == StereoLayers.KIND_FLOOR)
            {
                kinds |= bit;
            }
        }
        g.setGraphic(target);
    }

    @Override
    public void end(Graphic g)
    {
        if (frameCanvas != null)
        {
            g.setGraphic(frameCanvas);
        }
        doneMask = mask;
        doneKinds = kinds;
        frameCanvas = null;
        mask = 0;
        kinds = 0;
    }

    /*
     * ScreenAndroid.VrCompositor
     */

    @Override
    public void compose(Canvas atlas, Bitmap frame)
    {
        atlas.drawColor(0, PorterDuff.Mode.CLEAR);

        final int layers = doneMask;
        final int layerKinds = doneKinds;
        doneMask = 0;
        doneKinds = 0;
        scene = layers != 0;

        if (frame == null)
        {
            postMask = 0;
            return;
        }

        // Engine frame buffer (playfield in scenes, whole picture in menus) at its own resolution.
        int w = frame.getWidth();
        int h = frame.getHeight();
        if (w > CELL_W || h > CELL_H)
        {
            final double scale = Math.min(CELL_W / (double) w, CELL_H / (double) h);
            w = (int) Math.floor(w * scale);
            h = (int) Math.floor(h * scale);
        }
        drawCell(atlas, frame, StereoLayers.PLAYFIELD, w, h);

        for (int id = 0; id < LAYERS; id++)
        {
            final int bit = 1 << id;
            if (id != StereoLayers.PLAYFIELD && (layers & bit) != 0 && bitmaps[id] != null)
            {
                drawCell(atlas, bitmaps[id], id, w, h);
            }
        }

        postMask = layers | PLAYFIELD_BIT;
        postKinds = layerKinds;
        postWidth = w;
        postHeight = h;
    }

    private void drawCell(Canvas atlas, Bitmap bitmap, int id, int w, int h)
    {
        final int x = id % COLS * CELL_W;
        final int y = id / COLS * CELL_H;
        src.set(0, 0, bitmap.getWidth(), bitmap.getHeight());
        dst.set(x, y, x + w, y + h);
        atlas.drawBitmap(bitmap, src, dst, paint);
    }

    @Override
    public void posted()
    {
        XrBridge.frameInfo(System.nanoTime(), postMask, postKinds, postWidth, postHeight);
    }
}
