/*
 * Copyright (C) 2026 - GPL-3.0-or-later (same licence as Lionheart Remake).
 */
package com.b3dgs.lionheart.quest;

import android.graphics.SurfaceTexture;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;

import com.b3dgs.lionengine.Config;
import com.b3dgs.lionengine.Resolution;
import com.b3dgs.lionengine.android.ActivityGame;
import com.b3dgs.lionengine.android.graphic.EngineAndroid;
import com.b3dgs.lionengine.android.graphic.ScreenAndroid;
import com.b3dgs.lionengine.audio.AudioFactory;
import com.b3dgs.lionengine.audio.sc68.Sc68Format;
import com.b3dgs.lionengine.graphic.engine.Loader;
import com.b3dgs.lionheart.Constant;
import com.b3dgs.lionheart.GameConfig;
import com.b3dgs.lionheart.Loading;
import com.b3dgs.lionheart.Settings;

import java.io.File;

/**
 * Meta Quest entry point: the game renders into an offscreen surface that the OpenXR host shows as a
 * virtual screen.
 */
public final class ActivityLionheartVR extends ActivityGame
{
    private static final String TAG = "LionheartVR";
    /** Game frame size: 4x the original 320x240, so the compositor scales crisp pixel art down. */
    private static final int WIDTH = Constant.RESOLUTION.width() * 4;
    private static final int HEIGHT = Constant.RESOLUTION.height() * 4;

    private SurfaceTexture texture;
    private Surface surface;

    @Override
    protected void start(Bundle bundle)
    {
        texture = new SurfaceTexture(false);
        texture.setDefaultBufferSize(WIDTH, HEIGHT);
        surface = new Surface(texture);
        ScreenAndroid.setVrSurface(surface, WIDTH, HEIGHT);

        final File files = getExternalFilesDir(null);
        XrBridge.start(this, texture, WIDTH, HEIGHT, files != null ? files.getAbsolutePath() : null);
        Log.i(TAG, "VR host started, game surface " + WIDTH + "x" + HEIGHT);

        // Assets live under com/b3dgs/lionheart, not under the .quest package name.
        EngineAndroid.setResourcesPackage("com.b3dgs.lionheart");
        EngineAndroid.start(Constant.PROGRAM_NAME, Constant.PROGRAM_VERSION, this);
        AudioFactory.addFormat(Sc68Format.getFailsafe());

        Settings.load();
        final Settings settings = Settings.getInstance();
        AudioFactory.setVolume(settings.getVolumeMaster());

        final Resolution resolution = new Resolution(WIDTH,
                                                     HEIGHT,
                                                     settings.getResolution(Constant.RESOLUTION_OUTPUT).rate());
        final Config config = Config.fullscreen(resolution);
        ScreenAndroid.setSize(config);
        Loader.startAsync(config, Loading.class, new GameConfig(), Boolean.FALSE);
    }

    @Override
    protected void onDestroy()
    {
        XrBridge.stop();
        super.onDestroy();
    }
}
