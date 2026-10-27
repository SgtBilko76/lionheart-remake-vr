/*
 * Copyright (C) 2026 - GPL-3.0-or-later (same licence as Lionheart Remake).
 */
package com.b3dgs.lionheart.quest;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.Rect;

import com.b3dgs.lionengine.android.graphic.GraphicAndroid;
import com.b3dgs.lionengine.android.graphic.ScreenAndroid;
import com.b3dgs.lionengine.graphic.Graphic;
import com.b3dgs.lionheart.StereoLayers;

/**
 * Routes each stereo layer of the scene into its own bitmap, then packs the engine frame buffer and the layers
 * into one atlas (2 x 4 cells of 640x480) that the OpenXR host renders at different depths. Vector text (menus,
 * messages, info) goes to a separate high resolution layer below the cells (1280x960, frame scaled up to fit).
 * Everything runs on the game thread.
 */
final class StereoRouter implements StereoLayers.Router, ScreenAndroid.VrCompositor, GraphicAndroid.TextSink
{
    static final int CELL_W = 640;
    static final int CELL_H = 480;
    static final int COLS = 2;
    static final int ROWS = 4;
    static final int ATLAS_W = CELL_W * COLS;
    static final int TEXT_W = 1280;
    static final int TEXT_H = 960;
    static final int TEXT_Y = CELL_H * ROWS;
    static final int ATLAS_H = TEXT_Y + TEXT_H;
    /** Text layer id in the frame info mask (cell 7 is unused by the scene layers). */
    static final int TEXT_LAYER = 7;
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

    /** High resolution text layer. */
    private final Bitmap textBitmap = Bitmap.createBitmap(TEXT_W, TEXT_H, Bitmap.Config.ARGB_8888);
    private final Canvas textCanvas = new Canvas(textBitmap);
    private boolean textUsed;
    private int textW;
    private int textH;
    private final Paint textPaint = new Paint();

    private int postMask;
    private int postKinds;
    private int postWidth;
    private int postHeight;

    StereoRouter()
    {
        paint.setFilterBitmap(false);
        paint.setAntiAlias(false);
        textPaint.setFilterBitmap(false);
    }

    /**
     * Text scale for a frame size.
     *
     * @param width The frame width.
     * @param height The frame height.
     * @return The scale (frame to text layer).
     */
    static float textScale(int width, int height)
    {
        return Math.min(TEXT_W / (float) width, TEXT_H / (float) height);
    }

    /*
     * GraphicAndroid.TextSink
     */

    @Override
    public Canvas canvasFor(int width, int height)
    {
        if (width <= 0 || height <= 0 || width > TEXT_W || height > TEXT_H)
        {
            return null;
        }
        if (!textUsed || width != textW || height != textH)
        {
            final float scale = textScale(width, height);
            textCanvas.setMatrix(null);
            textCanvas.drawColor(0, PorterDuff.Mode.CLEAR);
            textCanvas.scale(scale, scale);
            textUsed = true;
            textW = width;
            textH = height;
        }
        return textCanvas;
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

        int text = 0;
        if (textUsed)
        {
            // Text layer content: the frame size scaled up (same aspect as the cells).
            final float scale = textScale(textW, textH);
            final int tw = Math.round(textW * scale);
            final int th = Math.round(textH * scale);
            src.set(0, 0, tw, th);
            dst.set(0, TEXT_Y, tw, TEXT_Y + th);
            atlas.drawBitmap(textBitmap, src, dst, textPaint);
            textUsed = false;
            text = 1 << TEXT_LAYER;
        }

        postMask = layers | PLAYFIELD_BIT | text;
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
