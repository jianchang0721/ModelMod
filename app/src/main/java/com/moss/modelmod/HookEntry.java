package com.moss.modelmod;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.view.Gravity;
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
import java.io.FileOutputStream;
import java.io.InputStream;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

public class HookEntry implements IXposedHookLoadPackage {

    private static native void setLogPath(String path);
    private static native void setHideMask(int mask);
    private static native void reloadModel();

    private static final int REQ_PICK = 0x4D44;   // 自定义模型选择器

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

        // 文件选择器回调：把选中的文件复制到游戏私有目录，然后重载模型
        XposedHelpers.findAndHookMethod(Activity.class, "onActivityResult",
                int.class, int.class, Intent.class, new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam param) {
                int req = (Integer) param.args[0];
                if (req != REQ_PICK) return;
                int res = (Integer) param.args[1];
                Intent data = (Intent) param.args[2];
                if (res != Activity.RESULT_OK || data == null || data.getData() == null) return;
                Activity act = (Activity) param.thisObject;
                copyUriToFiles(act, data.getData());
                try { reloadModel(); } catch (Throwable ignored) { }
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

        // ---- 两个操作按钮 ----
        LinearLayout btnRow = new LinearLayout(act);
        btnRow.setOrientation(LinearLayout.HORIZONTAL);
        btnRow.setPadding(0, 10, 0, 2);

        TextView btnReload = mkBtn(act, "刷新模型");
        btnReload.setOnClickListener(v -> {
            try {
                reloadModel();
                toast(act, "已刷新");
            } catch (Throwable t) {
                XposedBridge.log(TAG + "reload fail: " + t);
            }
        });

        TextView btnPick = mkBtn(act, "加载模型");
        btnPick.setOnClickListener(v -> pickModel(act));

        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        blp.rightMargin = 10;
        btnRow.addView(btnReload, blp);
        LinearLayout.LayoutParams blp2 = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        btnRow.addView(btnPick, blp2);
        body.addView(btnRow);

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

    private TextView mkBtn(Activity act, String text) {
        TextView tv = new TextView(act);
        tv.setText(text);
        tv.setTextColor(0xFFFFD54F);
        tv.setTextSize(12);
        tv.setGravity(Gravity.CENTER);
        tv.setPadding(20, 16, 20, 16);
        GradientDrawable b = new GradientDrawable();
        b.setColor(0x33FFD54F);
        b.setCornerRadius(14f);
        b.setStroke(1, 0xAAFFD54F);
        tv.setBackground(b);
        return tv;
    }

    private void toast(Activity act, String msg) {
        try {
            android.widget.Toast.makeText(act, msg, android.widget.Toast.LENGTH_SHORT).show();
        } catch (Throwable ignored) {
        }
    }

    private void pickModel(Activity act) {
        try {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            act.startActivityForResult(i, REQ_PICK);
        } catch (Throwable t) {
            XposedBridge.log(TAG + "pick fail: " + t);
            toast(act, "打不开文件选择器");
        }
    }

    private void copyUriToFiles(Context ctx, Uri uri) {
        InputStream in = null;
        FileOutputStream out = null;
        try {
            in = ctx.getContentResolver().openInputStream(uri);
            File dst = new File(ctx.getFilesDir(), "custom.mmesh");
            out = new FileOutputStream(dst);
            byte[] buf = new byte[65536];
            int n;
            long total = 0;
            while ((n = in.read(buf)) > 0) { out.write(buf, 0, n); total += n; }
            XposedBridge.log(TAG + "model copied: " + dst.getAbsolutePath() + " size=" + total);
        } catch (Throwable t) {
            XposedBridge.log(TAG + "copy fail: " + t);
        } finally {
            try { if (out != null) out.close(); } catch (Throwable ignored) { }
            try { if (in != null) in.close(); } catch (Throwable ignored) { }
        }
    }
}
