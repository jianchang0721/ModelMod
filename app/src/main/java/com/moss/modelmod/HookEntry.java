package com.moss.modelmod;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

import java.io.File;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

public class HookEntry implements IXposedHookLoadPackage {

    private static native void setLogPath(String path);
    private static native void setHideMask(int mask);

    private static final String TAG    = "[ModelMod] ";
    private static final String TARGET = "com.hermes.h1game";
    private static final String SELF   = "com.moss.modelmod";

    private static boolean overlayShown = false;
    private static int hideMask = 0x3F;

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam lp) {
        if (!TARGET.equals(lp.packageName)) return;

        XposedBridge.log(TAG + "enter process: " + lp.processName);

        XposedHelpers.findAndHookMethod(Application.class, "attach", Context.class,
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam param) {
                        Context ctx = (Context) param.args[0];
                        if (ctx != null) loadNative(ctx);
                    }
                });

        XposedHelpers.findAndHookMethod(Activity.class, "onCreate", Bundle.class,
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam param) {
                        final Activity act = (Activity) param.thisObject;
                        if (act == null) return;
                        new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
                            @Override
                            public void run() {
                                if (overlayShown) return;
                                overlayShown = true;
                                try {
                                    showOverlay(act);
                                } catch (Throwable t) {
                                    XposedBridge.log(TAG + "overlay fail: " + t);
                                }
                            }
                        }, 6000);
                    }
                });
    }

    private void loadNative(Context ctx) {
        try {
            ApplicationInfo ai = ctx.getPackageManager().getApplicationInfo(SELF, 0);
            String so = ai.nativeLibraryDir + "/libmodelmod.so";
            System.load(so);
            XposedBridge.log(TAG + "native loaded: " + so);

            String logPath = new File(ctx.getFilesDir(), "modelmod.log").getAbsolutePath();
            setLogPath(logPath);
            setHideMask(hideMask);
        } catch (Throwable t) {
            XposedBridge.log(TAG + "native load failed: " + t);
        }
    }

    private void showOverlay(Activity act) {
        WindowManager wm = (WindowManager) act.getSystemService(Context.WINDOW_SERVICE);
        if (wm == null) return;

        LinearLayout root = new LinearLayout(act);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xDD101018);
        root.setPadding(24, 16, 24, 16);

        TextView title = new TextView(act);
        title.setText("ModelMod");
        title.setTextColor(Color.WHITE);
        title.setTextSize(14);
        title.setPadding(0, 0, 0, 8);
        root.addView(title);

        final String[] labels = {"身体 Body", "脸 Face", "头发 Hair", "武器 Weapon", "描边 PX", "NPC"};
        for (int i = 0; i < labels.length; i++) {
            final int bit = i;
            Switch sw = new Switch(act);
            sw.setText(labels[i]);
            sw.setTextColor(Color.WHITE);
            sw.setTextSize(12);
            sw.setChecked((hideMask & (1 << bit)) != 0);
            sw.setOnCheckedChangeListener((btn, checked) -> {
                if (checked) hideMask |= (1 << bit);
                else hideMask &= ~(1 << bit);
                try {
                    setHideMask(hideMask);
                } catch (Throwable ignored) {
                }
            });
            root.addView(sw);
        }

        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
                PixelFormat.TRANSLUCENT);
        lp.token = act.getWindow().getDecorView().getWindowToken();
        lp.gravity = Gravity.TOP | Gravity.START;
        lp.x = 40;
        lp.y = 240;

        title.setOnTouchListener(new View.OnTouchListener() {
            private int sx, sy;
            private float dx, dy;

            @Override
            public boolean onTouch(View v, MotionEvent e) {
                switch (e.getAction()) {
                    case MotionEvent.ACTION_DOWN:
                        sx = lp.x;
                        sy = lp.y;
                        dx = e.getRawX();
                        dy = e.getRawY();
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        lp.x = sx + (int) (e.getRawX() - dx);
                        lp.y = sy + (int) (e.getRawY() - dy);
                        wm.updateViewLayout(root, lp);
                        return true;
                }
                return false;
            }
        });

        wm.addView(root, lp);
        XposedBridge.log(TAG + "overlay shown");
    }
}
