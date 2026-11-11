package com.legacy.mclce;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.graphics.SurfaceTexture;
import android.graphics.Typeface;
import android.hardware.display.DisplayManager;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.TextureView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;

public class MainActivity extends Activity implements InputManager.InputDeviceListener {
    static {
        System.loadLibrary("mcle");
    }

    // What each surface is for, bottom to top. The world is drawn in the
    // first, the menu movies in the second, the items the game draws into
    // them in the third, and what a movie has over those in the fourth.
    private static final int LAYER_WORLD = 0;
    private static final int LAYER_SWF = 1;
    private static final int LAYER_CUSTOM_DRAW = 2;
    private static final int LAYER_SWF_TOP = 3;

    private native void nativeCreate(String documents, String support, String caches, float density);
    private native void nativeSurface(int layer, Surface surface, int width, int height);
    private native void nativeResume(float refreshRate);
    private native void nativeRefreshRate(float refreshRate);
    private native void nativePause();
    private native void nativeKey(int deviceId, int keyCode, boolean down);
    private native void nativeAxes(int deviceId, float[] axes);
    private native void nativePadChanged(int deviceId, boolean connected, int vendorId);

    // The axes handed to nativeAxes, in this order
    private static final int[] AXES = {
        MotionEvent.AXIS_X, MotionEvent.AXIS_Y,
        MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
        MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER,
        MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_GAS,
        MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y,
    };
    private final float[] axisValues = new float[AXES.length];

    private File documents;
    private float density;
    private SurfaceView world;
    private View introBackdrop;
    private TextView splash;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // Edge to edge from the first layout, so the surfaces come up at
        // the size they stay
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        askForFastestMode();

        documents = getExternalFilesDir(null);
        if (documents == null) documents = getFilesDir();
        density = getResources().getDisplayMetrics().density;
        nativeCreate(documents.getAbsolutePath(), getFilesDir().getAbsolutePath(),
            getCacheDir().getAbsolutePath(), density);

        FrameLayout layout = new FrameLayout(this);
        world = new SurfaceView(this);
        world.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                nativeSurface(LAYER_WORLD, holder.getSurface(), width, height);
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                nativeSurface(LAYER_WORLD, null, 0, 0);
            }
        });
        layout.addView(world);

        // Windows64 clears to white outside a game, which is what shows
        // around the intro's logos
        introBackdrop = new View(this);
        introBackdrop.setBackgroundColor(Color.WHITE);
        introBackdrop.setVisibility(View.GONE);
        layout.addView(introBackdrop);

        layout.addView(overlay(LAYER_SWF));
        layout.addView(overlay(LAYER_CUSTOM_DRAW));
        layout.addView(overlay(LAYER_SWF_TOP));

        // The yellow splash line by the logo. The console draws it outside
        // the movie too (CXuiCtrlSplashPulser).
        splash = new TextView(this);
        splash.setTextColor(Color.rgb(255, 255, 0));
        splash.setShadowLayer(1, density, density, Color.argb(204, 0, 0, 0));
        splash.setTextSize(TypedValue.COMPLEX_UNIT_DIP, 18);
        splash.setGravity(Gravity.CENTER);
        splash.setSingleLine(true);
        splash.setVisibility(View.GONE);
        layout.addView(splash, new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT));
        setContentView(layout);
        hideSystemBars();

        ((InputManager) getSystemService(Context.INPUT_SERVICE)).registerInputDeviceListener(this, null);
    }

    // Called from the frame thread. The centre is in density-independent
    // units, the rotation in radians.
    void setSplash(final String text, final boolean hidden, final float alpha,
                   final float x, final float y, final float rotation, final float scale) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                // The game's own font, once it's been unpacked next to the game files
                if (splash.getTag() == null) {
                    File font = new File(documents, "Mojang Font_7.ttf");
                    if (font.exists()) {
                        splash.setTypeface(Typeface.createFromFile(font));
                        splash.setTag(font);
                    }
                }
                if (!text.contentEquals(splash.getText())) splash.setText(text);
                splash.setVisibility(hidden ? View.GONE : View.VISIBLE);
                splash.setAlpha(alpha);
                splash.measure(View.MeasureSpec.UNSPECIFIED, View.MeasureSpec.UNSPECIFIED);
                splash.setTranslationX(x * density - splash.getMeasuredWidth() / 2f);
                splash.setTranslationY(y * density - splash.getMeasuredHeight() / 2f);
                splash.setRotation((float) Math.toDegrees(rotation));
                splash.setScaleX(scale);
                splash.setScaleY(scale);
            }
        });
    }

    // The world's surface with this many lines, fewer than the screen has.
    // The display scales it up to the view.
    void setWorldLines(final int lines) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                int width = world.getWidth();
                int height = world.getHeight();
                int shorter = Math.min(width, height);
                if (lines <= 0 || shorter <= lines) return;
                world.getHolder().setFixedSize(width * lines / shorter, height * lines / shorter);
            }
        });
    }

    void setIntroBackdrop(final boolean shown) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                introBackdrop.setVisibility(shown ? View.VISIBLE : View.GONE);
            }
        });
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
        reportPad(deviceId, true);
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        nativePadChanged(deviceId, false, 0);
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
    }

    // A see-through layer over the world
    private TextureView overlay(final int layer) {
        TextureView view = new TextureView(this);
        view.setOpaque(false);
        view.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            private Surface surface;

            @Override
            public void onSurfaceTextureAvailable(SurfaceTexture texture, int width, int height) {
                surface = new Surface(texture);
                nativeSurface(layer, surface, width, height);
            }

            @Override
            public void onSurfaceTextureSizeChanged(SurfaceTexture texture, int width, int height) {
                nativeSurface(layer, surface, width, height);
            }

            @Override
            public boolean onSurfaceTextureDestroyed(SurfaceTexture texture) {
                nativeSurface(layer, null, 0, 0);
                if (surface != null) surface.release();
                surface = null;
                return true;
            }

            @Override
            public void onSurfaceTextureUpdated(SurfaceTexture texture) {
            }
        });
        return view;
    }

    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    // A frame is drawn every refresh, so the screen is asked for the
    // fastest mode it has at the size it's showing
    private void askForFastestMode() {
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode shown = display.getMode();
        Display.Mode fastest = shown;
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == shown.getPhysicalWidth()
                    && mode.getPhysicalHeight() == shown.getPhysicalHeight()
                    && mode.getRefreshRate() > fastest.getRefreshRate()) {
                fastest = mode;
            }
        }
        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        attributes.preferredDisplayModeId = fastest.getModeId();
        getWindow().setAttributes(attributes);
    }

    // The mode changes a little after it's asked for
    private final DisplayManager.DisplayListener displayListener = new DisplayManager.DisplayListener() {
        @Override public void onDisplayAdded(int id) {}
        @Override public void onDisplayRemoved(int id) {}
        @Override public void onDisplayChanged(int id) {
            nativeRefreshRate(getWindowManager().getDefaultDisplay().getRefreshRate());
        }
    };

    @Override
    protected void onResume() {
        super.onResume();
        for (int id : InputDevice.getDeviceIds()) reportPad(id, true);
        ((DisplayManager) getSystemService(Context.DISPLAY_SERVICE)).registerDisplayListener(displayListener, null);
        nativeResume(getWindowManager().getDefaultDisplay().getRefreshRate());
    }

    @Override
    protected void onPause() {
        ((DisplayManager) getSystemService(Context.DISPLAY_SERVICE)).unregisterDisplayListener(displayListener);
        nativePause();
        super.onPause();
    }

    // The way out is the game's own Exit Game
    @Override
    public void onBackPressed() {
    }

    private static boolean isPad(InputDevice device) {
        if (device == null) return false;
        int sources = device.getSources();
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private void reportPad(int deviceId, boolean connected) {
        InputDevice device = InputDevice.getDevice(deviceId);
        if (connected && !isPad(device)) return;
        nativePadChanged(deviceId, connected, device != null ? device.getVendorId() : 0);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (isPad(event.getDevice())) {
            int action = event.getAction();
            if (action == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
                nativeKey(event.getDeviceId(), event.getKeyCode(), true);
            } else if (action == KeyEvent.ACTION_UP) {
                nativeKey(event.getDeviceId(), event.getKeyCode(), false);
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                && event.getAction() == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < AXES.length; i++) axisValues[i] = event.getAxisValue(AXES[i]);
            nativeAxes(event.getDeviceId(), axisValues);
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }
}
