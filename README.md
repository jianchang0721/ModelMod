# ModelMod

LSPosed 模块：给 Unity IL2CPP 游戏（com.hermes.h1game / Unity 2018.4.13f1）
运行时替换 SkinnedMeshRenderer 的模型。

## 重要：这里只有源码

APK 必须在你自己的机器上编译，没有现成产物。三条路，按省事程度排：

### 路线 1：GitHub Actions（最省事，手机就能完成）

1. 新建一个 GitHub 仓库，把本目录（含 .github/）整个传上去
2. 仓库 Actions 页面会自动跑 Build ModelMod
3. 跑完在 Artifacts 里下载 ModelMod-apk.zip，解压出 app-release.apk

不用装 Android Studio，不用配 NDK。首次构建约 8 分钟。

### 路线 2：电脑上 Android Studio

1. 装 Android Studio（自带 JDK 17）
2. 装 NDK 25.2.9519653 和 CMake 3.22.1（SDK Manager -> SDK Tools 里勾）
3. 打开本目录（Open an Existing Project，选 ModelMod 根目录）
4. 等 Gradle Sync 完，菜单 Build -> Build Bundle(s) / APK(s) -> Build APK(s)
5. 产物：app/build/outputs/apk/release/app-release.apk

### 路线 3：命令行

需要 JDK 17 + Android SDK + NDK 25 + CMake 3.22：

```bash
cd ModelMod
gradle :app:assembleRelease
# 产物：app/build/outputs/apk/release/app-release.apk
```

## 安装

```bash
adb install -r app-release.apk
```

LSPosed 里启用 ModelMod，作用域勾选 航海王热血航线。

## 使用

### 第一步：拿角色指纹（MOD_MODE = 0，默认就是）

装好后直接起游戏，进到有角色的界面，然后：

```bash
adb logcat -s ModelMod | grep SMR
```

会看到：

```
[ModelMod] [SMR 0] path=HeroRoot/hero_11001/body | bone0=Bip001 | mat0=11001_body | v=4821 sub=1
[ModelMod] [SMR 1] path=HeroRoot/hero_11001/face | bone0=Bip001 | mat0=11001_face | v=1024 sub=1
```

### 第二步：填映射表

改 app/src/main/cpp/mod.cpp 里的 g_rules：

```cpp
static CharRule g_rules[] = {
    { "11001", "/data/local/tmp/modelmod/11001_body.ab", "body" },
    { "11001", "/data/local/tmp/modelmod/11001_face.ab", "face" },
};
```

同时把顶部的 `#define MOD_MODE 0` 改成 `1`，重编。

### 第三步：准备模型 bundle

用 Unity 2018.4.13f1 打包（版本必须精确一致），
BuildTarget 选 Android，压缩选 ChunkBasedCompression。

```bash
adb shell mkdir -p /data/local/tmp/modelmod
adb push 11001_body.ab /data/local/tmp/modelmod/
adb shell chmod -R 755 /data/local/tmp/modelmod
```

## 日志关键字

| 日志 | 含义 |
|---|---|
| `libil2cpp.so not found` | 游戏还没加载完引擎，或者加固拦了 |
| `domain timeout` | il2cpp 初始化失败 |
| `SkinnedMeshRenderer not found` | UnityEngine.CoreModule 还没就位 |
| `bundle load failed` | bundle 路径不对或 Unity 版本不匹配 |
| `swapped ... -> key=...` | 替换成功 |

## 加固对抗

游戏有 libtoyger / libsecuritydevice / libPxlwCheckIris。被拦时按顺序试：

1. LSPosed 开启「隐藏模块」
2. 改 so 名字（libmodelmod.so -> libmonochrome.so，同时改 build.gradle 的 target 名）
3. 延迟加载：把 System.load 挪到 Activity.onCreate 之后 3 秒
4. 换 Zygisk 模式 + Shamiko
