package de.robv.android.xposed.callbacks;

import android.content.pm.ApplicationInfo;

public class XC_LoadPackage extends XCallback {

    public XC_LoadPackage() {
    }

    public XC_LoadPackage(int priority) {
        super(priority);
    }

    public static class LoadPackageParam extends XCallback.Param {
        public String packageName;
        public String processName;
        public ClassLoader classLoader;
        public ApplicationInfo appInfo;
        public boolean isFirstApplication;
    }
}
