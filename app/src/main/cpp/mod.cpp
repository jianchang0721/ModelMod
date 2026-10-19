#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <time.h>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <android/log.h>

#include "il2cpp_api.h"

#define TAG "ModelMod"

static void flog(const char* fmt, ...);

#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__); flog(__VA_ARGS__); } while (0)
#define LOGE(...) do { __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__); flog(__VA_ARGS__); } while (0)

// ================= 模式开关 =================
// 0 = 只打印角色指纹（先跑这个，拿清单）
// 1 = 执行精确替换（拿到清单后改这个，重编）
#ifndef MOD_MODE
#define MOD_MODE 1
#endif

// ================= 角色映射表 =================
// 填法：日志里看到 [SMR 0] path=HeroRoot/hero_11001/body bone0=Bip001 mat0=11001_body
//      key 填 "11001"，bundle 填对应文件路径，asset 填 bundle 内的资源名
struct CharRule {
    const char* id;      // 角色 ID，匹配 "/<id>_" 或 "/<id>/"，空串 = 全部
    const char* part;    // 部位关键字，nullptr/空 = 全部
    const char* bundle;  // nullptr = 用立方体兜底
    const char* asset;
};

static CharRule g_rules[] = {
    // 测试：所有角色的 Body 换立方体
    { "", "Body", nullptr, nullptr },
    // 填真模型时这样写：
    // { "11001", "Body", "/data/local/tmp/modelmod/11001_body.ab", "body" },
};
static const int RULE_COUNT = (int)(sizeof(g_rules) / sizeof(g_rules[0]));

Il2CppApi g_api{};
static JavaVM* g_vm = nullptr;
static Il2CppDomain* g_domain = nullptr;

static thread_local pid_t t_tid = 0;
static pid_t cur_tid() {
    if (!t_tid) t_tid = (pid_t)syscall(__NR_gettid);
    return t_tid;
}

// UnityMain 线程的 tid 不等于 pid！只能靠线程名认
static bool is_unity_main() {
    char name[16] = {0};
    if (prctl(PR_GET_NAME, name) != 0) return false;
    return strncmp(name, "UnityMain", 9) == 0;
}
static std::string g_log_path;
static FILE* g_log_fp = nullptr;
static std::map<std::string, Il2CppObject*> g_mesh_cache;
static bool g_inplace_mode = false;
static const MethodInfo* g_setEnabled = nullptr;
static std::map<void*, int> g_patched_meshes;

// worker 线程只读扫描 → 队列 → 主线程执行修改
struct PendingItem {
    Il2CppObject* smr;
    std::string chain;
};
static std::vector<PendingItem> g_pending;
static pthread_mutex_t g_pending_mtx = PTHREAD_MUTEX_INITIALIZER;

// 日志同时落盘，路径由 Java 层 getFilesDir() 传过来
static FILE* log_fp() {
    if (g_log_fp) return g_log_fp;
    if (!g_log_path.empty()) {
        g_log_fp = fopen(g_log_path.c_str(), "a");
        if (g_log_fp) return g_log_fp;
    }
    return nullptr;
}

static void flog(const char* fmt, ...) {
    FILE* fp = log_fp();
    if (!fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fflush(fp);
}

// ================= 基础工具 =================
static Il2CppClass* find_class_any(const char* ns, const char* name) {
    size_t n = 0;
    const Il2CppAssembly** asms = g_api.domain_get_assemblies(g_domain, &n);
    if (!asms) return nullptr;
    for (size_t i = 0; i < n; i++) {
        Il2CppImage* img = g_api.assembly_get_image(asms[i]);
        if (!img) continue;
        Il2CppClass* k = g_api.class_from_name(img, ns, name);
        if (k) return k;
    }
    return nullptr;
}

// 沿继承链往上找：il2cpp_class_get_methods 只返回该类自己声明的方法，
// 不包含继承的。get_name 在 UnityEngine.Object 上，GameObject 自己是没有的。
static const MethodInfo* find_method(Il2CppClass* klass, const char* name, int argc) {
    if (!klass) return nullptr;
    for (Il2CppClass* k = klass; k; k = g_api.class_get_parent(k)) {
        void* iter = nullptr;
        const MethodInfo* m;
        while ((m = g_api.class_get_methods(k, &iter))) {
            if (strcmp(g_api.method_get_name(m), name) != 0) continue;
            if ((int)g_api.method_get_param_count(m) != argc) continue;
            return m;
        }
    }
    return nullptr;
}

static void dump_class_methods(Il2CppClass* klass) {
    int total = 0;
    for (Il2CppClass* k = klass; k; k = g_api.class_get_parent(k)) {
        LOGI("--- class %s ---", g_api.class_get_name(k));
        void* iter = nullptr;
        const MethodInfo* m;
        int n = 0;
        while ((m = g_api.class_get_methods(k, &iter))) {
            const char* nm = g_api.method_get_name(m);
            LOGI("  %s/%u", nm ? nm : "?", g_api.method_get_param_count(m));
            if (++n > 80) break;
        }
        total += n;
    }
    LOGI("total methods dumped: %d", total);
}

static Il2CppObject* invoke_static(const MethodInfo* m, void** args) {
    if (!m) return nullptr;
    Il2CppException* exc = nullptr;
    Il2CppObject* r = g_api.runtime_invoke(m, nullptr, args, &exc);
    if (exc) { LOGE("invoke static exception %p", exc); return nullptr; }
    return r;
}

static Il2CppObject* invoke_inst(const MethodInfo* m, Il2CppObject* self, void** args) {
    if (!m) return nullptr;
    Il2CppException* exc = nullptr;
    Il2CppObject* r = g_api.runtime_invoke(m, self, args, &exc);
    if (exc) { LOGE("invoke instance exception %p", exc); return nullptr; }
    return r;
}

static int32_t unbox_i32(Il2CppObject* boxed) {
    if (!boxed) return 0;
    return *(int32_t*)((char*)boxed + 16);
}

static std::string to_utf8(Il2CppString* s) {
    if (!s) return "";
    int32_t len = *(int32_t*)((char*)s + 16);
    const uint16_t* p = (const uint16_t*)((char*)s + 20);
    std::string out;
    out.reserve(len);
    for (int32_t i = 0; i < len; i++) {
        uint32_t c = p[i];
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

static std::string obj_name(Il2CppObject* o) {
    if (!o) return "";
    Il2CppClass* k = g_api.object_get_class(o);
    const MethodInfo* m = find_method(k, "get_name", 0);
    if (!m) return "";
    return to_utf8((Il2CppString*)invoke_inst(m, o, nullptr));
}

static bool contains(const std::string& s, const char* k) {
    return s.find(k) != std::string::npos;
}

// ================= 载入 libil2cpp =================
static std::string self_cmdline() {
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (!f) return "";
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return std::string(buf, n ? strnlen(buf, n) : 0);
}

static void* try_noload() {
    return dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
}

static int phdr_cb(struct dl_phdr_info* info, size_t, void* data) {
    if (!info->dlpi_name || !info->dlpi_name[0]) return 0;
    if (strstr(info->dlpi_name, "libil2cpp.so")) {
        *(void**)data = (void*)info->dlpi_addr;
        return 1;
    }
    return 0;
}

static void* try_phdr() {
    void* base = nullptr;
    dl_iterate_phdr(phdr_cb, &base);
    if (!base) return nullptr;
    return dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
}

static void* try_maps() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return nullptr;
    char line[512];
    char path[256] = {0};
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "libil2cpp.so")) {
            char* p = strchr(line, '/');
            if (p) {
                strncpy(path, p, sizeof(path) - 1);
                char* nl = strchr(path, '\n');
                if (nl) *nl = 0;
                break;
            }
        }
    }
    fclose(f);
    if (path[0]) return dlopen(path, RTLD_NOW);
    return nullptr;
}

static void dump_so_list() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) { LOGE("cannot open maps"); return; }
    char line[512];
    std::map<std::string, int> seen;
    while (fgets(line, sizeof(line), f)) {
        char* p = strchr(line, '/');
        if (!p) continue;
        std::string path(p);
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
        if (path.find(".so") == std::string::npos) continue;
        size_t slash = path.rfind('/');
        std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
        if (!seen.count(base)) { seen[base] = 1; LOGI("SO %s", base.c_str()); }
    }
    fclose(f);
    LOGI("SO count %zu", seen.size());
}

// 从 /proc/self/maps 拿 so 的加载基址（最低地址那一段）
static uintptr_t maps_base(const char* soname) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t addrs[64];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, soname)) continue;
        uintptr_t start = strtoull(line, nullptr, 16);
        if (n < 64) addrs[n++] = start;
    }
    fclose(f);
    uintptr_t lowest = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* p = (uint8_t*)addrs[i];
        if (p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') return addrs[i];
        if (!lowest || addrs[i] < lowest) lowest = addrs[i];
    }
    return lowest;
}

// 自己解 ELF 的 dynsym，不靠 linker
static uintptr_t elf_sym(uintptr_t base, const char* name) {
    if (!base) return 0;
    ElfW(Ehdr)* eh = (ElfW(Ehdr)*)base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return 0;

    uintptr_t dyn = 0;
    ElfW(Phdr)* ph = (ElfW(Phdr)*)(base + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) { dyn = base + ph[i].p_vaddr; break; }
    }
    if (!dyn) return 0;

    uintptr_t symtab = 0, strtab = 0, hash = 0, gnu = 0;
    for (ElfW(Dyn)* d = (ElfW(Dyn)*)dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab = d->d_un.d_ptr; break;
            case DT_STRTAB:   strtab = d->d_un.d_ptr; break;
            case DT_HASH:     hash   = d->d_un.d_ptr; break;
            case DT_GNU_HASH: gnu    = d->d_un.d_ptr; break;
            default: break;
        }
    }
    if (symtab < base) symtab += base;
    if (strtab < base) strtab += base;
    if (hash && hash < base) hash += base;
    if (gnu && gnu < base) gnu += base;
    if (!symtab || !strtab) return 0;

    uint32_t nsym = 0;
    if (hash) {
        nsym = ((uint32_t*)hash)[1];
    } else if (gnu) {
        uint32_t* g = (uint32_t*)gnu;
        uint32_t nbuckets = g[0], symoffset = g[1], bloom = g[2];
        uint32_t* buckets = &g[4 + bloom * 2];
        uint32_t* chain = &buckets[nbuckets];
        uint32_t last = 0;
        for (uint32_t i = 0; i < nbuckets; i++) if (buckets[i] > last) last = buckets[i];
        if (last >= symoffset) {
            while (!(chain[last - symoffset] & 1)) last++;
            nsym = last + 1;
        }
    }
    if (!nsym) return 0;
    if (nsym > 200000) nsym = 200000;   // 防越界，il2cpp 大约十几万个符号

    ElfW(Sym)* syms = (ElfW(Sym)*)symtab;
    const char* strs = (const char*)strtab;
    size_t nameLen = strlen(name);
    for (uint32_t i = 0; i < nsym; i++) {
        uint32_t off = syms[i].st_name;
        if (off == 0 || off > 0xFFFFFF) continue;
        const char* s = strs + off;
        // 自己逐字节比，不用 strcmp，避开 bionic fortify 的 __strcmp_chk
        size_t k = 0;
        while (k <= nameLen && s[k] == name[k]) k++;
        if (k == nameLen + 1) {
            return base + syms[i].st_value;
        }
    }
    return 0;
}

static uintptr_t wait_il2cpp() {
    for (int i = 0; ; i++) {
        uintptr_t base = maps_base("libil2cpp.so");
        if (base) {
            LOGI("libil2cpp base 0x%lx after %d tries", (unsigned long)base, i);
            return base;
        }
        if (i == 300) { LOGI("30s passed, dumping so list"); dump_so_list(); }
        usleep(100 * 1000);
    }
}

static bool bind_api(uintptr_t base) {
#define B(field, name) \
    g_api.field = (decltype(g_api.field))elf_sym(base, name); \
    if (!g_api.field) { LOGE("missing symbol: %s", name); return false; }

    B(domain_get,                "il2cpp_domain_get")
    B(domain_get_assemblies,     "il2cpp_domain_get_assemblies")
    B(thread_attach,             "il2cpp_thread_attach")
    B(assembly_get_image,        "il2cpp_assembly_get_image")
    B(image_get_name,            "il2cpp_image_get_name")
    B(image_get_class_count,     "il2cpp_image_get_class_count")
    B(image_get_class,           "il2cpp_image_get_class")
    B(class_from_name,           "il2cpp_class_from_name")
    B(class_get_name,            "il2cpp_class_get_name")
    B(class_get_namespace,      "il2cpp_class_get_namespace")
    B(class_get_parent,          "il2cpp_class_get_parent")
    B(class_is_assignable_from,  "il2cpp_class_is_assignable_from")
    B(class_get_type,            "il2cpp_class_get_type")
    B(class_get_methods,         "il2cpp_class_get_methods")
    B(method_get_name,           "il2cpp_method_get_name")
    B(method_get_param_count,    "il2cpp_method_get_param_count")
    B(method_get_param,          "il2cpp_method_get_param")
    B(class_from_type,           "il2cpp_class_from_type")
    B(runtime_invoke,            "il2cpp_runtime_invoke")
    B(string_new,                "il2cpp_string_new")
    B(object_new,                "il2cpp_object_new")
    B(object_get_class,          "il2cpp_object_get_class")
    B(array_new,                 "il2cpp_array_new")
    B(type_get_object,           "il2cpp_type_get_object")
#undef B
    return true;
}

// ================= 造一个立方体 Mesh =================
static Il2CppObject* build_cube_mesh() {
    Il2CppClass* MeshCls = find_class_any("UnityEngine", "Mesh");
    Il2CppClass* V3Cls   = find_class_any("UnityEngine", "Vector3");
    Il2CppClass* I32Cls  = find_class_any("System", "Int32");
    if (!MeshCls || !V3Cls || !I32Cls) {
        LOGE("class missing Mesh=%p V3=%p I32=%p", MeshCls, V3Cls, I32Cls);
        return nullptr;
    }

    Il2CppObject* mesh = g_api.object_new(MeshCls);
    const MethodInfo* ctor = find_method(MeshCls, ".ctor", 0);
    if (ctor) invoke_inst(ctor, mesh, nullptr);

    static const float CV[8][3] = {
        {-0.5f,-0.5f,-0.5f}, { 0.5f,-0.5f,-0.5f},
        { 0.5f, 0.5f,-0.5f}, {-0.5f, 0.5f,-0.5f},
        {-0.5f,-0.5f, 0.5f}, { 0.5f,-0.5f, 0.5f},
        { 0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f},
    };
    Il2CppArray* verts = g_api.array_new(V3Cls, 8);
    for (int i = 0; i < 8; i++) {
        float* p = (float*)array_addr(verts, 12, i);
        p[0] = CV[i][0]; p[1] = CV[i][1]; p[2] = CV[i][2];
    }

    static const int32_t TRI[36] = {
        0,1,2, 0,2,3,  4,6,5, 4,7,6,
        0,4,5, 0,5,1,  1,5,6, 1,6,2,
        2,6,7, 2,7,3,  3,7,4, 3,4,0,
    };
    Il2CppArray* tris = g_api.array_new(I32Cls, 36);
    memcpy(array_addr(tris, 4, 0), TRI, sizeof(TRI));

    const MethodInfo* setV = find_method(MeshCls, "set_vertices", 1);
    const MethodInfo* setT = find_method(MeshCls, "set_triangles", 1);
    if (setV) { void* a[1] = { verts }; invoke_inst(setV, mesh, a); }
    if (setT) { void* a[1] = { tris  }; invoke_inst(setT, mesh, a); }

    const MethodInfo* recalc = find_method(MeshCls, "RecalculateNormals", 0);
    if (recalc) invoke_inst(recalc, mesh, nullptr);

    LOGI("cube mesh built: %p", mesh);
    return mesh;
}

// ================= 从 AssetBundle 取 Mesh =================
static Il2CppObject* load_mesh_from_bundle(const char* path, const char* asset) {
    Il2CppClass* ABCls = find_class_any("UnityEngine", "AssetBundle");
    if (!ABCls) { LOGE("AssetBundle class not found"); return nullptr; }

    const MethodInfo* loadFile = find_method(ABCls, "LoadFromFile", 1);
    if (!loadFile) { LOGE("LoadFromFile(string) not found"); return nullptr; }

    void* a1[1] = { g_api.string_new(path) };
    Il2CppObject* bundle = invoke_static(loadFile, a1);
    if (!bundle) { LOGE("bundle load failed: %s", path); return nullptr; }
    LOGI("bundle loaded: %s", path);

    const MethodInfo* loadAsset = find_method(ABCls, "LoadAsset", 1);
    if (!loadAsset) return nullptr;

    void* a2[1] = { g_api.string_new(asset) };
    Il2CppObject* obj = invoke_inst(loadAsset, bundle, a2);
    if (!obj) { LOGE("asset not found: %s", asset); return nullptr; }
    LOGI("asset loaded: %s -> %p", asset, obj);

    Il2CppClass* GOCls  = find_class_any("UnityEngine", "GameObject");
    Il2CppClass* SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    Il2CppClass* objCls = g_api.object_get_class(obj);

    if (GOCls && SMRCls && objCls &&
        g_api.class_is_assignable_from(GOCls, objCls)) {
        const MethodInfo* getComp = find_method(GOCls, "GetComponent", 1);
        if (getComp) {
            void* a3[1] = { g_api.type_get_object(g_api.class_get_type(SMRCls)) };
            Il2CppObject* smr = invoke_inst(getComp, obj, a3);
            if (smr) {
                const MethodInfo* getMesh = find_method(SMRCls, "get_sharedMesh", 0);
                if (getMesh) {
                    Il2CppObject* m = invoke_inst(getMesh, smr, nullptr);
                    LOGI("mesh from prefab: %p", m);
                    return m;
                }
            }
        }
    }

    Il2CppClass* MeshCls = find_class_any("UnityEngine", "Mesh");
    if (MeshCls && objCls && g_api.class_is_assignable_from(MeshCls, objCls)) return obj;
    return nullptr;
}

// ================= 指纹结构 =================
struct Fingerprint {
    Il2CppObject* smr;
    std::string chain;
    std::string bone0;
    std::string mat0;
    int verts;
    int submesh;
};

static void collect_fingerprints(Il2CppClass* SMRCls, std::vector<Fingerprint>& out) {
    out.clear();

    Il2CppClass* ObjectCls = find_class_any("UnityEngine", "Object");
    Il2CppClass* CompCls   = find_class_any("UnityEngine", "Component");
    Il2CppClass* TransCls  = find_class_any("UnityEngine", "Transform");
    Il2CppClass* Renderer  = find_class_any("UnityEngine", "Renderer");
    if (!ObjectCls || !CompCls || !TransCls || !Renderer) return;

    const MethodInfo* find     = find_method(ObjectCls, "FindObjectsOfType", 1);
    const MethodInfo* getGO    = find_method(CompCls, "get_gameObject", 0);
    const MethodInfo* getTrans = find_method(CompCls, "get_transform", 0);
    const MethodInfo* getParent= find_method(TransCls, "get_parent", 0);
    const MethodInfo* getBones = find_method(SMRCls, "get_bones", 0);
    const MethodInfo* getMats  = find_method(Renderer, "get_sharedMaterials", 0);
    const MethodInfo* getMesh  = find_method(SMRCls, "get_sharedMesh", 0);
    if (!find || !getGO || !getTrans || !getParent || !getBones || !getMats || !getMesh) return;

    void* a[1] = { g_api.type_get_object(g_api.class_get_type(SMRCls)) };
    Il2CppArray* arr = (Il2CppArray*)invoke_static(find, a);

    static int dbgFind = 0;
    bool usedFallback = false;

    if (!arr || arr->max_length == 0) {
        // 备用：Resources.FindObjectsOfTypeAll，包含未激活对象
        Il2CppClass* ResCls = find_class_any("UnityEngine", "Resources");
        if (ResCls) {
            const MethodInfo* fa = find_method(ResCls, "FindObjectsOfTypeAll", 1);
            if (fa) {
                arr = (Il2CppArray*)invoke_static(fa, a);
                usedFallback = true;
            }
        }
    }

    if (dbgFind < 5) {
        dbgFind++;
        LOGI("find: arr=%p len=%ld fallback=%d", arr, arr ? (long)arr->max_length : -1L, (int)usedFallback);
    } else {
        static size_t lastLen = 0;
        size_t len = arr ? arr->max_length : 0;
        if (len != lastLen) {
            lastLen = len;
            LOGI("find: len changed %zu", len);
        }
    }
    if (!arr) return;

    for (size_t i = 0; i < arr->max_length; i++) {
        Il2CppObject* smr = *(Il2CppObject**)array_addr(arr, sizeof(void*), i);
        if (!smr) continue;

        Fingerprint fp;
        fp.smr = smr;

        Il2CppObject* go = invoke_inst(getGO, smr, nullptr);
        std::string chain = obj_name(go);
        Il2CppObject* tr = invoke_inst(getTrans, smr, nullptr);
        for (int d = 0; d < 5 && tr; d++) {
            Il2CppObject* p = invoke_inst(getParent, tr, nullptr);
            if (!p) break;
            std::string pn = obj_name(p);
            if (pn.empty()) break;
            chain = pn + "/" + chain;
            tr = p;
        }
        fp.chain = chain;

        fp.bone0 = "";
        Il2CppArray* bones = (Il2CppArray*)invoke_inst(getBones, smr, nullptr);
        if (bones && bones->max_length > 0)
            fp.bone0 = obj_name(*(Il2CppObject**)array_addr(bones, sizeof(void*), 0));

        fp.mat0 = "";
        Il2CppArray* mats = (Il2CppArray*)invoke_inst(getMats, smr, nullptr);
        if (mats && mats->max_length > 0)
            fp.mat0 = obj_name(*(Il2CppObject**)array_addr(mats, sizeof(void*), 0));

        fp.verts = 0;
        fp.submesh = 0;
        Il2CppObject* mesh = invoke_inst(getMesh, smr, nullptr);
        if (mesh) {
            Il2CppClass* mc = g_api.object_get_class(mesh);
            const MethodInfo* gv = find_method(mc, "get_vertexCount", 0);
            const MethodInfo* gs = find_method(mc, "get_subMeshCount", 0);
            if (gv) fp.verts = unbox_i32(invoke_inst(gv, mesh, nullptr));
            if (gs) fp.submesh = unbox_i32(invoke_inst(gs, mesh, nullptr));
        }

        out.push_back(fp);
    }
}

// ================= 模式 0：打印清单 =================
static void dump_fingerprints(Il2CppClass* SMRCls) {
    std::vector<Fingerprint> list;
    collect_fingerprints(SMRCls, list);
    LOGI("========== %zu SkinnedMeshRenderer ==========", list.size());
    for (size_t i = 0; i < list.size(); i++) {
        const Fingerprint& f = list[i];
        LOGI("[SMR %zu] path=%s | bone0=%s | mat0=%s | v=%d sub=%d",
             i, f.chain.c_str(), f.bone0.c_str(), f.mat0.c_str(), f.verts, f.submesh);
    }
}

// ================= 模式 1：精确替换 =================
static Il2CppObject* mesh_for_rule(CharRule& r) {
    std::string key = r.id ? r.id : "";
    auto it = g_mesh_cache.find(key);
    if (it != g_mesh_cache.end()) return it->second;

    Il2CppObject* m = nullptr;
    if (r.bundle) m = load_mesh_from_bundle(r.bundle, r.asset);
    if (!m) m = build_cube_mesh();

    if (m) {
        g_mesh_cache[key] = m;
        LOGI("rule ready: [%s] -> %p", key.c_str(), m);
    }
    return m;
}

// 路径里 /<id>_ 或 /<id>/ 才算命中，避免 11001 误伤 110011
static bool path_has_id(const std::string& chain, const char* id) {
    if (!id || !id[0]) return true;
    std::string k = std::string("/") + id;
    size_t pos = 0;
    while ((pos = chain.find(k, pos)) != std::string::npos) {
        size_t after = pos + k.size();
        if (after >= chain.size()) return true;
        char c = chain[after];
        if (c == '_' || c == '/' || c == '-') return true;
        pos = after;
    }
    return false;
}

static bool rule_matches(const CharRule& r, const Fingerprint& f) {
    if (!path_has_id(f.chain, r.id)) return false;
    if (r.part && r.part[0] && !contains(f.chain, r.part)) return false;
    return true;
}

// 直接改 Mesh 的顶点数组（不换 Mesh 对象）
static int scale_mesh_vertices(Il2CppObject* mesh, Il2CppClass* MeshCls, float k) {
    const MethodInfo* getVerts = find_method(MeshCls, "get_vertices", 0);
    const MethodInfo* setVerts = find_method(MeshCls, "set_vertices", 1);
    if (!getVerts || !setVerts) {
        LOGE("get_vertices=%p set_vertices=%p", getVerts, setVerts);
        return 0;
    }
    Il2CppException* exc = nullptr;
    Il2CppArray* arr = (Il2CppArray*)g_api.runtime_invoke(getVerts, mesh, nullptr, &exc);
    if (exc) { LOGE("get_vertices threw %p", exc); return 0; }
    if (!arr) { LOGE("get_vertices returned null"); return 0; }
    if (arr->max_length == 0) { LOGE("vertex array empty"); return 0; }
    for (size_t i = 0; i < arr->max_length; i++) {
        float* p = (float*)array_addr(arr, 12, i);
        p[0] *= k; p[1] *= k; p[2] *= k;
    }
    void* a[1] = { arr };
    invoke_inst(setVerts, mesh, a);
    return (int)arr->max_length;
}

// 造一个立方体 GameObject 并挂到目标 transform 的父级下
static Il2CppObject* spawn_cube_for(Il2CppObject* smr) {
    Il2CppClass* GOCls    = find_class_any("UnityEngine", "GameObject");
    Il2CppClass* CompCls  = find_class_any("UnityEngine", "Component");
    Il2CppClass* TransCls = find_class_any("UnityEngine", "Transform");
    if (!GOCls || !CompCls || !TransCls) { LOGE("spawn: class missing"); return nullptr; }

    const MethodInfo* createPrim = find_method(GOCls, "CreatePrimitive", 1);
    if (!createPrim) { LOGE("spawn: CreatePrimitive missing"); return nullptr; }
    LOGI("cube: CreatePrimitive=%p", (void*)createPrim);

    int32_t cubeType = 3;   // PrimitiveType.Cube
    void* a1[1] = { &cubeType };
    Il2CppObject* cube = invoke_static(createPrim, a1);
    LOGI("cube: created=%p", cube);
    if (!cube) { LOGE("spawn: CreatePrimitive null"); return nullptr; }

    const MethodInfo* getTransGO = find_method(GOCls, "get_transform", 0);
    const MethodInfo* getTransC  = find_method(CompCls, "get_transform", 0);
    const MethodInfo* getParent  = find_method(TransCls, "get_parent", 0);
    const MethodInfo* setParent  = find_method(TransCls, "SetParent", 1);

    Il2CppObject* ct = invoke_inst(getTransGO, cube, nullptr);
    Il2CppObject* st = invoke_inst(getTransC, smr, nullptr);
    Il2CppObject* sp = invoke_inst(getParent, st, nullptr);

    if (ct && sp && setParent) {
        void* a2[1] = { sp };
        invoke_inst(setParent, ct, a2);
    }
    return cube;
}

static size_t g_last_scan = 0;

static int patch_scene_targeted(Il2CppClass* SMRCls, const MethodInfo* setMesh, int budget) {
    std::vector<Fingerprint> list;
    collect_fingerprints(SMRCls, list);
    g_last_scan = list.size();

    Il2CppClass* MeshCls = find_class_any("UnityEngine", "Mesh");
    const MethodInfo* getMesh = find_method(SMRCls, "get_sharedMesh", 0);

    static bool scanLogged = false;
    if (!scanLogged) {
        scanLogged = true;
        LOGI("scan: smrs=%zu inplace=%d MeshCls=%p getMesh=%p rules=%d",
             list.size(), (int)g_inplace_mode, MeshCls, getMesh, RULE_COUNT);
    }

    int done = 0;
    for (size_t i = 0; i < list.size() && done < budget; i++) {
        Fingerprint& f = list[i];
        for (int r = 0; r < RULE_COUNT; r++) {
            CharRule& rule = g_rules[r];
            if (!rule_matches(rule, f)) continue;

            if (g_inplace_mode) {
                if (g_patched_meshes.count(f.smr)) break;

                // 1. 关掉原 renderer
                if (g_setEnabled) {
                    uint8_t off = 0;
                    void* a[1] = { &off };
                    invoke_inst(g_setEnabled, f.smr, a);
                }

                // 2. 造立方体挂到同一父级
                Il2CppObject* cube = spawn_cube_for(f.smr);

                g_patched_meshes[f.smr] = 1;
                LOGI("replaced %s cube=%p", f.chain.c_str(), cube);
                done++;
                break;
            }

            Il2CppObject* want = mesh_for_rule(rule);
            if (!want) break;

            Il2CppObject* cur = invoke_inst(getMesh, f.smr, nullptr);
            if (cur == want) break;

            void* a[1] = { want };
            invoke_inst(setMesh, f.smr, a);
            LOGI("swapped %s", f.chain.c_str());
            done++;
            break;
        }
    }
    return done;
}

// ================= 主线程 hook =================
static void* g_orig_delta = nullptr;
static void* g_orig_invoke = nullptr;
static void* g_orig_width = nullptr;
static void* g_orig_framecount = nullptr;
static int g_tick = 0;
static bool g_tick_inited = false;
static Il2CppClass* g_smrCls = nullptr;
static const MethodInfo* g_setMeshMI = nullptr;
static uintptr_t g_il2cpp_base = 0;
static volatile int g_tick_busy = 0;
static int g_tick_mode = 0;   // 1=deltaTime 2=time 3=runtime_invoke

static long scan_count(Il2CppClass* cls) {
    Il2CppClass* ObjCls = find_class_any("UnityEngine", "Object");
    if (!ObjCls) return -100;
    const MethodInfo* f = find_method(ObjCls, "FindObjectsOfType", 1);
    if (!f) return -101;
    void* a[1] = { g_api.type_get_object(g_api.class_get_type(cls)) };
    Il2CppArray* arr = (Il2CppArray*)invoke_static(f, a);
    return arr ? (long)arr->max_length : -102;
}

// 扫描全部 assembly/class，把名字带关键词的类全 dump 出来
static void dump_interesting_classes() {
    const char* keys[] = {"Model", "Char", "Hero", "Role", "Skin", "Avatar", "Fashion", "Dress", "Part"};
    size_t n = 0;
    const Il2CppAssembly** asms = g_api.domain_get_assemblies(g_domain, &n);
    if (!asms) { LOGE("dump: no assemblies"); return; }

    LOGI("dump: %zu assemblies", n);
    int found = 0;
    for (size_t i = 0; i < n; i++) {
        Il2CppImage* img = g_api.assembly_get_image(asms[i]);
        if (!img) continue;
        size_t cn = g_api.image_get_class_count(img);
        if (cn == 0) continue;
        const char* imgName = g_api.image_get_name(img);
        LOGI("dump: img %s classes=%zu", imgName ? imgName : "?", cn);
        for (size_t j = 0; j < cn && found < 400; j++) {
            Il2CppClass* c = g_api.image_get_class(img, j);
            if (!c) continue;
            const char* nm = g_api.class_get_name(c);
            if (!nm) continue;
            bool hit = false;
            for (int k = 0; k < 9; k++) if (strstr(nm, keys[k])) { hit = true; break; }
            if (!hit) continue;
            const char* ns = g_api.class_get_namespace(c);
            LOGI("CLASS %s.%s", ns ? ns : "", nm);
            found++;
        }
    }
    LOGI("dump: interesting=%d", found);
}

// Unity PlayerLoop 里的每帧钩子候选
static void probe_playerloop() {
    const char* names[] = {"UpdateAllSkinnedMeshes", "ParticleSystemBeginUpdateAll", "ParticleSystemEndUpdateAll"};
    size_t n = 0;
    const Il2CppAssembly** asms = g_api.domain_get_assemblies(g_domain, &n);
    if (!asms) return;
    for (size_t i = 0; i < n; i++) {
        Il2CppImage* img = g_api.assembly_get_image(asms[i]);
        if (!img) continue;
        size_t cn = g_api.image_get_class_count(img);
        for (size_t j = 0; j < cn; j++) {
            Il2CppClass* c = g_api.image_get_class(img, j);
            if (!c) continue;
            const char* nm = g_api.class_get_name(c);
            if (!nm) continue;
            for (int k = 0; k < 3; k++) {
                if (strcmp(nm, names[k]) != 0) continue;
                LOGI("FOUND %s in %s", nm, g_api.image_get_name(img));
                void* iter = nullptr;
                const MethodInfo* m;
                int cnt = 0;
                while ((m = g_api.class_get_methods(c, &iter)) && cnt < 20) {
                    LOGI("  M %s/%u", g_api.method_get_name(m), g_api.method_get_param_count(m));
                    cnt++;
                }
            }
        }
    }
}

static void probe_types() {
    const char* names[] = {"GameObject", "Transform", "MonoBehaviour", "Camera",
                           "SkinnedMeshRenderer", "MeshRenderer", "Animator"};
    for (int i = 0; i < 7; i++) {
        Il2CppClass* c = find_class_any("UnityEngine", names[i]);
        if (!c) { LOGI("probe %-20s: class null", names[i]); continue; }
        LOGI("probe %-20s: len=%ld", names[i], scan_count(c));
    }
}

static int g_cube_done = 0;
static long g_lastGO = -1;

// Instantiate hook：在主线程创建对象时插一脚
static void main_tick();
static void* g_orig_canvas = nullptr;
static void* g_orig_inst1 = nullptr;
static void* g_orig_inst2 = nullptr;
static int g_inst_hits = 0;
static std::vector<Il2CppObject*> g_smr_queue;
static pthread_mutex_t g_qmtx = PTHREAD_MUTEX_INITIALIZER;
static void try_patch_in_main();
static long g_lastSMR = -1;

static void hk_canvasupdate(void* m) {
    main_tick();
    ((void(*)(void*))g_orig_canvas)(m);
}

static void* g_orig_setactive = nullptr;
static void* g_orig_getname = nullptr;
static void* g_orig_gettrans = nullptr;

static Il2CppString* hk_getname(void* self, void* m) {
    main_tick();
    return ((Il2CppString*(*)(void*, void*))g_orig_getname)(self, m);
}

static Il2CppObject* hk_gettrans(void* self, void* m) {
    main_tick();
    return ((Il2CppObject*(*)(void*, void*))g_orig_gettrans)(self, m);
}

static void hk_setactive(void* self, void* m, uint8_t v) {
    static int cnt = 0;
    if (cnt < 10) {
        cnt++;
        char nm[16] = {0};
        prctl(PR_GET_NAME, nm);
        LOGI("setactive#%d tid=%d name=%s", cnt, (int)cur_tid(), nm);
    }
    main_tick();
    ((void(*)(void*, void*, uint8_t))g_orig_setactive)(self, m, v);
}

static void try_patch_in_main() {
    std::vector<Il2CppObject*> q;
    pthread_mutex_lock(&g_qmtx);
    q.swap(g_smr_queue);
    pthread_mutex_unlock(&g_qmtx);

    if (q.empty()) return;
    LOGI("main: enter tid=%d isMain=%d queue=%zu",
         (int)cur_tid(), (int)(cur_tid() == getpid()), q.size());

    int done = 0;
    for (auto smr : q) {
        if (g_patched_meshes.count(smr)) continue;
        if (done >= 3) break;

        if (g_setEnabled) {
            uint8_t off = 0;
            void* a[1] = { &off };
            invoke_inst(g_setEnabled, smr, a);
        }
        LOGI("main: hidden smr=%p", smr);

        Il2CppObject* cb = spawn_cube_for(smr);
        LOGI("main: cube=%p", cb);
        g_patched_meshes[smr] = 1;
        done++;
    }
    LOGI("main: done=%d", done);
}

static Il2CppObject* hk_inst1(void* m, Il2CppObject* orig) {
    Il2CppObject* r = ((Il2CppObject*(*)(void*, Il2CppObject*))g_orig_inst1)(m, orig);
    g_inst_hits++;
    if (g_inst_hits < 20) {
        LOGI("inst1 #%d tid=%d main=%d orig=%p ret=%p",
             g_inst_hits, (int)cur_tid(), (int)(cur_tid() == getpid()), orig, r);
    }
    if ((g_inst_hits % 5) == 0) try_patch_in_main();
    return r;
}

static Il2CppObject* hk_inst2(void* m, Il2CppObject* orig, Il2CppObject* parent) {
    Il2CppObject* r = ((Il2CppObject*(*)(void*, Il2CppObject*, Il2CppObject*))g_orig_inst2)(m, orig, parent);
    if (g_inst_hits < 20) { g_inst_hits++; LOGI("inst2 #%d orig=%p ret=%p", g_inst_hits, orig, r); }
    if (g_inst_hits == 5) try_patch_in_main();
    return r;
}

static void do_patch_tick() {
    if (!g_tick_inited) {
        g_tick_inited = true;
        g_smrCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
        LOGI("tick init: smr=%p setEnabled=%p", g_smrCls, g_setEnabled);
        probe_types();
    }
    if (!g_smrCls) return;

    // 场景变化监控：GO / SMR 数量一变就报
    {
        static int probeTick = 0;
        if ((probeTick++ % 20) == 0) {
            Il2CppClass* GOCls = find_class_any("UnityEngine", "GameObject");
            long go = GOCls ? scan_count(GOCls) : -200;
            long smr = scan_count(g_smrCls);
            if (go != g_lastGO || smr != g_lastSMR) {
                g_lastGO = go;
                g_lastSMR = smr;
                LOGI("scene: GO=%ld SMR=%ld", go, smr);
            }
        }
    }

    std::vector<Fingerprint> list;
    collect_fingerprints(g_smrCls, list);
    g_last_scan = list.size();

    if (list.empty()) return;

    // 不管 worker 是否已隐藏，前 3 个匹配的 SMR 直接造 cube（当前主线，安全）
    int done = 0;
    for (auto& f : list) {
        if (done >= 1 || g_cube_done >= 3) break;

        for (int r = 0; r < RULE_COUNT; r++) {
            if (!rule_matches(g_rules[r], f)) continue;

            g_cube_done++;
            LOGI("cube: trying %s", f.chain.c_str());
            Il2CppObject* cb = spawn_cube_for(f.smr);
            LOGI("cube: result=%p", cb);
            done++;
            break;
        }
    }
    if (done > 0) LOGI("tick cube done=%d total=%zu", done, g_last_scan);
}

// 节流 + 防重入 + 只主线程执行
static void main_tick() {
    pid_t tid = cur_tid();
    bool isMain = is_unity_main();

    static int tlog = 0;
    static int tmain = 0;
    if (isMain && tmain < 30) {
        tmain++;
        LOGI("tick MAIN #%d tid=%d cnt=%d", tmain, (int)tid, g_tick);
    }
    if (!isMain && (tlog++ % 500) == 0) {
        char nm[16] = {0};
        prctl(PR_GET_NAME, nm);
        LOGI("tick sub tid=%d name=%s cnt=%d", (int)tid, nm, g_tick);
    }

    if (!isMain) return;      // 非主线程不算数
    if (__sync_lock_test_and_set(&g_tick_busy, 1)) return;

    // 按时间节流，不受调用次数影响
    static int cnt = 0;
    if ((cnt++ % 20) == 0) do_patch_tick();
    __sync_lock_release(&g_tick_busy);
}

// get_deltaTime 是静态方法，AOT 签名 (MethodInfo* method)
static float hk_delta(void* method) {
    main_tick();
    return ((float(*)(void*))g_orig_delta)(method);
}

static int hk_width(void* method) {
    main_tick();
    return ((int(*)(void*))g_orig_width)(method);
}

static int hk_framecount(void* method) {
    main_tick();
    return ((int(*)(void*))g_orig_framecount)(method);
}

// ---- 实例方法 hook：AOT 签名 (void* self, MethodInfo* m) ----
struct Vec3 { float x, y, z; };

static void* g_orig_name = nullptr;
static void* g_orig_gettrans = nullptr;
static void* g_orig_getpos = nullptr;

static Il2CppString* hk_name(void* self, void* m) {
    main_tick();
    return ((Il2CppString*(*)(void*, void*))g_orig_name)(self, m);
}

static Il2CppObject* hk_gettrans(void* self, void* m) {
    main_tick();
    return ((Il2CppObject*(*)(void*, void*))g_orig_gettrans)(self, m);
}

static Vec3 hk_getpos(void* self, void* m) {
    main_tick();
    return ((Vec3(*)(void*, void*))g_orig_getpos)(self, m);
}

// 观察器：把游戏反射调用的名字里带关键字的全记下来
static std::set<std::string> g_seen;
static int g_obs_scan = 0;
static int g_obs_hit = 0;

static void observe(const MethodInfo* m) {
    if (!m || ++g_obs_scan > 300000 || g_obs_hit > 400) return;
    const char* n = g_api.method_get_name(m);
    if (!n) return;
    if (!strstr(n, "Mesh") && !strstr(n, "Model") && !strstr(n, "Skin") &&
        !strstr(n, "Load") && !strstr(n, "Create") && !strstr(n, "Render") &&
        !strstr(n, "Asset") && !strstr(n, "Bundle") && !strstr(n, "Char") &&
        !strstr(n, "Hero") && !strstr(n, "Avatar"))
        return;
    std::string s(n);
    if (g_seen.count(s)) return;
    g_seen.insert(s);
    g_obs_hit++;
    LOGI("OBS %s", n);
}

static Il2CppObject* hk_invoke(const MethodInfo* m, void* obj, void** args, Il2CppException** exc) {
    observe(m);
    main_tick();
    return ((Il2CppObject*(*)(const MethodInfo*, void*, void**, Il2CppException**))g_orig_invoke)(m, obj, args, exc);
}

static void* load_so_by_name(const char* soname) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return nullptr;
    char line[512];
    char path[256] = {0};
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, soname)) continue;
        char* p = strchr(line, '/');
        if (!p) continue;
        strncpy(path, p, sizeof(path) - 1);
        char* nl = strchr(path, '\n');
        if (nl) *nl = 0;
        break;
    }
    fclose(f);
    if (!path[0]) return nullptr;
    return dlopen(path, RTLD_NOW);
}

static void* try_hook(void* sh, void* code, void* handler, const char* label) {
    typedef void* (*fn_hook)(void*, void*, void**);
    fn_hook h = (fn_hook)dlsym(sh, "shadowhook_hook_func_addr");
    if (!h) { LOGE("hook sym missing"); return nullptr; }
    void* orig = nullptr;
    void* stub = h(code, handler, &orig);
    LOGI("hook %s: code=%p stub=%p orig=%p", label, code, stub, orig);
    return (stub && orig) ? orig : nullptr;
}

static bool install_hooks() {
    void* sh = load_so_by_name("libshadowhook.so");
    if (!sh) { LOGE("libshadowhook.so not found"); return false; }
    LOGI("shadowhook handle=%p", sh);

    typedef int (*fn_init)(int, bool);
    fn_init init = (fn_init)dlsym(sh, "shadowhook_init");
    if (init) LOGI("shadowhook_init -> %d", init(0, false));

    bool ok = false;

    Il2CppClass* TimeCls = find_class_any("UnityEngine", "Time");
    if (TimeCls) {
        const MethodInfo* dm = find_method(TimeCls, "get_deltaTime", 0);
        if (dm) {
            void* o = try_hook(sh, *(void**)dm, (void*)hk_delta, "deltaTime");
            if (o) { g_orig_delta = o; g_tick_mode |= 1; ok = true; }
        }
    }

    // 同时装 runtime_invoke 作为主力，两个都带线程检查
    void* inv = (void*)elf_sym(g_il2cpp_base, "il2cpp_runtime_invoke");
    LOGI("runtime_invoke addr=%p", inv);
    if (inv) {
        void* o = try_hook(sh, inv, (void*)hk_invoke, "runtime_invoke");
        if (o) { g_orig_invoke = o; g_tick_mode |= 2; ok = true; }
    }

    // 每帧必调的主线程候选
    Il2CppClass* ScreenCls = find_class_any("UnityEngine", "Screen");
    if (ScreenCls) {
        const MethodInfo* w = find_method(ScreenCls, "get_width", 0);
        if (w) {
            void* o = try_hook(sh, *(void**)w, (void*)hk_width, "screen.width");
            if (o) { g_orig_width = o; g_tick_mode |= 4; ok = true; }
        }
    }

    Il2CppClass* TimeCls2 = find_class_any("UnityEngine", "Time");
    if (TimeCls2) {
        const MethodInfo* fc = find_method(TimeCls2, "get_frameCount", 0);
        if (fc) {
            void* o = try_hook(sh, *(void**)fc, (void*)hk_framecount, "frameCount");
            if (o) { g_orig_framecount = o; g_tick_mode |= 8; ok = true; }
        }
    }

    // 实例方法：游戏高频调用，且 AOT 签名简单
    Il2CppClass* ObjCls2 = find_class_any("UnityEngine", "Object");
    if (ObjCls2) {
        const MethodInfo* i1 = find_method(ObjCls2, "Instantiate", 1);
        if (i1) {
            void* o = try_hook(sh, *(void**)i1, (void*)hk_inst1, "Instantiate/1");
            if (o) { g_orig_inst1 = o; ok = true; }
        }
        const MethodInfo* i2 = find_method(ObjCls2, "Instantiate", 2);
        if (i2) {
            void* o = try_hook(sh, *(void**)i2, (void*)hk_inst2, "Instantiate/2");
            if (o) { g_orig_inst2 = o; ok = true; }
        }
        const MethodInfo* nm = find_method(ObjCls2, "get_name", 0);
        if (nm) {
            void* o = try_hook(sh, *(void**)nm, (void*)hk_name, "Object.get_name");
            if (o) { g_orig_name = o; g_tick_mode |= 16; ok = true; }
        }
    }

    Il2CppClass* CompCls2 = find_class_any("UnityEngine", "Component");
    if (CompCls2) {
        const MethodInfo* gt = find_method(CompCls2, "get_transform", 0);
        if (gt) {
            void* o = try_hook(sh, *(void**)gt, (void*)hk_gettrans, "Component.get_transform");
            if (o) { g_orig_gettrans = o; g_tick_mode |= 32; ok = true; }
        }
    }

    Il2CppClass* TransCls2 = find_class_any("UnityEngine", "Transform");
    if (TransCls2) {
        const MethodInfo* gp = find_method(TransCls2, "get_position", 0);
        if (gp) {
            void* o = try_hook(sh, *(void**)gp, (void*)hk_getpos, "Transform.get_position");
            if (o) { g_orig_getpos = o; g_tick_mode |= 64; ok = true; }
        }
    }

    LOGI("hook mask=%d", g_tick_mode);
    return ok;
}

// ================= 工作线程 =================
static void* worker(void*) {
    // 等 Java 层把日志路径传过来，最多等 2 秒
    for (int i = 0; i < 100 && g_log_path.empty(); i++) usleep(20 * 1000);

    std::string cmd = self_cmdline();
    LOGI("worker start in [%s]", cmd.c_str());
    if (cmd != "com.hermes.h1game") {
        LOGI("subprocess, skip");
        return nullptr;
    }

    uintptr_t base = wait_il2cpp();
    g_il2cpp_base = base;
    if (!base) { LOGE("libil2cpp.so not found"); return nullptr; }
    if (!bind_api(base)) return nullptr;
    LOGI("api bound, base 0x%lx", (unsigned long)base);

    for (int i = 0; i < 3000 && !g_domain; i++) {
        g_domain = g_api.domain_get();
        if (!g_domain) usleep(100 * 1000);
    }
    if (!g_domain) { LOGE("domain timeout"); return nullptr; }
    LOGI("domain ready: %p", g_domain);

    // domain 存在 != 初始化完了。等 assemblies 真正就绪，threadPool 才建好，
    // 否则 Thread::Attach 里 assert(g_Domain->threadPool != NULL) 直接 abort
    {
        int waited = 0;
        for (;;) {
            size_t n = 0;
            const Il2CppAssembly** asms = g_api.domain_get_assemblies(g_domain, &n);
            if (asms && n > 0) {
                LOGI("assemblies ready: %zu", n);
                break;
            }
            if (waited % 50 == 0) LOGI("waiting assemblies %d ms", waited * 100);
            usleep(100 * 1000);
            if (++waited > 600) { LOGE("assemblies timeout, abort mission"); return nullptr; }
        }
    }
    sleep(3);
    LOGI("proceeding to thread attach");

    // 先挂 JNI，il2cpp 的 Thread::Attach 内部要拿 JNIEnv，没有就 assert -> abort
    if (g_vm) {
        JNIEnv* env = nullptr;
        JavaVMAttachArgs aa;
        aa.version = JNI_VERSION_1_6;
        aa.name = (char*)"ModelMod";
        aa.group = nullptr;
        jint r = g_vm->AttachCurrentThread(&env, &aa);
        LOGI("AttachCurrentThread -> %d env=%p", (int)r, env);
    } else {
        LOGE("no JavaVM saved");
    }

    void* il2cpp_thread = g_api.thread_attach(g_domain);
    LOGI("il2cpp_thread_attach -> %p", il2cpp_thread);

    Il2CppClass* SMRCls = nullptr;
    for (int i = 0; i < 900; i++) {
        SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
        if (SMRCls) break;
        usleep(200 * 1000);
    }
    if (!SMRCls) { LOGE("SkinnedMeshRenderer not found"); return nullptr; }
    LOGI("SkinnedMeshRenderer: %p", SMRCls);

    // UGUI 每帧必调的 CanvasUpdateRegistry.PerformUpdate（主线程）
    {
        Il2CppClass* CU = find_class_any("UnityEngine.UI", "CanvasUpdateRegistry");
        if (CU) {
            LOGI("found CanvasUpdateRegistry");
            void* it = nullptr;
            const MethodInfo* mm;
            int cnt = 0;
            while ((mm = g_api.class_get_methods(CU, &it)) && cnt < 30) {
                LOGI("  M %s/%u", g_api.method_get_name(mm), g_api.method_get_param_count(mm));
                cnt++;
            }
            const MethodInfo* pu = find_method(CU, "PerformUpdate", 0);
            LOGI("PerformUpdate=%p", (void*)pu);
            if (pu) {
                void* sh = load_so_by_name("libshadowhook.so");
                if (sh) {
                    typedef int (*fn_init)(int, bool);
                    fn_init init = (fn_init)dlsym(sh, "shadowhook_init");
                    if (init) init(0, false);
                    void* o = try_hook(sh, *(void**)pu, (void*)hk_canvasupdate, "CanvasUpdate");
                    if (o) { g_orig_canvas = o; LOGI("canvas hook ok"); }
                }
            }
        } else {
            LOGE("no CanvasUpdateRegistry");
        }
    }

    // GameObject.SetActive：游戏开关对象必经之路，主线程高频
    {
        Il2CppClass* GO = find_class_any("UnityEngine", "GameObject");
        if (GO) {
            const MethodInfo* sa = find_method(GO, "SetActive", 1);
            LOGI("SetActive=%p", (void*)sa);
            if (sa) {
                void* sh = load_so_by_name("libshadowhook.so");
                if (sh) {
                    void* o = try_hook(sh, *(void**)sa, (void*)hk_setactive, "GameObject.SetActive");
                    if (o) { g_orig_setactive = o; LOGI("setactive hook ok"); }
                }
            }
        }
    }

    // Object.get_name / Component.get_transform：返回指针，ABI 简单，调用量大
    {
        Il2CppClass* OB = find_class_any("UnityEngine", "Object");
        if (OB) {
            const MethodInfo* nm = find_method(OB, "get_name", 0);
            if (nm) {
                void* sh = load_so_by_name("libshadowhook.so");
                if (sh) {
                    void* o = try_hook(sh, *(void**)nm, (void*)hk_getname, "Object.get_name");
                    if (o) { g_orig_getname = o; LOGI("getname hook ok"); }
                }
            }
        }
        Il2CppClass* CO = find_class_any("UnityEngine", "Component");
        if (CO) {
            const MethodInfo* gt = find_method(CO, "get_transform", 0);
            if (gt) {
                void* sh = load_so_by_name("libshadowhook.so");
                if (sh) {
                    void* o = try_hook(sh, *(void**)gt, (void*)hk_gettrans, "Component.get_transform");
                    if (o) { g_orig_gettrans = o; LOGI("gettrans hook ok"); }
                }
            }
        }
    }

    // Transform.get_position：每帧每个物体都调，真正的脏活入口
    {
        Il2CppClass* TR = find_class_any("UnityEngine", "Transform");
        if (TR) {
            const MethodInfo* gp = find_method(TR, "get_position", 0);
            LOGI("get_position=%p", (void*)gp);
            if (gp) {
                void* sh = load_so_by_name("libshadowhook.so");
                if (sh) {
                    void* o = try_hook(sh, *(void**)gp, (void*)hk_getpos, "Transform.get_position");
                    if (o) { g_orig_getpos = o; LOGI("getpos hook ok"); }
                }
            }
        }
    }

    if (MOD_MODE == 0) {
        LOGI("mode 0: fingerprint dump, every 10s");
        while (true) {
            dump_fingerprints(SMRCls);
            sleep(10);
        }
    }

    LOGI("mode 1: targeted swap");

    const MethodInfo* setMesh = find_method(SMRCls, "set_sharedMesh", 1);
    if (setMesh) LOGI("setter by name: set_sharedMesh");

    if (!setMesh) {
        setMesh = find_method(SMRCls, "set_mesh", 1);
        if (setMesh) LOGI("setter by name: set_mesh");
    }

    if (!setMesh) {
        // 按签名找：1 个参数，类型是 UnityEngine.Mesh
        Il2CppClass* MeshCls = find_class_any("UnityEngine", "Mesh");
        LOGI("fallback by signature, MeshCls=%p", MeshCls);
        for (Il2CppClass* k = SMRCls; k && !setMesh; k = g_api.class_get_parent(k)) {
            void* iter = nullptr;
            const MethodInfo* m;
            while ((m = g_api.class_get_methods(k, &iter))) {
                if (g_api.method_get_param_count(m) != 1) continue;
                const Il2CppType* pt = g_api.method_get_param(m, 0);
                if (!pt) continue;
                Il2CppClass* pc = g_api.class_from_type(pt);
                if (pc && pc == MeshCls) {
                    LOGI("setter by signature: %s", g_api.method_get_name(m));
                    setMesh = m;
                    break;
                }
            }
        }
    }

    if (!setMesh) {
        LOGE("no mesh setter, switching to hide mode");
        g_inplace_mode = true;
        Il2CppClass* RendererC = find_class_any("UnityEngine", "Renderer");
        g_setEnabled = find_method(RendererC, "set_enabled", 1);
        LOGI("hide mode, set_enabled=%p", (void*)g_setEnabled);
    }

    g_smrCls = SMRCls;

    // 只装 Instantiate hook：拿到主线程上下文去造对象
    {
        void* sh = load_so_by_name("libshadowhook.so");
        if (sh) {
            typedef int (*fn_init)(int, bool);
            fn_init init = (fn_init)dlsym(sh, "shadowhook_init");
            if (init) LOGI("shadowhook_init -> %d", init(0, false));

            Il2CppClass* ObjCls = find_class_any("UnityEngine", "Object");
            if (ObjCls) {
                const MethodInfo* i1 = find_method(ObjCls, "Instantiate", 1);
                if (i1) {
                    void* o = try_hook(sh, *(void**)i1, (void*)hk_inst1, "Instantiate/1");
                    if (o) g_orig_inst1 = o;
                }
                const MethodInfo* i2 = find_method(ObjCls, "Instantiate", 2);
                if (i2) {
                    void* o = try_hook(sh, *(void**)i2, (void*)hk_inst2, "Instantiate/2");
                    if (o) g_orig_inst2 = o;
                }
            }
        } else {
            LOGE("no shadowhook");
        }
    }

    LOGI("worker loop start (hide + instantiate-hook)");

    int round = 0;
    while (true) {
        round++;
        std::vector<Fingerprint> list;
        collect_fingerprints(SMRCls, list);
        LOGI("scan #%d: %zu smrs", round, list.size());

        int done = 0;
        for (auto& f : list) {
            if (done >= 4) break;
            if (g_patched_meshes.count(f.smr)) continue;

            for (int r = 0; r < RULE_COUNT; r++) {
                if (!rule_matches(g_rules[r], f)) continue;

                LOGI("matched %s", f.chain.c_str());

                if (g_setEnabled) {
                    uint8_t off = 0;
                    void* a[1] = { &off };
                    invoke_inst(g_setEnabled, f.smr, a);
                }

                // 塞队列，等主线程造对象
                pthread_mutex_lock(&g_qmtx);
                g_smr_queue.push_back(f.smr);
                pthread_mutex_unlock(&g_qmtx);

                g_patched_meshes[f.smr] = 1;
                done++;

                break;
            }
        }

        if (done > 0) LOGI("round %d done=%d", round, done);
        sleep(2);
    }
    return nullptr;
}

// ================= 入口 =================
// Java 层把游戏的 files 目录传进来，保证日志一定写得出去
extern "C" JNIEXPORT void JNICALL
Java_com_moss_modelmod_HookEntry_setLogPath(JNIEnv* env, jclass, jstring path) {
    if (!path) return;
    const char* p = env->GetStringUTFChars(path, nullptr);
    if (p) {
        g_log_path = p;
        env->ReleaseStringUTFChars(path, p);
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "log path: %s", g_log_path.c_str());
}

extern "C" jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    LOGI("JNI_OnLoad entered");
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    if (pthread_create(&t, &attr, worker, nullptr) == 0) {
        pthread_detach(t);
    } else {
        LOGE("pthread_create failed");
    }
    pthread_attr_destroy(&attr);
    return JNI_VERSION_1_6;
}
