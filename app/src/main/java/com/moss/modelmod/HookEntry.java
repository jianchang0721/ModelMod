package com.moss.modelmod;

import android.app.Application;
import android.content.Context;
import android.content.pm.ApplicationInfo;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

public class HookEntry implements IXposedHookLoadPackage {

    private static final String TAG    = "[ModelMod] ";
    private static final String TARGET = "com.hermes.h1game";
    private static final String SELF   = "com.moss.modelmod";

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam lp) {
        if (!TARGET.equals(lp.packageName)) return;

        XposedBridge.log(TAG + "enter process: " + lp.processName);

        XposedHelpers.findAndHookMethod(
            Application.class, "attach", Context.class,
            new XC_MethodHook() {
                @Override
                protected void afterHookedMethod(MethodHookParam param) {
                    Context ctx = (Context) param.args[0];
                    if (ctx == null) return;
                    loadNative(ctx);
                }
            });
    }

    private void loadNative(Context ctx) {
        try {
            ApplicationInfo ai = ctx.getPackageManager()
                    .getApplicationInfo(SELF, 0);
            String so = ai.nativeLibraryDir + "/libmodelmod.so";
            System.load(so);
            XposedBridge.log(TAG + "native loaded: " + so);
        } catch (Throwable t) {
            XposedBridge.log(TAG + "native load failed: " + t);
        }
    }
}
