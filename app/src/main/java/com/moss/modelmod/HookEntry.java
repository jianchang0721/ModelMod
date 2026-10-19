package com.moss.modelmod;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewParent;
import android.widget.FrameLayout;
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
    private static int hideMask = 0;   // 默认全部显示

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

        XposedHelpers.findAndHookMethod(Activity.class, "onResume", new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                final Activity act = (Activity) param.thisObject;
                if (act == null || overlayShown) return;
                String cn = act.getClass().getName();
                if (cn.toLowerCase().contains("splash")) return;

                new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        if (overlayShown || act.isFinishing()) return;
                        overlayShown = true;
                        try {
                            showOverlay(act);
                        } catch (Throwable t) {
                            overlayShown = false;
                            XposedBridge.log(TAG + "overlay fail: " + t);
                        }
                    }
                }, 1500);
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
        XposedBridge.log(TAG + "showOverlay on " + act.getClass().getName());

        final LinearLayout root = new LinearLayout(act);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(18, 10, 18, 10);

        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xE6121219);
        bg.setCornerRadius(24f);
        bg.setStroke(2, 0x66FFD54F);
        root.setBackground(bg);

        LinearLayout header = new LinearLayout(act);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setPadding(0, 4, 0, 4);

        final TextView title = new TextView(act);
        title.setText("ModelMod  ▾");
        title.setTextColor(0xFFFFD54F);
        title.setTextSize(13);
        title.setPadding(0, 6, 0, 6);
        header.addView(title);

        final LinearLayout body = new LinearLayout(act);
        body.setOrientation(LinearLayout.VERTICAL);

        TextView hint = new TextView(act);
        hint.setText("勾选 = 隐藏");
        hint.setTextColor(0xFF888888);
        hint.setTextSize(10);
        hint.setPadding(0, 0, 0, 6);
        body.addView(hint);

        final String[] labels = {
                "身体 Body",
                "脸 Face",
                "头发 Hair",
                "武器 Weapon",
                "描边 PX",
                "NPC",
                "特效 Effect",
                "披风 Cloak"
        };
        for (int i = 0; i < labels.length; i++) {
            final int bit = i;
            Switch sw = new Switch(act);
            sw.setText(labels[i]);
            sw.setTextColor(Color.WHITE);
            sw.setTextSize(12);
            sw.setPadding(0, 0, 0, 0);
            sw.setChecked((hideMask & (1 << bit)) != 0);
            sw.setOnCheckedChangeListener((btn, checked) -> {
                if (checked) hideMask |= (1 << bit);
                else hideMask &= ~(1 << bit);
                try {
                    setHideMask(hideMask);
                } catch (Throwable ignored) {
                }
            });
            body.addView(sw);
        }

        root.addView(header);
        root.addView(body);

        FrameLayout.LayoutParams flp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        flp.leftMargin = 36;
        flp.topMargin = 260;

        header.setOnTouchListener(new View.OnTouchListener() {
            private float dx, dy;
            private long downAt;
            private boolean moved;

            @Override
            public boolean onTouch(View v, MotionEvent e) {
                switch (e.getAction()) {
                    case MotionEvent.ACTION_DOWN:
                        dx = e.getRawX() - root.getX();
                        dy = e.getRawY() - root.getY();
                        downAt = System.currentTimeMillis();
                        moved = false;
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        root.setX(e.getRawX() - dx);
                        root.setY(e.getRawY() - dy);
                        moved = true;
                        return true;
                    case MotionEvent.ACTION_UP:
                        if (!moved && System.currentTimeMillis() - downAt < 300) {
                            boolean visible = body.getVisibility() == View.VISIBLE;
                            body.setVisibility(visible ? View.GONE : View.VISIBLE);
                            title.setText(visible ? "ModelMod  ▸" : "ModelMod  ▾");
                        }
                        return true;
                }
                return false;
            }
        });

        act.addContentView(root, flp);

        // 允许拖出容器边界
        ViewParent par = root.getParent();
        if (par instanceof ViewGroup) {
            ((ViewGroup) par).setClipChildren(false);
            ((ViewGroup) par).setClipToPadding(false);
        }

        XposedBridge.log(TAG + "overlay added");
    }
}
