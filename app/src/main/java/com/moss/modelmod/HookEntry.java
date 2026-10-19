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

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;

import org.json.JSONArray;
import org.json.JSONObject;

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
        try {
            byte[] data = readAll(ctx, uri);
            if (data == null || data.length < 24) { XposedBridge.log(TAG + "file too small"); return; }

            byte[] out;
            if (data[0] == 'g' && data[1] == 'l' && data[2] == 'T' && data[3] == 'F') {
                out = glbToMmesh(data);
                if (out == null) { XposedBridge.log(TAG + "glb convert failed"); return; }
                XposedBridge.log(TAG + "glb converted: " + out.length + " bytes");
            } else if (data[0] == 'M' && data[1] == 'M' && data[2] == 'S' && data[3] == 'H') {
                out = data;
                XposedBridge.log(TAG + "already mmesh: " + out.length + " bytes");
            } else {
                XposedBridge.log(TAG + "unknown format, first4=" + data[0] + "," + data[1] + "," + data[2] + "," + data[3]);
                return;
            }

            File dst = new File(ctx.getFilesDir(), "custom.mmesh");
            FileOutputStream fos = new FileOutputStream(dst);
            fos.write(out);
            fos.close();
            XposedBridge.log(TAG + "model ready: " + dst.getAbsolutePath() + " size=" + out.length);
        } catch (Throwable t) {
            XposedBridge.log(TAG + "copy fail: " + t);
        }
    }

    private byte[] readAll(Context ctx, Uri uri) throws Exception {
        InputStream in = ctx.getContentResolver().openInputStream(uri);
        if (in == null) return null;
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        int n;
        while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
        in.close();
        return bo.toByteArray();
    }

    private static void wInt(ByteArrayOutputStream o, int v) {
        o.write(v & 0xFF);
        o.write((v >> 8) & 0xFF);
        o.write((v >> 16) & 0xFF);
        o.write((v >> 24) & 0xFF);
    }

    private static void wFloat(ByteArrayOutputStream o, float f) {
        wInt(o, Float.floatToIntBits(f));
    }

    private static ByteBuffer viewOf(byte[] bin, JSONArray bvs, JSONObject acc) throws Exception {
        JSONObject bv = bvs.getJSONObject(acc.getInt("bufferView"));
        int bvOff  = bv.optInt("byteOffset", 0);
        int accOff = acc.optInt("byteOffset", 0);
        int start  = bvOff + accOff;
        return ByteBuffer.wrap(bin, start, bin.length - start).order(ByteOrder.LITTLE_ENDIAN);
    }

    // glTF 二进制 -> mmesh（只取第一个 mesh 的第一个 primitive）
    private byte[] glbToMmesh(byte[] d) {
        try {
            ByteBuffer bb = ByteBuffer.wrap(d).order(ByteOrder.LITTLE_ENDIAN);
            int total = bb.getInt(8);
            int off = 12;
            JSONObject js = null;
            byte[] bin = null;
            while (off + 8 <= d.length && off < total) {
                int clen = bb.getInt(off);
                int ctype = bb.getInt(off + 4);
                if (clen < 0 || off + 8 + clen > d.length) break;
                if (ctype == 0x4E4F534A) {
                    js = new JSONObject(new String(d, off + 8, clen, "UTF-8"));
                } else if (ctype == 0x004E4942) {
                    bin = Arrays.copyOfRange(d, off + 8, off + 8 + clen);
                }
                off += 8 + clen;
            }
            if (js == null || bin == null) return null;

            JSONArray meshes = js.getJSONArray("meshes");
            JSONObject prim = meshes.getJSONObject(0).getJSONArray("primitives").getJSONObject(0);
            JSONObject attrs = prim.getJSONObject("attributes");
            JSONArray accs = js.getJSONArray("accessors");
            JSONArray bvs  = js.getJSONArray("bufferViews");

            JSONObject posAcc = accs.getJSONObject(attrs.getInt("POSITION"));
            int nv = posAcc.getInt("count");
            float[] pos = new float[nv * 3];
            ByteBuffer pb = viewOf(bin, bvs, posAcc);
            for (int i = 0; i < nv * 3; i++) pos[i] = pb.getFloat();

            float[] nrm = new float[nv * 3];
            if (attrs.has("NORMAL")) {
                ByteBuffer nb = viewOf(bin, bvs, accs.getJSONObject(attrs.getInt("NORMAL")));
                for (int i = 0; i < nv * 3; i++) nrm[i] = nb.getFloat();
            }

            float[] uv = new float[nv * 2];
            if (attrs.has("TEXCOORD_0")) {
                ByteBuffer ub = viewOf(bin, bvs, accs.getJSONObject(attrs.getInt("TEXCOORD_0")));
                for (int i = 0; i < nv * 2; i++) uv[i] = ub.getFloat();
            }

            int ni = 0;
            int[] idx = new int[0];
            if (prim.has("indices")) {
                JSONObject ia = accs.getJSONObject(prim.getInt("indices"));
                ni = ia.getInt("count");
                int ct = ia.getInt("componentType");
                ByteBuffer ib = viewOf(bin, bvs, ia);
                idx = new int[ni];
                for (int i = 0; i < ni; i++) {
                    if (ct == 5125)      idx[i] = ib.getInt();
                    else if (ct == 5123) idx[i] = ib.getShort() & 0xFFFF;
                    else                 idx[i] = ib.get() & 0xFF;
                }
            } else {
                ni = nv;
                idx = new int[ni];
                for (int i = 0; i < ni; i++) idx[i] = i;
            }

            ByteArrayOutputStream o = new ByteArrayOutputStream(16 + nv * 32 + ni * 4);
            o.write(new byte[]{'M','M','S','H'});
            wInt(o, nv); wInt(o, ni); wInt(o, 7);
            for (float f : pos) wFloat(o, f);
            for (float f : nrm) wFloat(o, f);
            for (float f : uv)  wFloat(o, f);
            for (int v : idx)   wInt(o, v);
            return o.toByteArray();
        } catch (Throwable t) {
            XposedBridge.log(TAG + "glb parse fail: " + t);
            return null;
        }
    }
}
