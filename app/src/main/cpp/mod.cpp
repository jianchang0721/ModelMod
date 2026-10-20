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
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <android/log.h>
#include <time.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/syscall.h>

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

struct Vec3 { float x, y, z; };
struct Color4 { float r, g, b, a; };
struct Quat { float x, y, z, w; };

Il2CppApi g_api{};
static JavaVM* g_vm = nullptr;
static Il2CppDomain* g_domain = nullptr;

static thread_local pid_t t_tid = 0;
static pid_t cur_tid() {
    if (!t_tid) t_tid = (pid_t)syscall(__NR_gettid);
    return t_tid;
}

// UnityMain 线程的 tid 不等于 pid！只能靠线程名认
// 缓存：clock_gettime 每秒几千次，不能每次都 prctl
// 游戏会建好几个叫 UnityMain 的线程，只有一个是真的：clock_gettime 频率最高的那个
static volatile pid_t g_main_tid = 0;

// 每个线程的 clock_gettime 命中数，用来看谁才是真主线程
struct TidStat { volatile pid_t tid; volatile int hits; };
static TidStat g_tidstat[48];
static void stat_touch_tid(pid_t tid, int add) {
    for (int i = 0; i < 48; i++) {
        if (g_tidstat[i].tid == tid) { g_tidstat[i].hits += add; return; }
        if (g_tidstat[i].tid == 0) { g_tidstat[i].tid = tid; g_tidstat[i].hits = add; return; }
    }
}
static void stat_dump() {
    for (int i = 0; i < 48; i++) {
        if (!g_tidstat[i].tid) break;
        LOGI("tidstat tid=%d hits=%d", (int)g_tidstat[i].tid, g_tidstat[i].hits);
    }
}

static bool is_unity_main() {
    pid_t tid = cur_tid();
    if (g_main_tid) return tid == g_main_tid;
    static thread_local int8_t cached = 0;
    if (cached == 1) return true;              // 只缓存 true：线程名是启动后才设的
    char name[16] = {0};
    if (prctl(PR_GET_NAME, name) != 0) return false;
    if (strncmp(name, "UnityMain", 9) == 0) { cached = 1; return true; }
    return false;
}
static std::string g_log_path;
static FILE* g_log_fp = nullptr;
static std::map<std::string, Il2CppObject*> g_mesh_cache;
static bool g_inplace_mode = false;
static volatile int g_hide_mask = 0;   // 默认全显示。bit0 Body 1 Face 2 Hair 3 Weapon 4 PX 5 NPC 6 Effect 7 Cloak

// 每个 hook 入口的命中数，用来判断哪个真的在高频跑
static volatile int g_hookcnt[12] = {0};
#define HC_TWEEN    0
#define HC_CANVAS   1
#define HC_SETACT   2
#define HC_GETNAME  3
#define HC_GETTRANS 4
#define HC_GETPOS   5
#define HC_EVENT    6
#define HC_CSCALER  7
#define HC_ASPECT   8
#define HC_VOLUME   9
#define HC_CGT      10
#define HC_CGT_ALL  11
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
// il2cpp_class_get_methods 的迭代器是类内部共享的：worker 线程在扫时，主线程也扫，
// 两边互相把迭代器搞坏 → find_method 返回 null → 主线程 collect_fingerprints 直接返回空
static pthread_mutex_t g_find_mtx = PTHREAD_MUTEX_INITIALIZER;

static const MethodInfo* find_method(Il2CppClass* klass, const char* name, int argc) {
    if (!klass) return nullptr;
    pthread_mutex_lock(&g_find_mtx);
    const MethodInfo* found = nullptr;
    for (Il2CppClass* k = klass; k && !found; k = g_api.class_get_parent(k)) {
        void* iter = nullptr;
        const MethodInfo* m;
        while ((m = g_api.class_get_methods(k, &iter))) {
            if (strcmp(g_api.method_get_name(m), name) != 0) continue;
            if ((int)g_api.method_get_param_count(m) != argc) continue;
            found = m;
            break;
        }
    }
    pthread_mutex_unlock(&g_find_mtx);
    return found;
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

    // 字段相关（可选，缺了不影响主流程）
    g_api.class_get_fields = (fn_class_get_fields)elf_sym(base, "il2cpp_class_get_fields");
    g_api.field_get_name   = (fn_field_get_name)  elf_sym(base, "il2cpp_field_get_name");
    g_api.field_get_offset = (fn_field_get_offset)elf_sym(base, "il2cpp_field_get_offset");
    g_api.field_get_type   = (fn_field_get_type)  elf_sym(base, "il2cpp_field_get_type");
    g_api.type_get_name    = (fn_type_get_name)   elf_sym(base, "il2cpp_type_get_name");
    g_api.class_array_element_size = (fn_class_array_element_size)elf_sym(base, "il2cpp_class_array_element_size");
    LOGI("array_element_size api: %p", (void*)g_api.class_array_element_size);
    LOGI("field api: %p %p %p %p %p", (void*)g_api.class_get_fields, (void*)g_api.field_get_name,
         (void*)g_api.field_get_offset, (void*)g_api.field_get_type, (void*)g_api.type_get_name);
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
    if (!ObjectCls || !CompCls || !TransCls || !Renderer) {
        LOGE("collect: class missing O=%p C=%p T=%p R=%p", ObjectCls, CompCls, TransCls, Renderer);
        return;
    }

    const MethodInfo* find     = find_method(ObjectCls, "FindObjectsOfType", 1);
    const MethodInfo* getGO    = find_method(CompCls, "get_gameObject", 0);
    const MethodInfo* getTrans = find_method(CompCls, "get_transform", 0);
    const MethodInfo* getParent= find_method(TransCls, "get_parent", 0);
    const MethodInfo* getBones = find_method(SMRCls, "get_bones", 0);
    const MethodInfo* getMats  = find_method(Renderer, "get_sharedMaterials", 0);
    const MethodInfo* getMesh  = find_method(SMRCls, "get_sharedMesh", 0);
    if (!find || !getGO || !getTrans || !getParent || !getBones || !getMats || !getMesh) {
        LOGE("collect: method missing find=%p GO=%p TR=%p PA=%p BO=%p MA=%p ME=%p",
             (void*)find, (void*)getGO, (void*)getTrans, (void*)getParent,
             (void*)getBones, (void*)getMats, (void*)getMesh);
        return;
    }

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

// 只看路径最后一段：避免 Face 误命中字样，也避免 Body_PX 归错类
static int part_bit(const std::string& chain) {
    size_t p = chain.rfind('/');
    std::string leaf = (p == std::string::npos) ? chain : chain.substr(p + 1);

    // 披风优先（Cloak / Cape）
    if (leaf.find("Cloak") != std::string::npos || leaf.find("Cape") != std::string::npos) return 7;
    // 特效层
    if (leaf.find("Effect") != std::string::npos || leaf.find("Fx") != std::string::npos ||
        leaf.find("FX") != std::string::npos || leaf.find("Trail") != std::string::npos ||
        leaf.find("Glow") != std::string::npos || leaf.find("Aura") != std::string::npos) return 6;
    // 描边
    if (leaf.find("PX") != std::string::npos) return 4;

    if (leaf.find("Weapon") != std::string::npos) return 3;
    if (leaf.find("Hair") != std::string::npos) return 2;
    if (leaf.find("Face") != std::string::npos) return 1;
    if (leaf.find("Body") != std::string::npos) return 0;

    if (chain.find("Cloak") != std::string::npos || chain.find("Cape") != std::string::npos) return 7;
    if (chain.find("Effect") != std::string::npos || chain.find("Trail") != std::string::npos) return 6;
    if (chain.find("_PX") != std::string::npos) return 4;
    if (chain.find("Weapon") != std::string::npos) return 3;
    if (chain.find("Hair") != std::string::npos) return 2;
    if (chain.find("Face") != std::string::npos) return 1;
    if (chain.find("Body") != std::string::npos) return 0;
    return -1;
}

// 开关面板驱动：按部位 bit 判断要不要隐藏
static bool rule_matches(const CharRule& r, const Fingerprint& f) {
    (void)r;
    int mask = g_hide_mask;

    if (f.chain.compare(0, 9, "NPCRoot/") == 0) {
        if (!(mask & (1 << 5))) return false;
    }

    int bit = part_bit(f.chain);
    if (bit < 0) return false;
    if (!(mask & (1 << bit))) return false;
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

    // 放大 + 抬到头顶，不然埋在模型里看不见
    if (ct) {
        const MethodInfo* setScale = find_method(TransCls, "set_localScale", 1);
        const MethodInfo* setLPos   = find_method(TransCls, "set_localPosition", 1);
        if (setScale) {
            Vec3 s = {3.0f, 3.0f, 3.0f};
            void* a[1] = { &s };
            invoke_inst(setScale, ct, a);
            LOGI("cube: scale set");
        }
        if (setLPos) {
            Vec3 p = {0.0f, 2.0f, 0.0f};
            void* a[1] = { &p };
            invoke_inst(setLPos, ct, a);
            LOGI("cube: pos set");
        }
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

// 枚举所有 MonoBehaviour 子类，找出真正带 Update 的（这些必定主线程每帧调、且走虚表不可内联）
static void find_updates() {
    Il2CppClass* MB = find_class_any("UnityEngine", "MonoBehaviour");
    if (!MB) { LOGE("no MonoBehaviour class"); return; }

    size_t n = 0;
    const Il2CppAssembly** asms = g_api.domain_get_assemblies(g_domain, &n);
    if (!asms) return;

    int found = 0;
    for (size_t i = 0; i < n && found < 60; i++) {
        Il2CppImage* img = g_api.assembly_get_image(asms[i]);
        if (!img) continue;
        size_t cn = g_api.image_get_class_count(img);
        for (size_t j = 0; j < cn && found < 60; j++) {
            Il2CppClass* c = g_api.image_get_class(img, j);
            if (!c) continue;
            if (!g_api.class_is_assignable_from(MB, c)) continue;
            const MethodInfo* m = find_method(c, "Update", 0);
            if (!m) continue;
            LOGI("UPDATE %s.%s", g_api.class_get_namespace(c), g_api.class_get_name(c));
            found++;
        }
    }
    LOGI("updates=%d", found);
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

static volatile int g_cube_done = 0;
// ===== v114: GPU 蒙皮（克隆原 SMR，引擎自己驱骨骼）状态 =====
static volatile int g_gpu_skin = 0;
static volatile int g_gpu_tried = 0;
static volatile int g_probed = 0;
static std::set<Il2CppObject*> g_ourSmrs;
static volatile int g_custom_on = 1;      // 1 = 显示我们自己的模型

// v144 渲染模式：
//   0 = 静态（v113 那套：完整模型 + 贴图，不跟骨骼）
//   1 = 刚性分段（每块挂一根骨骼，近似）
//   2 = CPU 蒙皮（标准 LBS，顶点每 2 帧重算一次）
static volatile int g_render_mode = 0;

static volatile int g_replace_mode = 0;   // 1 = 隐藏原角色，用我们的模型顶替
static Il2CppObject* g_player_root = nullptr;   // 玩家主控角色的根节点
static volatile int g_hide_main_pending = 0;    // 替换开关切换后要立即刷一次隐藏
static volatile int g_force_tick = 0;           // 刷新模型时跳过 3 秒节流
static volatile int g_only_main = 1;            // 1 = 只隐藏主控，不动对面
static Il2CppObject* g_last_model_go = nullptr;
static Il2CppObject* g_model_go = nullptr;
static long g_lastGO = -1;

// Instantiate hook：在主线程创建对象时插一脚
static void main_tick();
static void apply_replace_mode();
static Il2CppObject* spawn_model_object(Il2CppObject* smr);
static void ensure_model_visible();
static Il2CppObject* find_role_root(Il2CppObject* smr);
static bool under_player_root(Il2CppObject* smr);
static int spawn_parts_loop(Il2CppObject* smr);
static void build_target_list(Il2CppObject* playerRoot);
static int spawn_rigid_parts(Il2CppObject* origSmr);
static void update_skin_parts();

// ===== v141: 原生蒙皮验证 =====
// 思路：一行 bindposes 都不改，只改原 mesh 自己的顶点和权重，
// 让游戏拿它自己的 bindposes + bones 去做蒙皮。
// 1 = 顶点整体减半（验证 set_vertices 单次调用能否生效）
// 2 = 全部顶点绑到 0 号骨（验证 set_boneWeights 能否生效）
// 3 = 还原
static volatile int g_test_cmd = 0;
static std::vector<unsigned char> g_bak_verts;
static std::vector<unsigned char> g_bak_bw;
static int g_bak_nv = 0;
static Il2CppObject* g_bak_mesh = nullptr;
static void run_mesh_test(int cmd);
static void probe_native_skinning();

static void* g_orig_canvas = nullptr;
static void* g_orig_inst1 = nullptr;
static void* g_orig_inst2 = nullptr;
static int g_inst_hits = 0;
static std::vector<Il2CppObject*> g_smr_queue;
static std::set<std::string> g_path_dump;
static pthread_mutex_t g_qmtx = PTHREAD_MUTEX_INITIALIZER;
static void try_patch_in_main();
static long g_lastSMR = -1;

static void hk_canvasupdate(void* m) {
    g_hookcnt[HC_CANVAS]++;
    main_tick();
    ((void(*)(void*))g_orig_canvas)(m);
}

static void* g_orig_setactive = nullptr;
static void* g_orig_tween = nullptr;

// DOTween 的 Update：第三方 MonoBehaviour，虚表调用，不可能被内联
static void hk_tween_update(void* self, void* m) {
    g_hookcnt[HC_TWEEN]++;
    main_tick();
    ((void(*)(void*, void*))g_orig_tween)(self, m);
}

// ---------------------------------------------------------------
// 自己的 ARM64 inline hook：shadowhook 实测根本改不动代码（日志里 hook 后字节还是原指令）
// 这条路径不需要调原函数，直接写 16 字节绝对跳转就行
// ldr x16, #8 ; br x16 ; <addr>
// ---------------------------------------------------------------
static bool self_hook_code(void* target, void* replace) {
    uintptr_t page = (uintptr_t)target & ~(uintptr_t)0xFFFULL;
    size_t len = 0x2000;
    if (mprotect((void*)page, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("mprotect RWX fail at %p errno=%d", (void*)page, errno);
        return false;
    }
    unsigned char* p = (unsigned char*)target;
    uint32_t insn[4];
    insn[0] = 0x58000050;   // ldr x16, #8
    insn[1] = 0xD61F0200;   // br x16
    uint64_t addr = (uint64_t)replace;
    memcpy(&insn[2], &addr, 8);
    memcpy(p, insn, 16);
    __builtin___clear_cache((char*)p, (char*)p + 16);
    mprotect((void*)page, len, PROT_READ | PROT_EXEC);
    LOGI("self_hook %p -> %p bytes=%02x%02x%02x%02x%02x%02x%02x%02x",
         target, replace, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    return true;
}

// libc 的 clock_gettime：UnityMain 每秒调几千次，这才是真正的高频主线程入口
static int (*g_orig_cgt)(clockid_t, struct timespec*) = nullptr;

// 兜底：hook 刚装上的瞬间有无数线程在调 clock_gettime，orig 还没赋值就崩了
static int cgt_fallback(clockid_t id, struct timespec* ts) {
    return (int)syscall(__NR_clock_gettime, id, ts);
}

// libunity/libil2cpp 的代码段范围：只有从这两个模块里发起 clock_gettime 才敢做重活
// （从 malloc/GC/ART 内部调进来的，此时可能持着锁，再调 il2cpp 就死锁）
static uintptr_t g_unity_lo = 0, g_unity_hi = 0;
static uintptr_t g_il2cpp_lo = 0, g_il2cpp_hi = 0;

static void find_module_ranges() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t lo = 0, hi = 0;
        if (sscanf(line, "%lx-%lx", &lo, &hi) != 2) continue;
        if (strstr(line, "libunity.so")) {
            if (!g_unity_lo || lo < g_unity_lo) g_unity_lo = lo;
            if (hi > g_unity_hi) g_unity_hi = hi;
        } else if (strstr(line, "libil2cpp.so")) {
            if (!g_il2cpp_lo || lo < g_il2cpp_lo) g_il2cpp_lo = lo;
            if (hi > g_il2cpp_hi) g_il2cpp_hi = hi;
        }
    }
    fclose(f);
    LOGI("ranges unity=%p-%p il2cpp=%p-%p", (void*)g_unity_lo, (void*)g_unity_hi,
         (void*)g_il2cpp_lo, (void*)g_il2cpp_hi);
}

static bool safe_call_site() {
    uintptr_t r = (uintptr_t)__builtin_return_address(0);
    if (r >= g_unity_lo && r < g_unity_hi) return true;
    if (r >= g_il2cpp_lo && r < g_il2cpp_hi) return true;
    return false;
}

static int hk_cgt(clockid_t id, struct timespec* ts) {
    g_hookcnt[HC_CGT_ALL]++;          // 不管哪个线程，先进来就算

    // 锁定真主线程：clock_gettime 调用次数最多的那个 UnityMain
    // （游戏有好几个同名线程，第一个跑到阈值的未必是主线程）
    {
        static thread_local int hits = 0;
        hits++;
        if ((hits % 503) == 0) {          // 每 503 次更新一次全局表，不拖性能
            stat_touch_tid(cur_tid(), 503);
        }
        if (!g_main_tid && hits > 5000) {
            char nm[16] = {0};
            if (prctl(PR_GET_NAME, nm) == 0 && strncmp(nm, "UnityMain", 9) == 0) {
                g_main_tid = cur_tid();
                LOGI("real main tid locked = %d name=%s (hits=%d)", (int)g_main_tid, nm, hits);
                stat_dump();
            }
        }
    }

    if (is_unity_main() && safe_call_site()) {
        g_hookcnt[HC_CGT]++;
        main_tick();
    }
    if (g_orig_cgt) return g_orig_cgt(id, ts);
    return cgt_fallback(id, ts);
}

// dotween 之外每帧必调的 MonoBehaviour 候选
static void* g_orig_event  = nullptr;
static void* g_orig_cscaler = nullptr;
static void* g_orig_aspect = nullptr;
static void* g_orig_volume = nullptr;

static void hk_event_update(void* self, void* m) {
    g_hookcnt[HC_EVENT]++;
    main_tick();
    ((void(*)(void*, void*))g_orig_event)(self, m);
}

static void hk_cscaler_update(void* self, void* m) {
    g_hookcnt[HC_CSCALER]++;
    main_tick();
    ((void(*)(void*, void*))g_orig_cscaler)(self, m);
}

static void hk_aspect_update(void* self, void* m) {
    g_hookcnt[HC_ASPECT]++;
    main_tick();
    ((void(*)(void*, void*))g_orig_aspect)(self, m);
}

static void hk_volume_update(void* self, void* m) {
    g_hookcnt[HC_VOLUME]++;
    main_tick();
    ((void(*)(void*, void*))g_orig_volume)(self, m);
}
static void* g_orig_getname = nullptr;

static Il2CppString* hk_getname(void* self, void* m) {
    g_hookcnt[HC_GETNAME]++;
    main_tick();
    return ((Il2CppString*(*)(void*, void*))g_orig_getname)(self, m);
}

static void hk_setactive(void* self, void* m, uint8_t v) {
    g_hookcnt[HC_SETACT]++;
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
        LOGI("tick init: smr=%p setEnabled=%p tid=%d", g_smrCls, g_setEnabled, (int)cur_tid());
        probe_types();

        // 诊断：主线程里 il2cpp 调用到底哪一步断了
        Il2CppClass* OC = find_class_any("UnityEngine", "Object");
        LOGI("diag: ObjectCls=%p", OC);
        if (OC && g_smrCls) {
            const MethodInfo* fm = find_method(OC, "FindObjectsOfType", 1);
            LOGI("diag: FindObjectsOfType=%p", (void*)fm);
            if (fm) {
                void* a[1] = { g_api.type_get_object(g_api.class_get_type(g_smrCls)) };
                LOGI("diag: typeObj=%p", a[0]);
                Il2CppArray* arr = (Il2CppArray*)invoke_static(fm, a);
                LOGI("diag: arr=%p len=%zu", arr, arr ? arr->max_length : (size_t)-1);
            }
        }
    }
    if (!g_smrCls) return;

    // 场景变化监控：GO / SMR 数量一变就报
    {
        static int probeTick = 0;
        if ((probeTick++ % 3) == 0) {
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

    // 场景切了引擎会把挂上去的对象销毁掉，托管对象还在但 m_CachedPtr 归 0，
    // 这时候得自己把标记重置，不然就一直不再重建
    if (g_last_model_go) {
        void* np = *(void**)((char*)g_last_model_go + 16);
        if (!np) {
            LOGI("model: engine destroyed previous one, rebuild");
            g_last_model_go = nullptr;
            g_cube_done = 0;
            g_gpu_tried = 0; g_gpu_skin = 0; g_ourSmrs.clear();   // v114
        }
    }

    // 目标列表（给悬浮窗选择用），每次 tick 刷一遍
    build_target_list(g_player_root);

    // 已经干完活就不再全量扫描，别再拖主线程
    if (g_cube_done >= 1) return;

    // 上一版模型先干掉，免得刷新后叠一堆
    if (g_last_model_go) {
        Il2CppClass* ObjectCls2 = find_class_any("UnityEngine", "Object");
        const MethodInfo* destroy = ObjectCls2 ? find_method(ObjectCls2, "Destroy", 1) : nullptr;
        if (destroy) {
            void* a[1] = { g_last_model_go };
            invoke_static(destroy, a);
            LOGI("model: old destroyed %p", g_last_model_go);
        }
        g_last_model_go = nullptr;
        g_model_go = nullptr;
    }

    std::vector<Fingerprint> list;
    collect_fingerprints(g_smrCls, list);
    g_last_scan = list.size();

    if (list.empty()) return;

    // 不管 worker 是否已隐藏，前 3 个匹配的 SMR 直接造 cube（当前主线，安全）
    // 选靶子：离主相机最近的那个 Body 就是你自己
    // （PlayerRoot 下同时挂着自己和对面玩家，光看路径分不出谁是谁）
    Il2CppObject* pick = nullptr;
    const char* chainOf = nullptr;
    {
        float camX = 0, camY = 0, camZ = 0;
        bool haveCam = false;
        Il2CppClass* CamClsP = find_class_any("UnityEngine", "Camera");
        Il2CppClass* TransClsP = find_class_any("UnityEngine", "Transform");
        Il2CppClass* CompClsP = find_class_any("UnityEngine", "Component");
        const MethodInfo* getTransC2 = CompClsP ? find_method(CompClsP, "get_transform", 0) : nullptr;
        const MethodInfo* getPosP = TransClsP ? find_method(TransClsP, "get_position", 0) : nullptr;
        if (CamClsP && TransClsP) {
            const MethodInfo* getMainP = find_method(CamClsP, "get_main", 0);
            Il2CppObject* cam = getMainP ? invoke_static(getMainP, nullptr) : nullptr;
            if (cam && getTransC2 && getPosP) {
                Il2CppObject* ct = invoke_inst(getTransC2, cam, nullptr);
                Il2CppObject* po = ct ? invoke_inst(getPosP, ct, nullptr) : nullptr;
                if (po) {
                    Vec3 v = *(Vec3*)((char*)po + 16);
                    camX = v.x; camY = v.y; camZ = v.z;
                    haveCam = true;
                }
            }
        }

        if (haveCam) {
            float best = 1e18f;
            for (auto& f : list) {
                if (f.chain.find("Body") == std::string::npos) continue;
                if (f.chain.find("VisualPool") != std::string::npos) continue;
                if (f.chain.find("AvatarPoolRoot") != std::string::npos) continue;
                if (f.chain.find("NPCRoot") != std::string::npos) continue;
                Il2CppObject* t = getTransC2 ? invoke_inst(getTransC2, f.smr, nullptr) : nullptr;
                Il2CppObject* po = t ? invoke_inst(getPosP, t, nullptr) : nullptr;
                if (!po) continue;
                Vec3 v = *(Vec3*)((char*)po + 16);
                float dx = v.x - camX, dy = v.y - camY, dz = v.z - camZ;
                float d = dx * dx + dy * dy + dz * dz;
                if (d < best) { best = d; pick = f.smr; chainOf = f.chain.c_str(); }
            }
            LOGI("pick: nearest body dist2=%.1f", best);
        }
        // 拿不到相机位置时退回老的 PlayerRoot 优先
        if (!pick) {
            for (auto& f : list) {
                if (f.chain.find("PlayerRoot") != std::string::npos &&
                    f.chain.find("Body") != std::string::npos) {
                    pick = f.smr; chainOf = f.chain.c_str(); break;
                }
            }
        }
    }

    // 锁定玩家主控角色根节点，隐藏逻辑靠它过滤
    if (pick) {
        Il2CppObject* pr = find_role_root(pick);
        if (pr && pr != g_player_root) {
            g_player_root = pr;
            LOGI("player root locked = %p", pr);
        }
    }

    int done = 0;
    if (pick) {
        g_cube_done++;
        LOGI("model: trying %s", chainOf ? chainOf : "?");
        int made = spawn_parts_loop(pick);
        LOGI("model: parts spawned=%d", made);
        done++;
    }
    if (done > 0) LOGI("tick cube done=%d total=%zu", done, g_last_scan);
}

// 替换开关切换后立即生效：把主控角色下的所有 SMR 统一开/关
static void apply_replace_mode() {
    Il2CppClass* SMRCls = g_smrCls ? g_smrCls : find_class_any("UnityEngine", "SkinnedMeshRenderer");
    if (!SMRCls) { LOGE("apply: no SMR class"); return; }
    if (!g_player_root) { LOGE("apply: player root not locked yet"); return; }

    std::vector<Fingerprint> list;
    collect_fingerprints(SMRCls, list);
    int n = 0;
    for (auto& f : list) {
        if (g_ourSmrs.count(f.smr)) continue;   // v114: 自己的 SMR 不参与隐藏
        if (!under_player_root(f.smr)) continue;
        uint8_t v = g_replace_mode ? 0 : 1;
        void* a[1] = { &v };
        if (g_setEnabled) { invoke_inst(g_setEnabled, f.smr, a); n++; }
    }
    LOGI("apply replace=%d -> %d smrs", (int)g_replace_mode, n);
}

// 节流 + 防重入 + 只主线程执行
static void main_tick() {
    static thread_local int in_tick = 0;
    if (in_tick) return;      // 防递归（以后 hook libc 时会用到）
    in_tick = 1;

    pid_t tid = cur_tid();
    bool isMain = is_unity_main();

    static int tmain = 0;
    if (isMain) {
        tmain++;
        if (tmain <= 5 || (tmain % 20000) == 0) {
            LOGI("tick MAIN #%d tid=%d", tmain, (int)tid);
            LOGI("hookcnt tween=%d canvas=%d setact=%d getname=%d gettrans=%d getpos=%d event=%d cscaler=%d aspect=%d volume=%d",
                 g_hookcnt[HC_TWEEN], g_hookcnt[HC_CANVAS], g_hookcnt[HC_SETACT],
                 g_hookcnt[HC_GETNAME], g_hookcnt[HC_GETTRANS], g_hookcnt[HC_GETPOS],
                 g_hookcnt[HC_EVENT], g_hookcnt[HC_CSCALER], g_hookcnt[HC_ASPECT], g_hookcnt[HC_VOLUME]);
            LOGI("hookcnt2 cgt_main=%d cgt_all=%d", g_hookcnt[HC_CGT], g_hookcnt[HC_CGT_ALL]);
        }
    }

    if (!isMain) { in_tick = 0; return; }      // 非主线程不算数
    if (__sync_lock_test_and_set(&g_tick_busy, 1)) { in_tick = 0; return; }

    // 时间节流 250ms 一次，不看命中次数
    static struct timespec last = {0, 0};
    struct timespec now;
    syscall(__NR_clock_gettime, CLOCK_MONOTONIC, &now);   // 绕开自己的 hook
    long ms = (long)(now.tv_sec - last.tv_sec) * 1000L + (now.tv_nsec - last.tv_nsec) / 1000000L;
    ensure_model_visible();

    // CPU 蒙皮（模式 2）：每 2 帧一次。
    // v113 记的"fdsan / NPTH"经复查站不住：update_skin_parts 里一行文件操作都没有，
    // 日志里 NPTH 一次都没出现过。真正崩的是旧版只减平移、没乘 bind 逆矩阵（v144 修）。
    if (g_render_mode == 2) {
        static int skinTick = 0;
        if ((skinTick++ % 3) == 0) update_skin_parts();
    }

    // v141: 验证命令，排到主线程跑
    if (g_test_cmd) {
        int c = g_test_cmd; g_test_cmd = 0;
        run_mesh_test(c);
    }

    // 开关切换：立即处理，不等 3 秒
    if (g_hide_main_pending) {
        g_hide_main_pending = 0;
        apply_replace_mode();
    }

    // 刷新模型：跳过节流马上重建
    if (g_force_tick) {
        g_force_tick = 0;
        last = now;
        do_patch_tick();
    } else if (last.tv_sec == 0 || ms >= 3000) {
        last = now;
        do_patch_tick();
    }
    __sync_lock_release(&g_tick_busy);
    in_tick = 0;
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
// struct Vec3 提到顶部了

static void* g_orig_name = nullptr;
static void* g_orig_gettrans = nullptr;
static void* g_orig_getpos = nullptr;

static Il2CppString* hk_name(void* self, void* m) {
    main_tick();
    return ((Il2CppString*(*)(void*, void*))g_orig_name)(self, m);
}

static Il2CppObject* hk_gettrans(void* self, void* m) {
    g_hookcnt[HC_GETTRANS]++;
    main_tick();
    return ((Il2CppObject*(*)(void*, void*))g_orig_gettrans)(self, m);
}

static Vec3 hk_getpos(void* self, void* m) {
    g_hookcnt[HC_GETPOS]++;
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

// 每个 hook 点的快照，看门狗拿它判断代码是否被改回去
struct HookRec {
    void* code;
    void* fn;
    void* stub;
    void* orig;
    const char* tag;
    unsigned char snap[16];
    int reinstalls;
};
static std::vector<HookRec> g_hookrecs;
static pthread_mutex_t g_hrec_mtx = PTHREAD_MUTEX_INITIALIZER;
static void* g_shook = nullptr;

static void* try_hook(void* sh, void* code, void* handler, const char* label) {
    typedef void* (*fn_hook)(void*, void*, void**);
    fn_hook h = (fn_hook)dlsym(sh, "shadowhook_hook_func_addr");
    if (!h) { LOGE("hook sym missing"); return nullptr; }
    void* orig = nullptr;
    void* stub = h(code, handler, &orig);
    {
        unsigned char b[16];
        memcpy(b, code, 16);
        LOGI("hook %s: code=%p stub=%p orig=%p bytes=%02x%02x%02x%02x%02x%02x%02x%02x",
             label, code, stub, orig, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    }
    if (!stub || !orig) return nullptr;

    HookRec r;
    r.code = code; r.fn = handler; r.stub = stub; r.orig = orig;
    r.tag = label; r.reinstalls = 0;
    memcpy(r.snap, code, 16);
    pthread_mutex_lock(&g_hrec_mtx);
    g_hookrecs.push_back(r);
    g_shook = sh;
    pthread_mutex_unlock(&g_hrec_mtx);
    return orig;
}

// 看门狗：腾讯 ACE 会把 .text 改回去，这里每 80ms 对比快照，变了就重装
static void* hook_watchdog(void*) {
    typedef void* (*fn_hook)(void*, void*, void**);
    typedef int (*fn_unhook)(void*);
    int round = 0;
    while (true) {
        usleep(80 * 1000);
        round++;
        pthread_mutex_lock(&g_hrec_mtx);
        for (size_t i = 0; i < g_hookrecs.size(); i++) {
            HookRec& r = g_hookrecs[i];
            if (memcmp((const void*)r.code, r.snap, 16) == 0) continue;

            fn_hook h = (fn_hook)dlsym(g_shook, "shadowhook_hook_func_addr");
            fn_unhook un = (fn_unhook)dlsym(g_shook, "shadowhook_unhook");
            if (un && r.stub) un(r.stub);
            void* orig2 = nullptr;
            void* stub2 = h ? h(r.code, r.fn, &orig2) : nullptr;
            if (stub2) {
                r.stub = stub2;
                if (orig2) r.orig = orig2;
                memcpy(r.snap, (const void*)r.code, 16);
                r.reinstalls++;
                if (r.reinstalls <= 3 || (r.reinstalls % 50) == 0)
                    LOGI("rehook %s #%d ok", r.tag, r.reinstalls);
            } else {
                if (r.reinstalls >= 0) LOGE("rehook %s FAILED", r.tag);
                r.reinstalls = -1000000;
            }
        }
        pthread_mutex_unlock(&g_hrec_mtx);

        if (round >= 125) {   // 约 10 秒报一次总量
            round = 0;
            int total = 0;
            pthread_mutex_lock(&g_hrec_mtx);
            for (auto& r : g_hookrecs) if (r.reinstalls > 0) total += r.reinstalls;
            pthread_mutex_unlock(&g_hrec_mtx);
            LOGI("watchdog: total rehooks=%d points=%zu", total, g_hookrecs.size());
            if (!g_hookrecs.empty()) {
                const unsigned char* c = (const unsigned char*)g_hookrecs[g_hookrecs.size() - 1].code;
                LOGI("watchdog: last hook %s bytes now=%02x%02x%02x%02x snap=%02x%02x%02x%02x",
                     g_hookrecs.back().tag, c[0], c[1], c[2], c[3],
                     g_hookrecs.back().snap[0], g_hookrecs.back().snap[1],
                     g_hookrecs.back().snap[2], g_hookrecs.back().snap[3]);
            }
        }
    }
    return nullptr;
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

// dump 一个类的字段偏移和方法名：判断能不能直接改字段，或有没有漏掉的 setter
static void dump_class_members(const char* ns, const char* name) {
    Il2CppClass* k = find_class_any(ns, name);
    if (!k) { LOGE("dump: no class %s.%s", ns, name); return; }
    LOGI("==== dump %s ====", name);
    void* it = nullptr;
    FieldInfo* f;
    int n = 0;
    if (g_api.class_get_fields) {
        while ((f = g_api.class_get_fields(k, &it)) && n < 120) {
            const Il2CppType* t = g_api.field_get_type ? g_api.field_get_type(f) : nullptr;
            const char* tn = (t && g_api.type_get_name) ? g_api.type_get_name(t) : "?";
            LOGI("F %-30s off=%4d type=%s", g_api.field_get_name(f),
                 (int)g_api.field_get_offset(f), tn ? tn : "?");
            n++;
        }
    }
    LOGI("-- %s fields=%d --", name, n);
    it = nullptr;
    const MethodInfo* m;
    n = 0;
    while ((m = g_api.class_get_methods(k, &it)) && n < 250) {
        LOGI("M %s/%u", g_api.method_get_name(m), g_api.method_get_param_count(m));
        n++;
    }
    LOGI("-- %s methods=%d --", name, n);
}

// ---------------------------------------------------------------
// 没有 set_sharedMesh：直接从 native 对象里找 mesh 指针的偏移
// IL2CPP 的 UnityEngine.Object 布局：klass(0) monitor(8) m_CachedPtr(16)
// ---------------------------------------------------------------
static void probe_native_mesh() {
    Il2CppClass* SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    if (!SMRCls) { LOGE("probe_native: no SMR"); return; }
    const MethodInfo* getMesh = find_method(SMRCls, "get_sharedMesh", 0);
    if (!getMesh) { LOGE("probe_native: no get_sharedMesh"); return; }

    std::vector<Fingerprint> list;
    collect_fingerprints(SMRCls, list);
    LOGI("probe_native: smrs=%zu", list.size());

    int shown = 0;
    for (size_t i = 0; i < list.size() && shown < 3; i++) {
        Il2CppObject* smr = list[i].smr;
        Il2CppObject* mesh = invoke_inst(getMesh, smr, nullptr);
        if (!mesh) continue;

        void* np_smr  = *(void**)((char*)smr + 16);
        void* np_mesh = *(void**)((char*)mesh + 16);
        LOGI("native[%d] %s", shown, list[i].chain.c_str());
        LOGI("  smrMg=%p smrNat=%p meshMg=%p meshNat=%p", smr, np_smr, mesh, np_mesh);
        shown++;
        if (!np_smr || !np_mesh) { LOGE("  null native ptr, skip"); continue; }

        int hits = 0;
        for (int off = 0; off < 0x300; off += 8) {
            void* v = *(void**)((char*)np_smr + off);
            if (v == np_mesh) { LOGI("  HIT mesh@0x%x", off); hits++; }
        }
        LOGI("  scan done hits=%d", hits);
    }
}

// ---------------------------------------------------------------
// 加载我们自己转好的 .mmesh，主线程里建 Mesh + GameObject 挂到角色身上
// 文件放在游戏私有目录，因为 app 读不了 /sdcard 上任意路径
// ---------------------------------------------------------------
// 悬浮窗上显示的诊断信息：主相机能看见哪些 layer、我们的模型在哪层、原角色在哪层
static char g_layer_info[192] = "model not spawned yet";
static int g_model_layer = -1;
static int g_role_layer = -1;

// 200ms 检查一次：模型还在、相机 cullingMask 又没它的层，就补回去（切场景时引擎会改）
static void ensure_model_visible() {
    if (!g_model_go) return;
    if (g_model_layer < 0) return;

    Il2CppClass* CamCls = find_class_any("UnityEngine", "Camera");
    if (!CamCls) return;
    const MethodInfo* getAll = find_method(CamCls, "get_allCameras", 0);
    const MethodInfo* getMask = find_method(CamCls, "get_cullingMask", 0);
    const MethodInfo* setMask = find_method(CamCls, "set_cullingMask", 1);
    if (!getMask || !setMask) return;

    // 遍历全部相机：战斗/主城可能不是 Camera.main 在渲染角色
    Il2CppArray* cams = nullptr;
    if (getAll) cams = (Il2CppArray*)invoke_static(getAll, nullptr);
    if (cams) {
        size_t n = cams->max_length > 16 ? 16 : cams->max_length;
        for (size_t i = 0; i < n; i++) {
            Il2CppObject* cam = *(Il2CppObject**)array_addr(cams, sizeof(void*), i);
            if (!cam) continue;
            Il2CppObject* r = invoke_inst(getMask, cam, nullptr);
            if (!r) continue;
            int cur = *(int*)((char*)r + 16);
            if ((cur & (1 << g_model_layer)) != 0) continue;
            int want = cur | (1 << g_model_layer);
            void* a[1] = { &want };
            invoke_inst(setMask, cam, a);
            LOGI("cam[%zu] mask 0x%08X -> 0x%08X", i, cur, want);
        }
        return;
    }

    // 拿不到 allCameras 就退回 Camera.main
    const MethodInfo* getMain = find_method(CamCls, "get_main", 0);
    Il2CppObject* cam = getMain ? invoke_static(getMain, nullptr) : nullptr;
    if (!cam) return;
    Il2CppObject* r = invoke_inst(getMask, cam, nullptr);
    if (!r) return;
    int cur = *(int*)((char*)r + 16);
    if ((cur & (1 << g_model_layer)) != 0) return;
    int want = cur | (1 << g_model_layer);
    void* a[1] = { &want };
    invoke_inst(setMask, cam, a);
    LOGI("camMask fix: 0x%08X -> 0x%08X", cur, want);
}

static void update_layer_info() {
    int camMask = -1;
    Il2CppClass* CamCls = find_class_any("UnityEngine", "Camera");
    if (CamCls) {
        const MethodInfo* getMain = find_method(CamCls, "get_main", 0);
        Il2CppObject* cam = getMain ? invoke_static(getMain, nullptr) : nullptr;
        if (cam) {
            const MethodInfo* getMask = find_method(CamCls, "get_cullingMask", 0);
            if (getMask) {
                Il2CppObject* r = invoke_inst(getMask, cam, nullptr);
                if (r) camMask = *(int*)((char*)r + 16);
            }
        }
    }
    snprintf(g_layer_info, sizeof(g_layer_info),
             "camMask=0x%08X\nmodelL=%d roleL=%d",
             camMask, g_model_layer, g_role_layer);
    LOGI("layerinfo: %s", g_layer_info);
}

static const char* kMmeshPaths[] = {
    "/data/user/0/com.hermes.h1game/files/custom.mmesh",   // 用户从悬浮窗选的
    "/data/data/com.hermes.h1game/files/custom.mmesh",
    "/sdcard/MT2/ModelMod/human.mmesh",
    "/storage/emulated/0/MT2/ModelMod/human.mmesh",
    "/data/user/0/com.hermes.h1game/files/human.mmesh",
    "/data/data/com.hermes.h1game/files/human.mmesh",
};

static Il2CppObject* spawn_model_object(Il2CppObject* smr) {
    FILE* f = nullptr;
    for (auto p : kMmeshPaths) {
        f = fopen(p, "rb");
        if (!f) continue;
        char m[4];
        if (fread(m, 1, 4, f) != 4 || memcmp(m, "MMSH", 4) != 0) {
            LOGE("mmesh: bad magic in %s, skip", p);
            fclose(f); f = nullptr;
            continue;
        }
        fseek(f, 0, SEEK_SET);
        LOGI("mmesh: opened %s", p);
        break;
    }
    if (!f) { LOGE("mmesh: no usable file"); return nullptr; }
    char magic[4]; uint32_t nv = 0, ni = 0, flags = 0;
    if (fread(magic, 1, 4, f) != 4) { fclose(f); LOGE("mmesh: header short"); return nullptr; }
    fread(&nv, 4, 1, f); fread(&ni, 4, 1, f); fread(&flags, 4, 1, f);
    LOGI("mmesh: nv=%u ni=%u flags=%u", nv, ni, flags);
    if (nv == 0 || nv > 300000 || ni == 0) { fclose(f); return nullptr; }

    std::vector<float> pos(nv * 3), nrm(nv * 3), uv(nv * 2);
    std::vector<uint32_t> idx(ni);
    fread(pos.data(), 4, nv * 3, f);
    // 模型自带的包围盒高度：不同来源的模型单位差异巨大（MMD 约 20 单位，Quaternius 0.08）
    float minY = 1e9f, maxY = -1e9f;
    for (uint32_t i = 0; i < nv; i++) {
        float y = pos[i * 3 + 1];
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
    }
    float modelH = maxY - minY;
    if (modelH < 0.0001f) modelH = 1.0f;
    LOGI("mmesh: bboxY %.3f .. %.3f (h=%.3f)", minY, maxY, modelH);
    fread(nrm.data(), 4, nv * 3, f);
    fread(uv.data(),  4, nv * 2, f);
    fread(idx.data(), 4, ni, f);
    fclose(f);

    Il2CppClass* MeshCls = find_class_any("UnityEngine", "Mesh");
    Il2CppClass* V3Cls   = find_class_any("UnityEngine", "Vector3");
    Il2CppClass* V2Cls   = find_class_any("UnityEngine", "Vector2");
    Il2CppClass* I32Cls  = find_class_any("System", "Int32");
    Il2CppClass* GOCls   = find_class_any("UnityEngine", "GameObject");
    Il2CppClass* MFCls   = find_class_any("UnityEngine", "MeshFilter");
    Il2CppClass* MRCls   = find_class_any("UnityEngine", "MeshRenderer");
    Il2CppClass* CompCls = find_class_any("UnityEngine", "Component");
    Il2CppClass* TransCls= find_class_any("UnityEngine", "Transform");
    if (!MeshCls || !V3Cls || !V2Cls || !I32Cls || !GOCls || !MFCls || !MRCls || !CompCls || !TransCls) {
        LOGE("mmesh: class missing M=%p V3=%p V2=%p I32=%p GO=%p MF=%p MR=%p", MeshCls, V3Cls, V2Cls, I32Cls, GOCls, MFCls, MRCls);
        return nullptr;
    }

    Il2CppObject* mesh = g_api.object_new(MeshCls);
    const MethodInfo* ctor = find_method(MeshCls, ".ctor", 0);
    if (ctor) invoke_inst(ctor, mesh, nullptr);

    Il2CppArray* va = g_api.array_new(V3Cls, nv);
    memcpy(array_addr(va, 12, 0), pos.data(), (size_t)nv * 12);
    Il2CppArray* na = g_api.array_new(V3Cls, nv);
    memcpy(array_addr(na, 12, 0), nrm.data(), (size_t)nv * 12);
    Il2CppArray* ua = g_api.array_new(V2Cls, nv);
    memcpy(array_addr(ua, 8, 0), uv.data(), (size_t)nv * 8);
    Il2CppArray* ia = g_api.array_new(I32Cls, ni);
    memcpy(array_addr(ia, 4, 0), idx.data(), (size_t)ni * 4);

    const MethodInfo* setV = find_method(MeshCls, "set_vertices", 1);
    const MethodInfo* setN = find_method(MeshCls, "set_normals", 1);
    const MethodInfo* setU = find_method(MeshCls, "set_uv", 1);
    const MethodInfo* setT = find_method(MeshCls, "set_triangles", 1);
    if (!setV || !setT) { LOGE("mmesh: setter missing V=%p T=%p", (void*)setV, (void*)setT); return nullptr; }
    { void* a[1] = { va }; invoke_inst(setV, mesh, a); }
    if (setN) { void* a[1] = { na }; invoke_inst(setN, mesh, a); }
    if (setU) { void* a[1] = { ua }; invoke_inst(setU, mesh, a); }
    { void* a[1] = { ia }; invoke_inst(setT, mesh, a); }
    LOGI("mmesh: mesh built %p", mesh);

    // GameObject + MeshFilter + MeshRenderer
    const MethodInfo* goCtor = find_method(GOCls, ".ctor", 1);
    if (!goCtor) { LOGE("mmesh: no GameObject ctor"); return nullptr; }
    void* an[1] = { g_api.string_new("ModelModBody") };
    Il2CppObject* go = g_api.object_new(GOCls);
    invoke_inst(goCtor, go, an);

    // 关键：不设 layer 的话，相机 cullingMask 里没有 Default 层就什么都不渲染
    // 直接把原角色 GameObject 的 layer 拄过来最稳
    {
        int layer = 0;
        const MethodInfo* getGO = find_method(CompCls, "get_gameObject", 0);
        Il2CppObject* sgo = (smr && getGO) ? invoke_inst(getGO, smr, nullptr) : nullptr;
        if (sgo) {
            const MethodInfo* getLayer = find_method(GOCls, "get_layer", 0);
            if (getLayer) {
                Il2CppObject* lr = invoke_inst(getLayer, sgo, nullptr);
                if (lr) layer = *(int*)((char*)lr + 16);
            }
        }
        LOGI("mmesh: source layer=%d", layer);
        g_role_layer = layer;
        const MethodInfo* setLayer = find_method(GOCls, "set_layer", 1);
        if (setLayer) { void* a[1] = { &layer }; invoke_inst(setLayer, go, a); LOGI("mmesh: layer set to %d", layer); }
        g_model_layer = layer;
    }

    const MethodInfo* addComp = find_method(GOCls, "AddComponent", 1);
    if (!addComp) { LOGE("mmesh: no AddComponent"); return nullptr; }

    void* t1[1] = { g_api.type_get_object(g_api.class_get_type(MFCls)) };
    Il2CppObject* mf = invoke_inst(addComp, go, t1);
    LOGI("mmesh: MeshFilter=%p", mf);
    if (mf) {
        const MethodInfo* setMesh = find_method(MFCls, "set_mesh", 1);
        if (setMesh) { void* a[1] = { mesh }; invoke_inst(setMesh, mf, a); }
        else LOGE("mmesh: no set_mesh");
    }

    void* t2[1] = { g_api.type_get_object(g_api.class_get_type(MRCls)) };
    Il2CppObject* mr = invoke_inst(addComp, go, t2);
    LOGI("mmesh: MeshRenderer=%p", mr);

    // 一次性地 dump 游戏骨骼名字（要主 Body，不要 _PX 描边层，那层挂的是脸部表情骨）
    {
        static int dumped = 0;
        Il2CppObject* dumpTarget = smr;
        if (!dumped) {
            std::string ch = smr ? obj_name(smr) : "";
            if (ch.find("_PX") != std::string::npos) {
                Il2CppClass* SMRc = g_smrCls ? g_smrCls : find_class_any("UnityEngine", "SkinnedMeshRenderer");
                std::vector<Fingerprint> all;
                if (SMRc) collect_fingerprints(SMRc, all);
                for (auto& ff : all) {
                    if (ff.chain.find("Body") != std::string::npos &&
                        ff.chain.find("_PX") == std::string::npos) {
                        dumpTarget = ff.smr;
                        break;
                    }
                }
            }
        }
        if (!dumped && dumpTarget) {
            dumped = 1;
            Il2CppClass* smrCls2 = g_api.object_get_class(dumpTarget);
            smr = dumpTarget;
            const MethodInfo* getBones2 = find_method(smrCls2, "get_bones", 0);
            Il2CppArray* bones = getBones2 ? (Il2CppArray*)invoke_inst(getBones2, smr, nullptr) : nullptr;
            if (bones) {
                LOGI("BONES count=%zu", bones->max_length);
                size_t lim = bones->max_length > 90 ? 90 : bones->max_length;
                for (size_t i = 0; i < lim; i++) {
                    Il2CppObject* b = *(Il2CppObject**)array_addr(bones, sizeof(void*), i);
                    if (!b) continue;
                    LOGI("BONE %zu %s", i, obj_name(b).c_str());
                }
            } else {
                LOGE("BONES: get_bones null");
            }
        }
    }

    // 一次性 dump 纹理 API
    {
        static int dumpedTex = 0;
        if (!dumpedTex) {
            dumpedTex = 1;
            dump_class_members("UnityEngine", "Texture2D");
            dump_class_members("UnityEngine", "Texture");
            dump_class_members("UnityEngine", "Material");
        }
    }

    // 材质：优先自己建一个 unlit 纯色（借来的游戏材质不会对）
    if (mr) {
        Il2CppObject* mat = nullptr;
        Il2CppClass* ShaderCls = find_class_any("UnityEngine", "Shader");
        Il2CppClass* MatCls    = find_class_any("UnityEngine", "Material");
        const MethodInfo* shFind = ShaderCls ? find_method(ShaderCls, "Find", 1) : nullptr;
        if (shFind && MatCls) {
            const char* names[] = {"Unlit/Texture", "Mobile/Diffuse", "Legacy Shaders/Diffuse",
                                   "Sprites/Default", "UI/Default", "Unlit/Color"};
            for (int i = 0; i < 6 && !mat; i++) {
                void* a[1] = { g_api.string_new(names[i]) };
                Il2CppObject* sh = invoke_static(shFind, a);
                LOGI("mmesh: shader %s = %p", names[i], sh);
                if (!sh) continue;
                mat = g_api.object_new(MatCls);
                const MethodInfo* mctor = find_method(MatCls, ".ctor", 1);
                if (mctor) { void* b[1] = { sh }; invoke_inst(mctor, mat, b); }
                const MethodInfo* setColor = find_method(MatCls, "set_color", 1);
                if (setColor) { Color4 c = {0.86f, 0.66f, 0.52f, 1.0f}; void* d[1] = { &c }; invoke_inst(setColor, mat, d); }
                LOGI("mmesh: own material %p with %s", mat, names[i]);
            }
        }
        // 拿不到 shader 就只能借原角色的
        if (!mat && smr) {
            Il2CppClass* smrCls = g_api.object_get_class(smr);
            const MethodInfo* getMat = find_method(smrCls, "get_sharedMaterial", 0);
            mat = getMat ? invoke_inst(getMat, smr, nullptr) : nullptr;
            LOGI("mmesh: fallback src material=%p", mat);
        }
        if (mat) {
            const MethodInfo* setMat = find_method(MRCls, "set_sharedMaterial", 1);
            if (setMat) { void* a[1] = { mat }; invoke_inst(setMat, mr, a); }

            // ---- 挂贴图：读 .mmtex（"MMTX"+w+h+RGBA），建 Texture2D 塞进材质 ----
            {
                FILE* tf = nullptr;
                const char* texPaths[] = {
                    "/data/user/0/com.hermes.h1game/files/custom.mmtex",
                    "/data/data/com.hermes.h1game/files/custom.mmtex",
                    "/sdcard/MT2/ModelMod/body.mmtex",
                };
                for (auto tp : texPaths) { tf = fopen(tp, "rb"); if (tf) { LOGI("tex: opened %s", tp); break; } }
                if (tf) {
                    char tm[4]; uint32_t tw = 0, th = 0;
                    if (fread(tm, 1, 4, tf) == 4 && memcmp(tm, "MMTX", 4) == 0) {
                        fread(&tw, 4, 1, tf); fread(&th, 4, 1, tf);
                        LOGI("tex: %ux%u", tw, th);
                        if (tw > 0 && th > 0 && tw <= 4096 && th <= 4096) {
                            size_t nbytes = (size_t)tw * th * 4;
                            std::vector<unsigned char> px(nbytes);
                            fread(px.data(), 1, nbytes, tf);
                            LOGI("tex: px[0..3] = %u,%u,%u,%u | %u,%u,%u,%u",
                                 px[0], px[1], px[2], px[3], px[nbytes/2], px[nbytes/2+1],
                                 px[nbytes/2+2], px[nbytes/2+3]);

                            Il2CppClass* TexCls = find_class_any("UnityEngine", "Texture2D");
                            Il2CppClass* ByteCls = find_class_any("System", "Byte");
                            if (TexCls && ByteCls) {
                                // 必须用 mipChain=false 的构造函数：默认 ctor(w,h) 带 mipmap，
                                // 而 LoadRawTextureData 只喂了 mip0，渲染时采样到垃圾 mip 就会显示灰/白
                                const MethodInfo* tctor4 = find_method(TexCls, ".ctor", 4);
                                const MethodInfo* tctor = find_method(TexCls, ".ctor", 2);
                                Il2CppObject* tex = g_api.object_new(TexCls);
                                if (tctor4) {
                                    int wi = (int)tw, hi = (int)th;
                                    int fmt = 4;          // TextureFormat.RGBA32
                                    bool mipChain = false;
                                    void* ta[4] = { &wi, &hi, &fmt, &mipChain };
                                    invoke_inst(tctor4, tex, ta);
                                    LOGI("tex: ctor4 used (RGBA32, no mip)");
                                } else if (tctor) {
                                    int wi = (int)tw, hi = (int)th;
                                    void* ta[2] = { &wi, &hi };
                                    invoke_inst(tctor, tex, ta);
                                    LOGI("tex: ctor2 fallback");
                                }
                                const MethodInfo* loadRaw = find_method(TexCls, "LoadRawTextureData", 1);
                                const MethodInfo* apply = find_method(TexCls, "Apply", 0);
                                if (tex && loadRaw) {
                                    Il2CppArray* barr = g_api.array_new(ByteCls, nbytes);
                                    memcpy(array_addr(barr, 1, 0), px.data(), nbytes);
                                    void* la[1] = { barr };
                                    invoke_inst(loadRaw, tex, la);
                                    if (apply) {
                                        const MethodInfo* apply1 = find_method(TexCls, "Apply", 1);
                                        if (apply1) { bool upd = false; void* aa[1] = { &upd }; invoke_inst(apply1, tex, aa); }
                                        else invoke_inst(apply, tex, nullptr);
                                        LOGI("tex: applied");
                                    }
                                    // 诊断：UV 样本
                                    {
                                        const MethodInfo* gm = find_method(MeshCls, "get_uv", 0);
                                        LOGI("tex: uv sample %s", gm ? "has get_uv" : "no get_uv");
                                    }
                                    // 显式按名字设一遍（有的 shader 的属性不是 _MainTex 也能兜住）
                                    {
                                        const MethodInfo* setTexStr = find_method(MatCls, "SetTexture", 2);
                                        if (setTexStr) {
                                            void* st[2] = { g_api.string_new("_MainTex"), tex };
                                            invoke_inst(setTexStr, mat, st);
                                            LOGI("tex: SetTexture(_MainTex) done");
                                        }
                                    }
                                    const MethodInfo* setMain = find_method(MatCls, "set_mainTexture", 1);
                                    if (setMain) { void* sa[1] = { tex }; invoke_inst(setMain, mat, sa); LOGI("tex: mainTexture set %p", tex); }
                                    else LOGE("tex: no set_mainTexture");

                                    // 关键：材质原来的 tint 是肉色，会和贴图相乘，刷成白色贴图才准
                                    const MethodInfo* setCol = find_method(MatCls, "set_color", 1);
                                    if (setCol) { Color4 wcol = {1.0f, 1.0f, 1.0f, 1.0f}; void* ca[1] = { &wcol }; invoke_inst(setCol, mat, ca); LOGI("tex: tint white"); }

                                    // 读回宽高确认纹理真的建出来了
                                    const MethodInfo* getW = find_method(TexCls, "get_width", 0);
                                    const MethodInfo* getH = find_method(TexCls, "get_height", 0);
                                    int rw = -1, rh = -1;
                                    if (getW) { Il2CppObject* o = invoke_inst(getW, tex, nullptr); if (o) rw = *(int*)((char*)o + 16); }
                                    if (getH) { Il2CppObject* o = invoke_inst(getH, tex, nullptr); if (o) rh = *(int*)((char*)o + 16); }
                                    LOGI("tex: readback %dx%d (ctor2=%p)", rw, rh, (void*)tctor);
                                } else {
                                    LOGE("tex: tex=%p loadRaw=%p", tex, (void*)loadRaw);
                                }
                            }
                        }
                    }
                    fclose(tf);
                } else {
                    LOGE("tex: no mmtex found");
                }
            }
        }
    }

    // 挂到角色父节点，缩放 20 倍（模型高 0.08 -> 1.6m）
    const MethodInfo* getTransGO = find_method(GOCls, "get_transform", 0);
    const MethodInfo* getTransC  = find_method(CompCls, "get_transform", 0);   // SMR 是 Component，不能走 GameObject.get_transform
    const MethodInfo* getParent  = find_method(TransCls, "get_parent", 0);
    const MethodInfo* setParent  = find_method(TransCls, "SetParent", 1);
    const MethodInfo* setLPos    = find_method(TransCls, "set_localPosition", 1);
    const MethodInfo* setLScale  = find_method(TransCls, "set_localScale", 1);
    Il2CppObject* gt = getTransGO ? invoke_inst(getTransGO, go, nullptr) : nullptr;
    LOGI("mmesh: step gt=%p setLPos=%p setLScale=%p", gt, (void*)setLPos, (void*)setLScale);

    if (gt && setLScale) { Vec3 s = {20, 20, 20}; void* a[1] = { &s }; invoke_inst(setLScale, gt, a); LOGI("mmesh: scale ok"); }
    if (gt && setLPos)   { Vec3 p = {0, 0, 0};    void* a[1] = { &p }; invoke_inst(setLPos, gt, a);   LOGI("mmesh: pos ok"); }

    Il2CppObject* st = (smr && getTransC) ? invoke_inst(getTransC, smr, nullptr) : nullptr;
    Il2CppObject* sp = (st && getParent) ? invoke_inst(getParent, st, nullptr) : nullptr;
    LOGI("mmesh: step st=%p sp=%p setParent=%p", st, sp, (void*)setParent);

    // 找角色根节点：从 SMR 往上爬，停在 PlayerRoot / AvatarPoolRoot 的直接子节点，
    // 那个节点才是会跟着角色跑的东西（挂到中间某层可能是个不动的池子节点）
    // 角色实例的命名是 12011_U_1 / 15043_H_2 / 11055_C_1 这种，
    // 池子根（VisualPool / VisualModelPool）不会动，挂在它下面就是个站桩
    Il2CppObject* roleRoot = st;
    {
        Il2CppObject* cur = st;
        for (int i = 0; i < 8 && cur; i++) {
            std::string cn = obj_name(cur);
            LOGI("mmesh: node[%d] = %s", i, cn.c_str());
            bool isRole = cn.find("_U_") != std::string::npos ||
                          cn.find("_H_") != std::string::npos ||
                          cn.find("_C_") != std::string::npos;
            if (isRole) {
                roleRoot = cur;
                LOGI("mmesh: roleRoot = %s", cn.c_str());
                break;
            }
            Il2CppObject* p = getParent ? invoke_inst(getParent, cur, nullptr) : nullptr;
            if (!p) break;
            cur = p;
        }
    }

    if (gt && roleRoot && setParent) {
        void* a[1] = { roleRoot };
        invoke_inst(setParent, gt, a);

        // 角色节点自带的缩放不是 1（很多游戏模型单位是厘米，节点缩放 0.01），
        // 直接写 localScale=20 会被父级再乘一次，直接缩到看不见
        const MethodInfo* getLossy = find_method(TransCls, "get_lossyScale", 0);
        float psc = 1.0f;
        if (getLossy) {
            Il2CppObject* ls = invoke_inst(getLossy, roleRoot, nullptr);
            if (ls) {
                Vec3 v = *(Vec3*)((char*)ls + 16);
                LOGI("mmesh: parent lossyScale = %f %f %f", v.x, v.y, v.z);
                if (v.x > 0.00001f) psc = v.x;
            }
        }
        // 自动缩放：不管模型是 0.08 单位还是 20 单位，都拉到 1.7 米高
        float k = 1.7f / (modelH * psc);
        if (setLScale) { Vec3 s = {k, k, k}; void* b[1] = { &s }; invoke_inst(setLScale, gt, b); }

        // MMD/glTF 模型多数面朝 -Z，Unity 是 +Z，转 180
        const MethodInfo* setLRot = find_method(TransCls, "set_localRotation", 1);
        if (setLRot) {
            Quat q = {0.0f, 1.0f, 0.0f, 0.0f};
            void* b[1] = { &q };
            invoke_inst(setLRot, gt, b);
            LOGI("mmesh: rotated 180 on Y");
        }

        if (g_replace_mode) {
            // 替换模式：模型站在角色原点，再把原模型的 SMR 关掉
            if (setLPos) { Vec3 p = {0, 0, 0}; void* b[1] = { &p }; invoke_inst(setLPos, gt, b); }
            if (smr && g_setEnabled) {
                uint8_t off = 0;
                void* a[1] = { &off };
                invoke_inst(g_setEnabled, smr, a);
                LOGI("mmesh: original SMR hidden (replace mode)");
            }
            LOGI("mmesh: parent set to role root %p scale=%f REPLACE", roleRoot, k);
        } else {
            // 预览模式：顶在头顶上方，不会被原模型/特效遮住
            if (setLPos) { Vec3 p = {0, 2.4f, 0}; void* b[1] = { &p }; invoke_inst(setLPos, gt, b); }
            LOGI("mmesh: parent set to role root %p scale=%f ABOVE", roleRoot, k);
        }
    }

    LOGI("mmesh: done go=%p trans=%p parent=%p", go, gt, sp);
    g_last_model_go = go;
    g_model_go = go;
    return go;
}



// ---------------------------------------------------------------
// CPU 蒙皮：读 MMS2 的骨骼权重，每 N 帧算一次顶点
// ---------------------------------------------------------------
static const MethodInfo* find_method_any(Il2CppClass* klass, const char* name);
static int array_elem_size(Il2CppClass* k, int fallback);

static std::string norm_bone(const std::string& in, bool stripBip);

// 硬映射：只对 11001 这名角色有效，名字对不上时兜底
// 依据 11001_U_1/Body 的骨骼树：
//   L Calf → Bone19(脚) → Bone18(脚趾) → Bone17
//   R Calf → Bone16(脚) → Bone15(脚趾) → Bone14
//   Bone57 是独立根，在左手末端；Bone38 在右手末端
static const struct { const char* target; int slot; } kHardMap[] = {
    {"bone19",  8},   // L Foot
    {"bone18",  9},   // L Toe0
    {"bone16", 12},   // R Foot
    {"bone15", 13},   // R Toe0
    {"bone57", 17},   // L Hand
    {"bone38", 21},   // R Hand
};

// ---------------------------------------------------------------
// v144: 从游戏自己的 bindposes 收集每根骨骼的 bind 逆矩阵
//
// 这才是蒙皮缺的那块。之前所有方案都只有平移（x - 骨骼位置），
// bind 姿态的旋转整个丢了 —— 所以在 bind pose 下算出来就已经歪了。
// bindposes[i] = inverse(bone.localToWorldMatrix at bind)，完整 4x4，
// 直接拿来当 B^-1 用，一行都不用自己推。
// 实测这个数组可读（bindref 已验证 1201 根骨骼），不像 get_vertices。
// ---------------------------------------------------------------
static std::map<void*, std::vector<float>> g_bindInv;
static volatile int g_bindInv_built = 0;

static void build_bindinv_map() {
    g_bindInv.clear();
    Il2CppClass* SMRCls    = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    Il2CppClass* ObjectCls = find_class_any("UnityEngine", "Object");
    Il2CppClass* MeshCls   = find_class_any("UnityEngine", "Mesh");
    Il2CppClass* Mat4Cls   = find_class_any("UnityEngine", "Matrix4x4");
    if (!SMRCls || !ObjectCls || !MeshCls) { LOGE("bindinv: class missing"); return; }
    const MethodInfo* find = find_method(ObjectCls, "FindObjectsOfType", 1);
    const MethodInfo* gb   = find_method_any(SMRCls, "get_bones");
    const MethodInfo* gm   = find_method_any(SMRCls, "get_sharedMesh");
    const MethodInfo* gbp  = find_method_any(MeshCls, "get_bindposes");
    if (!find || !gb || !gm || !gbp) { LOGE("bindinv: method missing"); return; }
    int mstride = array_elem_size(Mat4Cls, 64);
    void* a[1] = { g_api.type_get_object(g_api.class_get_type(SMRCls)) };
    Il2CppArray* arr = (Il2CppArray*)invoke_static(find, a);
    // 场景里 23/40 个 SMR 是未激活的池子对象，FindObjectsOfType 拿不到它们，
    // 而我们要找的骨骼很可能就在那些 SMR 的 bones[] 里
    if (!arr || arr->max_length == 0) {
        Il2CppClass* ResCls = find_class_any("UnityEngine", "Resources");
        const MethodInfo* fa = ResCls ? find_method(ResCls, "FindObjectsOfTypeAll", 1) : nullptr;
        if (fa) {
            arr = (Il2CppArray*)invoke_static(fa, a);
            LOGI("bindinv: switched to Resources.FindObjectsOfTypeAll");
        }
    }
    if (!arr) { LOGE("bindinv: FindObjectsOfType null"); return; }
    int n = 0, smrs = 0;
    for (size_t i = 0; i < arr->max_length; i++) {
        Il2CppObject* smr = *(Il2CppObject**)array_addr(arr, sizeof(void*), i);
        if (!smr) continue;
        Il2CppArray* bones = (Il2CppArray*)invoke_inst(gb, smr, nullptr);
        Il2CppObject* mesh  = invoke_inst(gm, smr, nullptr);
        if (!bones || !mesh || bones->max_length == 0) continue;
        Il2CppArray* bps = (Il2CppArray*)invoke_inst(gbp, mesh, nullptr);
        if (!bps || bps->max_length != bones->max_length) continue;
        smrs++;
        for (size_t k = 0; k < bones->max_length; k++) {
            Il2CppObject* b = *(Il2CppObject**)array_addr(bones, sizeof(void*), k);
            if (!b) continue;
            if (g_bindInv.find((void*)b) != g_bindInv.end()) continue;
            const float* m = (const float*)array_addr(bps, (size_t)mstride, k);
            std::vector<float> v(16);
            for (int q = 0; q < 16; q++) v[q] = m[q];
            g_bindInv[(void*)b] = v;
            n++;
        }
    }
    g_bindInv_built = 1;
    LOGI("bindinv: %d bones from %d SMRs (bindposes readable=%s)", n, smrs, n > 0 ? "YES" : "NO");
}

// 列主序 4x4：out = a * b
static void mat4_mul(const float* a, const float* b, float* out) {
    float t[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            t[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] +
                           a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    for (int i = 0; i < 16; i++) out[i] = t[i];
}

// 列主序仿射 4x4 求逆（最后一行必须是 0,0,0,1）
static bool mat4_inv_affine(const float* m, float* out) {
    const float* c0 = m + 0;
    const float* c1 = m + 4;
    const float* c2 = m + 8;
    const float* c3 = m + 12;
    float det = c0[0] * (c1[1] * c2[2] - c2[1] * c1[2])
              - c1[0] * (c0[1] * c2[2] - c2[1] * c0[2])
              + c2[0] * (c0[1] * c1[2] - c1[1] * c0[2]);
    if (fabsf(det) < 1e-12f) return false;
    float id = 1.0f / det;
    float i00 = (c1[1] * c2[2] - c2[1] * c1[2]) * id;
    float i10 = -(c0[1] * c2[2] - c2[1] * c0[2]) * id;
    float i20 = (c0[1] * c1[2] - c1[1] * c0[2]) * id;
    float i01 = -(c1[0] * c2[2] - c2[0] * c1[2]) * id;
    float i11 = (c0[0] * c2[2] - c2[0] * c0[2]) * id;
    float i21 = -(c0[0] * c1[2] - c1[0] * c0[2]) * id;
    float i02 = (c1[0] * c2[1] - c2[0] * c1[1]) * id;
    float i12 = -(c0[0] * c2[1] - c2[0] * c0[1]) * id;
    float i22 = (c0[0] * c1[1] - c1[0] * c0[1]) * id;
    out[0] = i00; out[1] = i01; out[2] = i02; out[3] = 0;
    out[4] = i10; out[5] = i11; out[6] = i12; out[7] = 0;
    out[8] = i20; out[9] = i21; out[10] = i22; out[11] = 0;
    out[12] = -(i00 * c3[0] + i01 * c3[1] + i02 * c3[2]);
    out[13] = -(i10 * c3[0] + i11 * c3[1] + i12 * c3[2]);
    out[14] = -(i20 * c3[0] + i21 * c3[1] + i22 * c3[2]);
    out[15] = 1;
    return true;
}

struct SkinPart {
    Il2CppObject* mesh = nullptr;
    int nv = 0;
    std::vector<float> basePos;      // 原始顶点（模型空间）
    std::vector<uint8_t> joints;     // nv*4
    std::vector<float> weights;      // nv*4
    std::vector<float> bindT;        // nb*3 每骨骼 bind 平移（旧，纯平移版）
    std::vector<float> bindInv;      // nb*16 每骨骼 bind 逆矩阵（新，完整 4x4）
    std::vector<Il2CppObject*> bones;// 游戏骨骼 Transform
    Il2CppArray* varray = nullptr;   // 复用顶点数组，避免每帧 array_new 撑爆 GC
    Il2CppObject* root = nullptr;    // 挂在哪个 root 下（算相对矩阵用）
    float scaleK = 1.0f;             // PMX → 游戏单位的缩放
    bool basePrepared = false;       // basePos 是否已经转成游戏空间
    std::vector<float> outBuf;
    std::vector<float> lastMats;     // 上一帧骨骼矩阵，没变就跳过 set_vertices
};
static std::vector<SkinPart> g_skinParts;
static std::vector<std::string> g_lastBoneNames;

// 递归找子 Transform（按名字）
static Il2CppObject* find_child_by_name(Il2CppObject* root, const char* name, int depth = 0) {
    if (!root || depth > 8) return nullptr;
    Il2CppClass* TransCls = find_class_any("UnityEngine", "Transform");
    if (!TransCls) return nullptr;
    std::string rn = obj_name(root);
    if (rn == name) return root;
    const MethodInfo* getCount = find_method(TransCls, "get_childCount", 0);
    const MethodInfo* getChild = find_method(TransCls, "GetChild", 1);
    if (!getCount || !getChild) return nullptr;
    Il2CppObject* cobj = invoke_inst(getCount, root, nullptr);
    int cnt = cobj ? *(int*)((char*)cobj + 16) : 0;
    for (int i = 0; i < cnt && i < 200; i++) {
        void* a[1] = { &i };
        Il2CppObject* ch = invoke_inst(getChild, root, a);
        if (!ch) continue;
        Il2CppObject* r = find_child_by_name(ch, name, depth + 1);
        if (r) return r;
    }
    return nullptr;
}

// 读 MMS2 的骨骼部分（在 spawn_one_part 读完顶点/索引之后调用）
static bool load_skin_part(FILE* f, Il2CppObject* mesh, int nv, int ni,
                           Il2CppObject* roleRoot, float scaleK, Il2CppObject* smr) {
    uint32_t nb = 0;
    if (fread(&nb, 4, 1, f) != 1 || nb == 0 || nb > 128) return false;

    std::vector<std::string> names;
    for (uint32_t i = 0; i < nb; i++) {
        uint32_t len = 0;
        if (fread(&len, 4, 1, f) != 1 || len > 128) return false;
        std::vector<char> buf(len + 1, 0);
        if (fread(buf.data(), 1, len, f) != len) return false;
        names.push_back(std::string(buf.data(), len));
    }
    uint32_t jc = 0;
    if (fread(&jc, 4, 1, f) != 1) return false;
    if (jc != nb) return false;

    std::vector<uint8_t> joints((size_t)nv * 4);
    std::vector<float> weights((size_t)nv * 4);
    for (int i = 0; i < nv; i++) {
        uint8_t j4[4]; float w4[4];
        if (fread(j4, 1, 4, f) != 4) return false;
        if (fread(w4, 4, 4, f) != 4) return false;
        for (int k = 0; k < 4; k++) { joints[i*4+k] = j4[k]; weights[i*4+k] = w4[k]; }
    }
    std::vector<float> ibm((size_t)nb * 16);
    if (fread(ibm.data(), 4, (size_t)nb * 16, f) != (size_t)nb * 16) return false;

    SkinPart sp;
    sp.mesh = mesh;
    sp.root = roleRoot;
    sp.scaleK = (scaleK > 0.0f) ? scaleK : 1.0f;
    sp.nv = nv;
    sp.joints = joints;
    sp.weights = weights;
    sp.bindT.resize((size_t)nb * 3);
    for (uint32_t i = 0; i < nb; i++) {
        sp.bindT[i*3+0] = -ibm[i*16+12];
        sp.bindT[i*3+1] = -ibm[i*16+13];
        sp.bindT[i*3+2] = -ibm[i*16+14];
    }
    // ---- 找游戏骨骼 ----
    // v148：优先用这个 SMR 自己的 bones[]。
    // 原因：bindposes 和 SMR.bones[] 是按下标一一对应的，
    // 在角色层级里按名字瞎找（find_child_by_name）会拿到同名的另一个 Transform，
    // 那父指针就不在 bindposes 那本字典里 —— 上一版四肢 14 根全走 fallback 就是这个。
    Il2CppArray* smrBones = nullptr;
    {
        Il2CppClass* SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
        if (smr && SMRCls) {
            const MethodInfo* gb = find_method_any(SMRCls, "get_bones");
            if (gb) smrBones = (Il2CppArray*)invoke_inst(gb, smr, nullptr);
        }
    }
    size_t smrNB = (smrBones && smrBones->max_length < 4096) ? smrBones->max_length : 0;
    LOGI("skin: smr bones=%zu, our names=%u", smrNB, nb);

    std::vector<int> hit(nb, -1);
    if (smrNB > 0) {
        for (uint32_t i = 0; i < nb; i++) {
            std::string want = norm_bone(names[i], false);
            for (size_t j = 0; j < smrNB; j++) {
                Il2CppObject* t = *(Il2CppObject**)array_addr(smrBones, sizeof(void*), j);
                if (!t) continue;
                if (norm_bone(obj_name(t), false) == want) { hit[i] = (int)j; break; }
            }
        }
        // 名字对不上的，用硬映射再捞一遍
        int hard = 0;
        for (size_t h = 0; h < sizeof(kHardMap) / sizeof(kHardMap[0]); h++) {
            int slot = kHardMap[h].slot;
            if (slot < 0 || slot >= (int)nb || hit[slot] >= 0) continue;
            for (size_t j = 0; j < smrNB; j++) {
                Il2CppObject* t = *(Il2CppObject**)array_addr(smrBones, sizeof(void*), j);
                if (!t) continue;
                if (norm_bone(obj_name(t), false) == kHardMap[h].target) {
                    hit[slot] = (int)j;
                    hard++;
                    LOGI("skin: hardmap slot %d <- %s", slot, kHardMap[h].target);
                    break;
                }
            }
        }
        int nameHit = 0;
        for (uint32_t i = 0; i < nb; i++) if (hit[i] >= 0) nameHit++;
        LOGI("skin: name hits %d / %u (hardmap added %d)", nameHit, nb, hard);
    }

    // 原 mesh 的 bounds —— vertices 读不了，但这个属性通常还在，能给出角色真实尺寸
    if (smr) {
        Il2CppClass* SMRCls0 = find_class_any("UnityEngine", "SkinnedMeshRenderer");
        Il2CppClass* MeshCls0 = find_class_any("UnityEngine", "Mesh");
        const MethodInfo* gm0 = SMRCls0 ? find_method_any(SMRCls0, "get_sharedMesh") : nullptr;
        Il2CppObject* mesh0 = gm0 ? invoke_inst(gm0, smr, nullptr) : nullptr;
        const MethodInfo* gb0 = MeshCls0 ? find_method_any(MeshCls0, "get_bounds") : nullptr;
        Il2CppObject* bd = (gb0 && mesh0) ? invoke_inst(gb0, mesh0, nullptr) : nullptr;
        if (bd) {
            // Bounds { Vector3 center; Vector3 extents; } 各 3 个 float，装箱后 offset 16 起
            const float* f = (const float*)((char*)bd + 16);
            LOGI("meshbounds: center=(%.3f,%.3f,%.3f) extents=(%.3f,%.3f,%.3f) height=%.3f",
                 f[0], f[1], f[2], f[3], f[4], f[5], f[4] * 2.0f);
        } else {
            LOGI("meshbounds: bounds unreadable (get_bounds=%p)", (void*)gb0);
        }
    }

    int found = 0, withBind = 0;
    sp.bindInv.resize((size_t)nb * 16, 0.0f);
    for (uint32_t i = 0; i < nb; i++) {
        Il2CppObject* b = nullptr;
        if (hit[i] >= 0 && smrBones)
            b = *(Il2CppObject**)array_addr(smrBones, sizeof(void*), (size_t)hit[i]);
        if (!b) b = find_child_by_name(roleRoot, names[i].c_str());   // 最后兜底
        sp.bones.push_back(b);
        // 默认兜底：纯平移（= 老行为）
        float* bi = &sp.bindInv[i * 16];
        for (int q = 0; q < 4; q++) bi[q * 5] = 1.0f;
        bi[12] = -sp.bindT[i * 3 + 0];
        bi[13] = -sp.bindT[i * 3 + 1];
        bi[14] = -sp.bindT[i * 3 + 2];
        if (b) {
            found++;
            std::map<void*, std::vector<float>>::iterator it = g_bindInv.find((void*)b);
            if (it != g_bindInv.end()) {
                for (int q = 0; q < 16; q++) bi[q] = it->second[q];
                withBind++;
            } else {
                // 游戏骨骼里没有这根（bonedb 已确认 11001 没有 Spine2、手臂链在
                // UpperArm 之后断开，没有 Forearm）。顺着 Transform 父链往上找，
                // 借最近一个有 bindInv 的。代价是那一段丢一次弯曲，但不会撕。
                Il2CppClass* TransCls = find_class_any("UnityEngine", "Transform");
                const MethodInfo* gp = TransCls ? find_method_any(TransCls, "get_parent") : nullptr;
                Il2CppObject* cur = b;
                int borrowed = 0;
                for (int up = 0; up < 8 && cur && gp; up++) {
                    Il2CppObject* par = invoke_inst(gp, cur, nullptr);
                    if (!par) break;
                    cur = par;
                    std::map<void*, std::vector<float>>::iterator pit = g_bindInv.find((void*)cur);
                    if (pit != g_bindInv.end()) { it = pit; borrowed = up + 1; break; }
                }
                if (it != g_bindInv.end()) {
                    for (int q = 0; q < 16; q++) bi[q] = it->second[q];
                    withBind++;
                    LOGI("skin: bone[%u] %s bindInv borrowed from parent up %d",
                         i, names[i].c_str(), borrowed);
                }
            }
        }
    }

    // ---- 骨架对拍 ----
    // 我们的 bind 位置来自 MMS2 的 inverseBindMatrix 平移（PMX 单位，身高 20.2）
    // 游戏的 bind 位置 = bindposes 求逆后的平移（游戏单位）
    // ratio 一致 = 只是缩放问题；ratio 乱 = 骨架本身不一样
    for (uint32_t i = 0; i < nb; i++) {
        std::map<void*, std::vector<float>>::iterator cit =
            sp.bones[i] ? g_bindInv.find((void*)sp.bones[i]) : g_bindInv.end();
        if (cit == g_bindInv.end()) continue;
        float minv[16];
        if (!mat4_inv_affine(cit->second.data(), minv)) continue;
        float ox = sp.bindT[i*3+0], oy = sp.bindT[i*3+1], oz = sp.bindT[i*3+2];
        float gx = minv[12], gy = minv[13], gz = minv[14];
        float on = sqrtf(ox*ox + oy*oy + oz*oz);
        float gn = sqrtf(gx*gx + gy*gy + gz*gz);
        // M_bind = bindposes^-1，取 3x3 每列长度 = 该轴的世界缩放
        float s0 = sqrtf(minv[0]*minv[0] + minv[1]*minv[1] + minv[2]*minv[2]);
        float s1 = sqrtf(minv[4]*minv[4] + minv[5]*minv[5] + minv[6]*minv[6]);
        float s2 = sqrtf(minv[8]*minv[8] + minv[9]*minv[9] + minv[10]*minv[10]);
        // 旋转是不是单位：3x3 归一化后跟单位阵的偏差
        float dev = 0.0f;
        if (s0 > 1e-6f && s1 > 1e-6f && s2 > 1e-6f) {
            float r[3][3] = {
                {minv[0]/s0, minv[1]/s0, minv[2]/s0},
                {minv[4]/s1, minv[5]/s1, minv[6]/s1},
                {minv[8]/s2, minv[9]/s2, minv[10]/s2},
            };
            // 列主序：r[c][row]。单位阵是 r[0][0]=r[1][1]=r[2][2]=1
            dev = fabsf(r[0][0]-1) + fabsf(r[1][1]-1) + fabsf(r[2][2]-1)
                + fabsf(r[0][1]) + fabsf(r[0][2]) + fabsf(r[1][0])
                + fabsf(r[1][2]) + fabsf(r[2][0]) + fabsf(r[2][1]);
        }
        LOGI("bindpos: %2u %-20s ours=(%7.2f,%7.2f,%7.2f)|%6.2f| game=(%7.3f,%7.3f,%7.3f)|%6.3f| ratio=%.3f scale=(%.2f,%.2f,%.2f) rotdev=%.3f",
             i, names[i].c_str(), ox, oy, oz, on, gx, gy, gz, gn,
             (gn > 1e-6f) ? (on / gn) : 0.0f, s0, s1, s2, dev);
    }

    // 连父链都借不到的（Spine2 连对象都没找到），按硬指定父骨骼借
    static const struct { int slot; int parentSlot; } kBorrow[] = {
        { 3,  2},   // Spine2    <- Spine1
        {16, 15},   // L Forearm <- L UpperArm
        {20, 19},   // R Forearm <- R UpperArm
    };
    for (size_t h = 0; h < sizeof(kBorrow) / sizeof(kBorrow[0]); h++) {
        int s = kBorrow[h].slot, ps = kBorrow[h].parentSlot;
        if (s < 0 || s >= (int)nb || ps < 0 || ps >= (int)nb) continue;
        float* si = &sp.bindInv[s * 16];
        // 判断是不是还在吃纯平移兜底：单位旋转 + 只有平移
        bool fallback = true;
        for (int q = 0; q < 16 && fallback; q++) {
            float want = (q == 0 || q == 5 || q == 10 || q == 15) ? 1.0f : 0.0f;
            if (q == 12 || q == 13 || q == 14) continue;   // 平移位不比
            if (fabsf(si[q] - want) > 1e-4f) fallback = false;
        }
        if (!fallback) continue;
        const float* pi = &sp.bindInv[ps * 16];
        for (int q = 0; q < 16; q++) si[q] = pi[q];
        LOGI("skin: slot %d bindInv hard-borrowed from slot %d", s, ps);
    }
    LOGI("skin: %d bones, found %d/%u, real bindInv %d", nv, found, nb, withBind);
    for (uint32_t i = 0; i < nb; i++) {
        std::map<void*, std::vector<float>>::iterator it =
            sp.bones[i] ? g_bindInv.find((void*)sp.bones[i]) : g_bindInv.end();
        LOGI("skin:   bone[%2u] %-22s obj=%s bindInv=%s", i, names[i].c_str(),
             sp.bones[i] ? "OK" : "MISS", it != g_bindInv.end() ? "OK" : "fallback");
    }
    g_skinParts.push_back(sp);
    g_lastBoneNames = names;
    return found > 0;
}

// Unity 对象被销毁后 m_CachedPtr 会归零，这时候碰它必崩
static bool obj_alive(Il2CppObject* o) {
    if (!o) return false;
    void* np = *(void**)((char*)o + 16);
    return np != nullptr;
}

// 每帧把蒙皮后的顶点写回 mesh
static void update_skin_parts() {
    if (g_skinParts.empty()) return;
    Il2CppClass* MeshCls = find_class_any("UnityEngine", "Mesh");
    Il2CppClass* V3Cls   = find_class_any("UnityEngine", "Vector3");
    Il2CppClass* TransCls= find_class_any("UnityEngine", "Transform");
    if (!MeshCls || !V3Cls || !TransCls) return;

    const MethodInfo* getL2W   = find_method(TransCls, "get_localToWorldMatrix", 0);
    const MethodInfo* setVerts = find_method(MeshCls, "set_vertices", 1);
    const MethodInfo* getV     = find_method(MeshCls, "get_vertices", 0);
    if (!getL2W || !setVerts) return;

    // 场景切换会把挂上去的对象销毁掉，先把死掉的 part 清掉
    for (size_t i = g_skinParts.size(); i-- > 0; ) {
        if (!obj_alive(g_skinParts[i].mesh)) {
            LOGI("skin: part %zu dead, removed", i);
            g_skinParts.erase(g_skinParts.begin() + i);
        }
    }

    int totalPushed = 0;
    for (auto& sp : g_skinParts) {
        if (!sp.mesh) continue;

        // ---- 1) 原始顶点，从 PMX 空间转到游戏空间 ----
        // 静态路径靠 GameObject 的 localScale=k 和 localRotation=Ry180 做这件事；
        // CPU 蒙皮要自己算，因为顶点马上要跟游戏骨骼的世界矩阵相乘。
        if (sp.basePos.empty()) {
            if (!getV) continue;
            Il2CppArray* arr = (Il2CppArray*)invoke_inst(getV, sp.mesh, nullptr);
            if (!arr || arr->max_length != (size_t)sp.nv) {
                LOGE("skin: get_vertices back failed len=%ld expect %d",
                     arr ? (long)arr->max_length : -1L, sp.nv);
                continue;
            }
            sp.basePos.resize((size_t)sp.nv * 3);
            memcpy(sp.basePos.data(), array_addr(arr, 12, 0), (size_t)sp.nv * 12);
            sp.basePrepared = false;
        }
        if (!sp.basePrepared) {
            float k = sp.scaleK;
            // Ry180： (x, y, z) -> (-x, y, -z)
            for (int v = 0; v < sp.nv; v++) {
                sp.basePos[v*3+0] *= -k;
                sp.basePos[v*3+1] *=  k;
                sp.basePos[v*3+2] *= -k;
            }
            sp.basePrepared = true;
            LOGI("skin: %d verts mapped to game space (k=%.5f, Ry180)", sp.nv, k);
        }

        // ---- 2) 角色 root 的逆矩阵（骨骼矩阵要变成相对 root 的）----
        float rootInv[16];
        bool haveRoot = false;
        if (sp.root && getL2W) {
            Il2CppObject* rm = invoke_inst(getL2W, sp.root, nullptr);
            if (rm) haveRoot = mat4_inv_affine((const float*)((char*)rm + 16), rootInv);
        }

        // ---- 3) 每根骨骼的蒙皮矩阵 = root^-1 * M_world * BindInv ----
        size_t nb = sp.bones.size();
        if (sp.bindInv.size() < nb * 16) continue;
        std::vector<float> mats(nb * 16, 0.0f);
        for (size_t b = 0; b < nb; b++) {
            float* dst = &mats[b * 16];
            if (!sp.bones[b] || !obj_alive(sp.bones[b])) { dst[0] = 1; dst[5] = 1; dst[10] = 1; dst[15] = 1; continue; }
            Il2CppObject* wm = invoke_inst(getL2W, sp.bones[b], nullptr);
            if (!wm) { dst[0] = 1; dst[5] = 1; dst[10] = 1; dst[15] = 1; continue; }
            const float* M = (const float*)((char*)wm + 16);
            float Mrel[16];
            if (haveRoot) mat4_mul(rootInv, M, Mrel);
            else for (int q = 0; q < 16; q++) Mrel[q] = M[q];
            // 先乘 bind 逆矩阵：skin = Mrel * BindInv
            mat4_mul(Mrel, &sp.bindInv[b * 16], dst);
        }

        // 骨骼没动就不刷顶点，省一半开销
        if (sp.lastMats.size() == mats.size() &&
            memcmp(sp.lastMats.data(), mats.data(), mats.size() * sizeof(float)) == 0) {
            continue;
        }
        sp.lastMats = mats;

        // ---- 4) 线性蒙皮 out = Σ w_i * (M_i * BindInv_i * v) ----
        if (sp.outBuf.size() < (size_t)sp.nv * 3) sp.outBuf.resize((size_t)sp.nv * 3);
        for (int v = 0; v < sp.nv; v++) {
            float x = sp.basePos[v*3+0], y = sp.basePos[v*3+1], z = sp.basePos[v*3+2];
            float ox = 0, oy = 0, oz = 0;
            for (int k = 0; k < 4; k++) {
                float w = sp.weights[v*4+k];
                if (w <= 0.0001f) continue;
                int b = sp.joints[v*4+k];
                if (b < 0 || (size_t)b >= nb) continue;
                const float* m = &mats[b * 16];
                ox += w * (m[0]*x + m[4]*y + m[8]*z  + m[12]);
                oy += w * (m[1]*x + m[5]*y + m[9]*z  + m[13]);
                oz += w * (m[2]*x + m[6]*y + m[10]*z + m[14]);
            }
            sp.outBuf[v*3+0] = ox;
            sp.outBuf[v*3+1] = oy;
            sp.outBuf[v*3+2] = oz;
        }

        // v147: 每次重新分配。之前缓存 sp.varray 复用，看着省事，
        // 但那个 Il2CppArray 只有 C++ 侧一个裸指针指着它，托管 GC 完全不知道，
        // 几帧之后就被回收 —— memcpy 写到野内存上，必崩（v109-v113 的"崩溃"就是它）
        Il2CppArray* varr = g_api.array_new(V3Cls, sp.nv);
        if (!varr) continue;
        memcpy(array_addr(varr, 12, 0), sp.outBuf.data(), (size_t)sp.nv * 12);
        void* a[1] = { varr };
        invoke_inst(setVerts, sp.mesh, a);
        sp.varray = nullptr;
        totalPushed++;
    }
    static int dbg = 0;
    if (totalPushed && (dbg++ % 120) == 0) LOGI("skin: pushed %d parts", totalPushed);
}

// ---------------------------------------------------------------
// 多部位加载：custom_0..custom_3 每个都是一个材质组 + 自己的贴图
// ---------------------------------------------------------------
static Il2CppObject* spawn_one_part(Il2CppObject* smr, Il2CppObject* roleRoot,
                                    const char* meshPath, const char* texPath, float scaleK) {
    FILE* f = fopen(meshPath, "rb");
    if (!f) { LOGE("part: open fail %s", meshPath); return nullptr; }
    char magic[4]; uint32_t nv = 0, ni = 0, flags = 0;
    if (fread(magic, 1, 4, f) != 4) { fclose(f); return nullptr; }
    bool isSkinPart = (memcmp(magic, "MMS2", 4) == 0);
    if (!isSkinPart && memcmp(magic, "MMSH", 4) != 0) { fclose(f); return nullptr; }
    fread(&nv, 4, 1, f); fread(&ni, 4, 1, f); fread(&flags, 4, 1, f);
    if (nv == 0 || nv > 300000 || ni == 0) { fclose(f); return nullptr; }

    std::vector<float> pos(nv * 3), nrm(nv * 3), uv(nv * 2);
    std::vector<uint32_t> idx(ni);
    fread(pos.data(), 4, nv * 3, f);
    fread(nrm.data(), 4, nv * 3, f);
    fread(uv.data(),  4, nv * 2, f);
    fread(idx.data(), 4, ni, f);
    LOGI("part: %s nv=%u ni=%u skin=%d", meshPath, nv, ni, (int)isSkinPart);

    Il2CppClass* MeshCls = find_class_any("UnityEngine", "Mesh");
    Il2CppClass* V3Cls   = find_class_any("UnityEngine", "Vector3");
    Il2CppClass* V2Cls   = find_class_any("UnityEngine", "Vector2");
    Il2CppClass* I32Cls  = find_class_any("System", "Int32");
    Il2CppClass* ByteCls = find_class_any("System", "Byte");
    Il2CppClass* GOCls   = find_class_any("UnityEngine", "GameObject");
    Il2CppClass* MFCls   = find_class_any("UnityEngine", "MeshFilter");
    Il2CppClass* MRCls   = find_class_any("UnityEngine", "MeshRenderer");
    Il2CppClass* CompCls = find_class_any("UnityEngine", "Component");
    Il2CppClass* TransCls= find_class_any("UnityEngine", "Transform");
    Il2CppClass* MatCls  = find_class_any("UnityEngine", "Material");
    Il2CppClass* ShaderCls = find_class_any("UnityEngine", "Shader");
    Il2CppClass* TexCls  = find_class_any("UnityEngine", "Texture2D");
    if (!MeshCls || !V3Cls || !V2Cls || !I32Cls || !GOCls || !MFCls || !MRCls || !CompCls || !TransCls || !MatCls) {
        LOGE("part: class missing");
        return nullptr;
    }

    Il2CppObject* mesh = g_api.object_new(MeshCls);
    const MethodInfo* ctor = find_method(MeshCls, ".ctor", 0);
    if (ctor) invoke_inst(ctor, mesh, nullptr);
    Il2CppArray* va = g_api.array_new(V3Cls, nv); memcpy(array_addr(va, 12, 0), pos.data(), (size_t)nv * 12);
    Il2CppArray* na = g_api.array_new(V3Cls, nv); memcpy(array_addr(na, 12, 0), nrm.data(), (size_t)nv * 12);
    Il2CppArray* ua = g_api.array_new(V2Cls, nv); memcpy(array_addr(ua, 8, 0),  uv.data(),  (size_t)nv * 8);
    Il2CppArray* ia = g_api.array_new(I32Cls, ni); memcpy(array_addr(ia, 4, 0), idx.data(), (size_t)ni * 4);
    const MethodInfo* setV = find_method(MeshCls, "set_vertices", 1);
    const MethodInfo* setN = find_method(MeshCls, "set_normals", 1);
    const MethodInfo* setU = find_method(MeshCls, "set_uv", 1);
    const MethodInfo* setT = find_method(MeshCls, "set_triangles", 1);
    if (setV) { void* a[1] = { va }; invoke_inst(setV, mesh, a); }
    if (setN) { void* a[1] = { na }; invoke_inst(setN, mesh, a); }
    if (setU) { void* a[1] = { ua }; invoke_inst(setU, mesh, a); }
    if (setT) { void* a[1] = { ia }; invoke_inst(setT, mesh, a); }

    if (isSkinPart && mesh) {
        load_skin_part(f, mesh, (int)nv, (int)ni, roleRoot, scaleK, smr);
    }
    fclose(f);

    const MethodInfo* goCtor = find_method(GOCls, ".ctor", 1);
    if (!goCtor) return nullptr;
    void* an[1] = { g_api.string_new("ModelModPart") };
    Il2CppObject* go = g_api.object_new(GOCls);
    invoke_inst(goCtor, go, an);

    // layer 跟角色一致
    int layer = 0;
    const MethodInfo* getGO = find_method(CompCls, "get_gameObject", 0);
    Il2CppObject* sgo = (smr && getGO) ? invoke_inst(getGO, smr, nullptr) : nullptr;
    if (sgo) {
        const MethodInfo* getLayer = find_method(GOCls, "get_layer", 0);
        if (getLayer) { Il2CppObject* lr = invoke_inst(getLayer, sgo, nullptr); if (lr) layer = *(int*)((char*)lr + 16); }
    }
    const MethodInfo* setLayer = find_method(GOCls, "set_layer", 1);
    if (setLayer) { void* a[1] = { &layer }; invoke_inst(setLayer, go, a); }

    const MethodInfo* addComp = find_method(GOCls, "AddComponent", 1);
    if (!addComp) return nullptr;
    void* t1[1] = { g_api.type_get_object(g_api.class_get_type(MFCls)) };
    Il2CppObject* mf = invoke_inst(addComp, go, t1);
    if (mf) {
        const MethodInfo* setMesh = find_method(MFCls, "set_mesh", 1);
        if (setMesh) { void* a[1] = { mesh }; invoke_inst(setMesh, mf, a); }
    }
    void* t2[1] = { g_api.type_get_object(g_api.class_get_type(MRCls)) };
    Il2CppObject* mr = invoke_inst(addComp, go, t2);

    // 材质：Unlit/Texture + 自己的贴图
    if (mr) {
        Il2CppObject* mat = nullptr;
        const MethodInfo* shFind = ShaderCls ? find_method(ShaderCls, "Find", 1) : nullptr;
        if (shFind) {
            const char* sn[] = {"Unlit/Texture", "Mobile/Diffuse", "Sprites/Default"};
            for (int i = 0; i < 3 && !mat; i++) {
                void* a[1] = { g_api.string_new(sn[i]) };
                Il2CppObject* sh = invoke_static(shFind, a);
                if (!sh) continue;
                mat = g_api.object_new(MatCls);
                const MethodInfo* mctor = find_method(MatCls, ".ctor", 1);
                if (mctor) { void* b[1] = { sh }; invoke_inst(mctor, mat, b); }
                LOGI("part: shader %s", sn[i]);
            }
        }
        if (mat) {
            const MethodInfo* setMat = find_method(MRCls, "set_sharedMaterial", 1);
            if (setMat) { void* a[1] = { mat }; invoke_inst(setMat, mr, a); }

            FILE* tf = fopen(texPath, "rb");
            if (tf) {
                char tm[4]; uint32_t tw = 0, th = 0;
                if (fread(tm, 1, 4, tf) == 4 && memcmp(tm, "MMTX", 4) == 0) {
                    fread(&tw, 4, 1, tf); fread(&th, 4, 1, tf);
                    if (tw > 0 && th > 0 && tw <= 4096 && th <= 4096) {
                        size_t nbytes = (size_t)tw * th * 4;
                        std::vector<unsigned char> px(nbytes);
                        fread(px.data(), 1, nbytes, tf);
                        if (TexCls && ByteCls) {
                            const MethodInfo* tctor4 = find_method(TexCls, ".ctor", 4);
                            Il2CppObject* tex = g_api.object_new(TexCls);
                            if (tctor4) {
                                int wi = (int)tw, hi = (int)th, fmt = 4; bool mip = false;
                                void* ta[4] = { &wi, &hi, &fmt, &mip };
                                invoke_inst(tctor4, tex, ta);
                            }
                            const MethodInfo* loadRaw = find_method(TexCls, "LoadRawTextureData", 1);
                            const MethodInfo* apply1 = find_method(TexCls, "Apply", 1);
                            if (tex && loadRaw) {
                                Il2CppArray* barr = g_api.array_new(ByteCls, nbytes);
                                memcpy(array_addr(barr, 1, 0), px.data(), nbytes);
                                void* la[1] = { barr };
                                invoke_inst(loadRaw, tex, la);
                                if (apply1) { bool upd = false; void* aa[1] = { &upd }; invoke_inst(apply1, tex, aa); }
                                const MethodInfo* setMain = find_method(MatCls, "set_mainTexture", 1);
                                if (setMain) { void* sa[1] = { tex }; invoke_inst(setMain, mat, sa); }
                                const MethodInfo* setCol = find_method(MatCls, "set_color", 1);
                                if (setCol) { Color4 wc = {1,1,1,1}; void* ca[1] = { &wc }; invoke_inst(setCol, mat, ca); }
                                LOGI("part: tex ok %ux%u", tw, th);
                            }
                        }
                    }
                }
                fclose(tf);
            } else {
                LOGE("part: tex open fail %s", texPath);
            }
        }
    }

    // 挂到角色根节点，统一缩放和朝向
    const MethodInfo* getTransGO = find_method(GOCls, "get_transform", 0);
    const MethodInfo* setParent  = find_method(TransCls, "SetParent", 1);
    const MethodInfo* setLPos    = find_method(TransCls, "set_localPosition", 1);
    const MethodInfo* setLScale  = find_method(TransCls, "set_localScale", 1);
    const MethodInfo* setLRot    = find_method(TransCls, "set_localRotation", 1);
    Il2CppObject* gt = getTransGO ? invoke_inst(getTransGO, go, nullptr) : nullptr;
    if (gt && roleRoot && setParent) {
        void* a[1] = { roleRoot };
        invoke_inst(setParent, gt, a);
        if (g_render_mode == 2) {
            // CPU 蒙皮：顶点自己算到角色空间，节点不许再叠变换
            if (setLScale) { Vec3 s2 = {1, 1, 1}; void* b[1] = { &s2 }; invoke_inst(setLScale, gt, b); }
            if (setLRot) { Quat q = {0.0f, 0.0f, 0.0f, 1.0f}; void* b[1] = { &q }; invoke_inst(setLRot, gt, b); }
            if (setLPos) { Vec3 p = {0, 0, 0}; void* b[1] = { &p }; invoke_inst(setLPos, gt, b); }
            LOGI("part: cpu-skin transform (identity)");
        } else {
            if (setLScale) { Vec3 s2 = {scaleK, scaleK, scaleK}; void* b[1] = { &s2 }; invoke_inst(setLScale, gt, b); }
            if (setLRot) { Quat q = {0.0f, 1.0f, 0.0f, 0.0f}; void* b[1] = { &q }; invoke_inst(setLRot, gt, b); }
            if (setLPos) {
                Vec3 p = g_replace_mode ? Vec3{0, 0, 0} : Vec3{0, 2.4f, 0};
                void* b[1] = { &p };
                invoke_inst(setLPos, gt, b);
            }
        }
    }
    LOGI("part: done %s go=%p", meshPath, go);
    return go;
}

// 一次加载 custom_0..custom_3





// =====================================================================
// v116：不克隆了，直接改原角色的 SkinnedMeshRenderer
//
// v114/v115 一直在克隆的 SMR 上换 mesh，每次都 readback=FAIL。
// 克隆对象是新建的，native 侧可能半初始化；而原 SMR 是活的、骨骼齐全、
// 材质槽和蒙皮 shader 都是现成的 —— 直接在它上面动手才对。
//
// 流程：
//   1. 挑骨骼匹配最好的那个原 SMR（用 get_bones() 统计我们的骨骼名命中数）
//   2. 造一个带 bindposes + boneWeights 的 Mesh（长度对齐原 SMR 的骨骼数）
//   3. 把 native 结构里所有等于旧 mesh 指针的槽全写成新 mesh
//   4. get_sharedMesh 读回确认；失败就把旧指针全还原，绝不留下半坏状态
//
// 蒙皮公式：bindposes[gidx[i]] = Scale(k) * R_y180 * inverseBindMatrix[i]
// =====================================================================

#ifndef GPU_MAX_PARTS
#define GPU_MAX_PARTS 4
#endif

static const MethodInfo* find_method_any(Il2CppClass* klass, const char* name) {
    if (!klass) return nullptr;
    pthread_mutex_lock(&g_find_mtx);
    const MethodInfo* found = nullptr;
    for (Il2CppClass* k = klass; k && !found; k = g_api.class_get_parent(k)) {
        void* iter = nullptr;
        const MethodInfo* m;
        while ((m = g_api.class_get_methods(k, &iter))) {
            const char* nm = g_api.method_get_name(m);
            if (nm && strcmp(nm, name) == 0) { found = m; break; }
        }
    }
    pthread_mutex_unlock(&g_find_mtx);
    return found;
}

static FieldInfo* find_own_field(Il2CppClass* klass, const char* name) {
    if (!klass || !g_api.class_get_fields) return nullptr;
    pthread_mutex_lock(&g_find_mtx);
    FieldInfo* found = nullptr;
    void* iter = nullptr;
    FieldInfo* f;
    while ((f = g_api.class_get_fields(klass, &iter))) {
        const char* nm = g_api.field_get_name(f);
        if (nm && strcmp(nm, name) == 0) { found = f; break; }
    }
    pthread_mutex_unlock(&g_find_mtx);
    return found;
}

static int g_last_exc = 0;
static Il2CppObject* invoke_chk(const MethodInfo* m, Il2CppObject* self, void** args) {
    if (!m) { g_last_exc = 1; return nullptr; }
    Il2CppException* exc = nullptr;
    g_last_exc = 0;
    Il2CppObject* r = g_api.runtime_invoke(m, self, args, &exc);
    if (exc) { g_last_exc = 1; return nullptr; }
    return r;
}

static int array_elem_size(Il2CppClass* k, int fallback) {
    if (k && g_api.class_array_element_size) {
        int n = (int)g_api.class_array_element_size(k);
        if (n > 0 && n < 4096) return n;
    }
    return fallback;
}

struct BwLayout { int idx[4]; int wgt[4]; int size; bool ok; };
static BwLayout g_bw_layout = {{0,0,0,0},{0,0,0,0},0,false};

static BwLayout bw_layout(Il2CppClass* BW) {
    BwLayout L;
    // Unity 2018 的 BoneWeight 是 [StructLayout(LayoutKind.Sequential)]：
    // (int boneIndex, float weight) x4，紧凑 32 字节。
    // il2cpp_field_get_offset 返回的不是紧凑布局（实测 span=48），所以直接写死标准布局。
    L.idx[0]=0;  L.wgt[0]=4;  L.idx[1]=8;  L.wgt[1]=12;
    L.idx[2]=16; L.wgt[2]=20; L.idx[3]=24; L.wgt[3]=28;
    L.size = 32; L.ok = true;
    int stride = array_elem_size(BW, 32);
    if (stride != 32)
        LOGE("gpu: BoneWeight stride=%d != 32, using standard layout anyway", stride);
    LOGI("gpu: BW layout std i0=0 w0=4 i3=24 w3=28 stride=%d", stride);
    g_bw_layout = L;
    return L;
}

// ---------------------------------------------------------------
static void probe_unity_api() {
    LOGI("=== PROBE BEGIN ===");
    Il2CppClass* SMR = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    Il2CppClass* M   = find_class_any("UnityEngine", "Mesh");
    Il2CppClass* MPB = find_class_any("UnityEngine", "MaterialPropertyBlock");
    Il2CppClass* AB  = find_class_any("UnityEngine", "AssetBundle");
    if (M) {
        const char* w[] = {"set_bindposes","set_boneWeights","set_vertices","set_normals",
                           "set_uv","set_triangles","set_subMeshCount","get_subMeshCount",
                           "RecalculateBounds","SetTriangles","SetVertices","SetNormals"};
        for (size_t i = 0; i < sizeof(w)/sizeof(w[0]); i++) {
            const MethodInfo* m2 = find_method_any(M, w[i]);
            LOGI("Mesh.%s = %p argc=%u", w[i], (void*)m2,
                 m2 ? g_api.method_get_param_count(m2) : 0);
        }
        LOGI("Matrix4x4 stride=%d  BoneWeight stride=%d",
             array_elem_size(find_class_any("UnityEngine","Matrix4x4"), 64),
             array_elem_size(find_class_any("UnityEngine","BoneWeight"), 32));
        bw_layout(find_class_any("UnityEngine","BoneWeight"));
    }
    if (SMR) {
        const char* w[] = {"get_bones","get_sharedMesh","get_sharedMaterials","set_sharedMaterials"};
        for (size_t i = 0; i < sizeof(w)/sizeof(w[0]); i++)
            LOGI("SMR.%s = %p", w[i], (void*)find_method_any(SMR, w[i]));
    }
    if (MPB) {
        LOGI("MPB.SetMatrixArray = %p", (void*)find_method_any(MPB, "SetMatrixArray"));
        LOGI("MPB.SetVectorArray = %p", (void*)find_method_any(MPB, "SetVectorArray"));
    }
    if (AB) {
        LOGI("AssetBundle.LoadFromFile = %p", (void*)find_method_any(AB, "LoadFromFile"));
        LOGI("AssetBundle.LoadFromMemory = %p", (void*)find_method_any(AB, "LoadFromMemory"));
    }
    LOGI("=== PROBE END ===");
}

// ---------------------------------------------------------------
// 读 mmesh 的骨骼名（只读头，够用来挑 SMR）
// ---------------------------------------------------------------
static bool read_mm2_names(const char* path, std::vector<std::string>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char magic[4]; uint32_t nv = 0, ni = 0, flags = 0;
    if (fread(magic,1,4,f) != 4 || memcmp(magic,"MMS2",4) != 0) { fclose(f); return false; }
    if (fread(&nv,4,1,f) != 1 || fread(&ni,4,1,f) != 1 || fread(&flags,4,1,f) != 1) { fclose(f); return false; }
    if (nv == 0 || nv > 600000 || ni == 0) { fclose(f); return false; }
    long skip = (long)((size_t)nv*12 + (size_t)nv*12 + (size_t)nv*8 + (size_t)ni*4);
    if (fseek(f, skip, SEEK_CUR) != 0) { fclose(f); return false; }
    uint32_t nb = 0;
    if (fread(&nb,4,1,f) != 1 || nb == 0 || nb > 256) { fclose(f); return false; }
    out.clear();
    for (uint32_t i = 0; i < nb; i++) {
        uint32_t len = 0;
        if (fread(&len,4,1,f) != 1 || len > 128) { fclose(f); return false; }
        std::vector<char> b(len + 1, 0);
        if (fread(b.data(),1,len,f) != len) { fclose(f); return false; }
        out.push_back(std::string(b.data(), len));
    }
    fclose(f);
    return true;
}

// ---------------------------------------------------------------
// 骨骼名归一化 + 模糊映射
//   我们的:  "Bip001 L Thigh"  -> "lthigh"
//   游戏的:  "11055_L_Thigh"   -> "lthigh"
// ---------------------------------------------------------------
static std::string norm_bone(const std::string& in, bool stripBip) {
    std::string s;
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == ' ' || c == '_' || c == '-' || c == '.') continue;
        s.push_back((char)tolower((unsigned char)c));
    }
    if (stripBip && s.size() > 6 && s.compare(0, 6, "bip001") == 0) s = s.substr(6);
    size_t k = 0;
    while (k < s.size() && s[k] >= '0' && s[k] <= '9') k++;
    if (k > 0) s = s.substr(k);
    return s;
}

static bool bone_suffix_match(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    if (b.size() > a.size() && b.compare(b.size() - a.size(), a.size(), a) == 0) return true;
    return false;
}

// 两轮：先精确相等，再后缀匹配
static int map_bones(const std::vector<std::string>& ourNames,
                     const std::vector<std::string>& gameNorm,
                     std::vector<int>* gidxOut) {
    int nb = (int)ourNames.size(), N = (int)gameNorm.size();
    std::vector<int> gidx((size_t)nb, -1);
    int found = 0;
    for (int i = 0; i < nb; i++) {
        std::string a = norm_bone(ourNames[i], true);
        if (a.empty()) continue;
        for (int j = 0; j < N; j++) {
            if (gameNorm[j] == a) { gidx[i] = j; found++; break; }
        }
    }
    for (int i = 0; i < nb; i++) {
        if (gidx[i] >= 0) continue;
        std::string a = norm_bone(ourNames[i], true);
        if (a.empty()) continue;
        for (int j = 0; j < N; j++) {
            if (bone_suffix_match(a, gameNorm[j])) { gidx[i] = j; found++; break; }
        }
    }
    if (gidxOut) *gidxOut = gidx;
    return found;
}

static bool under_root_chain(Il2CppObject* smr, Il2CppObject* root) {
    if (!smr || !root) return false;
    Il2CppClass* CompC = find_class_any("UnityEngine", "Component");
    Il2CppClass* TransC = find_class_any("UnityEngine", "Transform");
    if (!CompC || !TransC) return false;
    const MethodInfo* gt = find_method(CompC, "get_transform", 0);
    const MethodInfo* gp = find_method(TransC, "get_parent", 0);
    if (!gt || !gp) return false;
    Il2CppObject* cur = invoke_inst(gt, smr, nullptr);
    for (int i = 0; i < 12 && cur; i++) {
        if (cur == root) return true;
        cur = invoke_inst(gp, cur, nullptr);
    }
    return false;
}

// ---------------------------------------------------------------
// 挑骨骼命中数最多的原 SMR
// ---------------------------------------------------------------
// ---------------------------------------------------------------
// 目标列表：给悬浮窗用，列出场景里所有可替换的 SMR
// 显示用「角色根节点名 + 部件名 + 骨骼数」，玩家自己打星号
// ---------------------------------------------------------------
static pthread_mutex_t g_tgt_mtx = PTHREAD_MUTEX_INITIALIZER;
static std::string g_tgt_list;
static std::vector<Il2CppObject*> g_tgt_smrs;
static volatile int g_tgt_sel = -1;      // -1 = 自动挑

static void build_target_list(Il2CppObject* playerRoot) {
    Il2CppClass* SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    if (!SMRCls) return;
    std::vector<Fingerprint> list;
    collect_fingerprints(SMRCls, list);
    const MethodInfo* gb = find_method_any(SMRCls, "get_bones");
    std::string out;
    std::vector<Il2CppObject*> smrs;
    std::map<std::string,int> bestOfRoot;
    int idx = 0;
    for (size_t i = 0; i < list.size(); i++) {
        Il2CppObject* s = list[i].smr;
        if (!s || !*(void**)((char*)s + 16)) continue;
        Il2CppArray* a = gb ? (Il2CppArray*)invoke_inst(gb, s, nullptr) : nullptr;
        int nb = a ? (int)a->max_length : 0;
        if (nb < 20) continue;                 // 跳过脸骨、手指这种小骨架
        std::string ch = list[i].chain;
        std::string leaf = ch;
        size_t p = ch.find_last_of('/');
        if (p != std::string::npos) leaf = ch.substr(p + 1);
        Il2CppObject* root = find_role_root(s);
        std::string rootName = root ? obj_name(root) : std::string();
        // 同一个角色下只保留骨骼最多的那个部件，避免列表里一堆同一角色的条目
        if (!rootName.empty()) {
            std::map<std::string,int>::iterator hit = bestOfRoot.find(rootName);
            if (hit != bestOfRoot.end() && hit->second >= nb) continue;
            bestOfRoot[rootName] = nb;
        }
        bool isPlayer = (playerRoot != nullptr) && under_root_chain(s, playerRoot);
        char line[360];
        snprintf(line, sizeof(line), "%d|%d|%s|%s|%d\n",
                 idx, isPlayer ? 1 : 0,
                 rootName.empty() ? leaf.c_str() : rootName.c_str(),
                 leaf.c_str(), nb);
        out += line;
        smrs.push_back(s);
        idx++;
    }
    pthread_mutex_lock(&g_tgt_mtx);
    g_tgt_list = out;
    g_tgt_smrs.swap(smrs);
    pthread_mutex_unlock(&g_tgt_mtx);
}

static Il2CppObject* pick_best_smr(Il2CppClass* SMRCls, const std::vector<std::string>& names,
                                   Il2CppObject* preferRoot,
                                   std::vector<Il2CppObject*>* outBones,
                                   std::vector<std::string>* outNorm) {
    std::vector<Fingerprint> list;
    collect_fingerprints(SMRCls, list);
    const MethodInfo* gb = find_method_any(SMRCls, "get_bones");
    if (!gb) { LOGE("gpu: no get_bones"); return nullptr; }

    Il2CppObject* best = nullptr;
    int bestN = 0;
    bool dumped = false;

    // 第一轮只扫玩家角色根节点下的；不够再扫全场
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 && bestN * 4 >= (int)names.size() * 3) break;
        for (size_t i = 0; i < list.size(); i++) {
            Il2CppObject* sm = list[i].smr;
            if (!sm || !*(void**)((char*)sm + 16)) continue;
            if (pass == 0 && preferRoot && !under_root_chain(sm, preferRoot)) continue;
            Il2CppArray* a = (Il2CppArray*)invoke_inst(gb, sm, nullptr);
            if (!a) continue;
            int N = (int)a->max_length;
            std::vector<std::string> gnorm((size_t)N);
            for (int j = 0; j < N; j++) {
                Il2CppObject* t = *(Il2CppObject**)array_addr(a, 8, (size_t)j);
                gnorm[j] = t ? norm_bone(obj_name(t), false) : "";
            }
            std::vector<int> gidx;
            int n = map_bones(names, gnorm, &gidx);
            if (n >= 3) LOGI("gpu: cand[p%d] %s bones=%d matched=%d", pass, list[i].chain.c_str(), N, n);
            // 玩家角色骨骼名对不上时，把真实名字打出来看看
            if (pass == 0 && !dumped && n * 2 < (int)names.size()) {
                dumped = true;
                for (int j = 0; j < N && j < 16; j++) {
                    Il2CppObject* t = *(Il2CppObject**)array_addr(a, 8, (size_t)j);
                    LOGI("  playBone[%d] = %s", j, t ? obj_name(t).c_str() : "(null)");
                }
            }
            if (n > bestN) {
                bestN = n; best = sm;
                if (outNorm) *outNorm = gnorm;
                if (outBones) {
                    outBones->clear();
                    for (int j = 0; j < N; j++)
                        outBones->push_back(*(Il2CppObject**)array_addr(a, 8, (size_t)j));
                }
            }
        }
    }
    LOGI("gpu: BEST matched=%d/%d", bestN, (int)names.size());
    if (!best || bestN * 4 < (int)names.size() * 3) return nullptr;
    return best;
}

// ---------------------------------------------------------------
// 交换 native 里的 mesh 槽（记录写过的偏移，失败可还原）
// ---------------------------------------------------------------
static int g_swapped_offs[16];
static int g_swapped_n = 0;

static int swap_mesh_native(Il2CppObject* node, void* oldNat, void* newNat, const char* tag) {
    g_swapped_n = 0;
    void* nat = *(void**)((char*)node + 16);
    if (!nat || !oldNat || !newNat) {
        LOGE("gpu: %s swap skipped (nat=%p old=%p new=%p)", tag, nat, oldNat, newNat);
        return 0;
    }
    for (int off = 0x40; off < 0x600; off += 8) {
        if (*(void**)((char*)nat + off) == oldNat) {
            *(void**)((char*)nat + off) = newNat;
            LOGI("gpu: %s native[0x%x] <- new mesh", tag, off);
            if (g_swapped_n < 16) g_swapped_offs[g_swapped_n++] = off;
        }
    }
    if (!g_swapped_n) LOGE("gpu: %s no slot held old mesh (0x40..0x600)", tag);
    return g_swapped_n;
}

static void restore_mesh_native(Il2CppObject* node, void* backTo) {
    void* nat = *(void**)((char*)node + 16);
    if (!nat) return;
    for (int i = 0; i < g_swapped_n; i++)
        *(void**)((char*)nat + g_swapped_offs[i]) = backTo;
    LOGI("gpu: restored %d slots -> %p", g_swapped_n, backTo);
    g_swapped_n = 0;
}

// ---------------------------------------------------------------
// Unity 的 m_Mesh 是 PPtr<Mesh>（存 instanceID），不是裸指针。
// 之前写 0x1d8 的裸指针读不回来就是这个原因。
// ---------------------------------------------------------------
static int get_instance_id(Il2CppObject* obj) {
    if (!obj) return 0;
    Il2CppClass* Oc = find_class_any("UnityEngine", "Object");
    if (!Oc) return 0;
    const MethodInfo* m = find_method_any(Oc, "GetInstanceID");
    if (!m) m = find_method_any(Oc, "get_instanceID");
    if (!m) return 0;
    Il2CppObject* r = invoke_inst(m, obj, nullptr);
    if (!r) return 0;
    return *(int*)((char*)r + 16);
}

// 在 native 结构里把所有等于 oldId 的 int32 换成 newId（PPtr 的 pathID 槽）
static int g_id_offs[16];
static int g_id_n = 0;

static int patch_by_instance_id(Il2CppObject* node, int oldId, int newId, const char* tag) {
    g_id_n = 0;
    void* nat = *(void**)((char*)node + 16);
    if (!nat || !oldId || !newId || oldId == newId) {
        LOGE("gpu: %s id-patch skipped (nat=%p old=%d new=%d)", tag, nat, oldId, newId);
        return 0;
    }
    for (int off = 0x30; off < 0x600; off += 4) {
        int v = *(int*)((char*)nat + off);
        if (v == oldId) {
            LOGI("gpu: %s instanceID %d @0x%x", tag, oldId, off);
            *(int*)((char*)nat + off) = newId;
            if (g_id_n < 16) g_id_offs[g_id_n++] = off;
        }
    }
    LOGI("gpu: %s patched %d instanceID slots", tag, g_id_n);
    return g_id_n;
}

static void restore_by_instance_id(Il2CppObject* node, int backTo) {
    void* nat = *(void**)((char*)node + 16);
    if (!nat) return;
    for (int i = 0; i < g_id_n; i++) *(int*)((char*)nat + g_id_offs[i]) = backTo;
    LOGI("gpu: restored %d instanceID slots -> %d", g_id_n, backTo);
    g_id_n = 0;
}

// ---------------------------------------------------------------
// 造一个带骨架绑定的 Mesh
//   name2idx: 我们的骨骼名 -> 目标 SMR 骨骼数组下标
//   N: 目标 SMR 的骨骼总数（bindposes 必须这个长度）
// ---------------------------------------------------------------
// ---------------------------------------------------------------
// 直接把已有 Mesh 的内容重写成我们的模型（不换引用，绕开 PPtr / set_sharedMesh）
// ---------------------------------------------------------------
static int rewrite_original_mesh(Il2CppObject* mesh,
                                 const char* meshPath,
                                 const std::vector<std::string>& gameNorm,
                                 int N, int fallback, float k,
                                 int* outNv, int* outFound,
                                 float* boneWorldY = nullptr) {
    Il2CppClass* MeshCls = find_class_any("UnityEngine", "Mesh");
    Il2CppClass* V3Cls   = find_class_any("UnityEngine", "Vector3");
    Il2CppClass* V2Cls   = find_class_any("UnityEngine", "Vector2");
    Il2CppClass* I32Cls  = find_class_any("System", "Int32");
    Il2CppClass* BWCls   = find_class_any("UnityEngine", "BoneWeight");
    Il2CppClass* M4Cls   = find_class_any("UnityEngine", "Matrix4x4");
    if (!mesh || !MeshCls || !V3Cls || !V2Cls || !I32Cls || !BWCls || !M4Cls) {
        LOGE("gpu: rewrite: class missing");
        return 0;
    }
    BwLayout bwL = bw_layout(BWCls);
    if (!bwL.ok) return 0;

    FILE* f = fopen(meshPath, "rb");
    if (!f) { LOGE("gpu: rewrite open fail %s", meshPath); return 0; }
    char magic[4]; uint32_t nv = 0, ni = 0, flags = 0;
    if (fread(magic,1,4,f) != 4 || memcmp(magic,"MMS2",4) != 0) { fclose(f); return 0; }
    if (fread(&nv,4,1,f) != 1 || fread(&ni,4,1,f) != 1 || fread(&flags,4,1,f) != 1) { fclose(f); return 0; }
    if (nv == 0 || nv > 600000 || ni == 0) { fclose(f); return 0; }

    std::vector<float> pos((size_t)nv*3), nrm((size_t)nv*3), uv((size_t)nv*2);
    std::vector<uint32_t> idx(ni);
    if (fread(pos.data(),4,(size_t)nv*3,f) != (size_t)nv*3) { fclose(f); return 0; }
    if (fread(nrm.data(),4,(size_t)nv*3,f) != (size_t)nv*3) { fclose(f); return 0; }
    if (fread(uv.data(), 4,(size_t)nv*2,f) != (size_t)nv*2) { fclose(f); return 0; }
    if (fread(idx.data(),4,ni,f) != ni) { fclose(f); return 0; }

    uint32_t nb = 0;
    if (fread(&nb,4,1,f) != 1 || nb == 0 || nb > 256) { fclose(f); return 0; }
    std::vector<std::string> names;
    for (uint32_t i = 0; i < nb; i++) {
        uint32_t len = 0;
        if (fread(&len,4,1,f) != 1 || len > 128) { fclose(f); return 0; }
        std::vector<char> b(len + 1, 0);
        if (fread(b.data(),1,len,f) != len) { fclose(f); return 0; }
        names.push_back(std::string(b.data(), len));
    }
    uint32_t jc = 0;
    if (fread(&jc,4,1,f) != 1 || jc != nb) { fclose(f); return 0; }
    std::vector<int>   joints((size_t)nv*4);
    std::vector<float> weights((size_t)nv*4);
    for (uint32_t i = 0; i < nv; i++) {
        uint8_t j4[4]; float w4[4];
        if (fread(j4,1,4,f) != 4 || fread(w4,4,4,f) != 4) { fclose(f); return 0; }
        for (int q = 0; q < 4; q++) { joints[i*4+q] = j4[q]; weights[i*4+q] = w4[q]; }
    }
    std::vector<float> ibm((size_t)nb*16);
    if (fread(ibm.data(),4,(size_t)nb*16,f) != (size_t)nb*16) { fclose(f); return 0; }
    fclose(f);

    std::vector<int> gidx;
    int found = map_bones(names, gameNorm, &gidx);
    int fb = -1;
    for (uint32_t i = 0; i < nb; i++) if (gidx[i] >= 0) { fb = gidx[i]; break; }
    if (fb < 0) fb = fallback;
    for (uint32_t i = 0; i < nb; i++) if (gidx[i] < 0) gidx[i] = fb;
    LOGI("gpu: rewrite nv=%u ni=%u bones=%d/%u targetBones=%d", nv, ni, found, nb, N);
    if (outFound) *outFound = found;

    // ===== 诊断：顶点空间范围、骨骼映射、样例顶点 =====
    {
        float mn[3] = {1e9f,1e9f,1e9f}, mx[3] = {-1e9f,-1e9f,-1e9f};
        for (uint32_t i = 0; i < nv; i++) {
            for (int c = 0; c < 3; c++) {
                float v = pos[i*3+c];
                if (v < mn[c]) mn[c] = v;
                if (v > mx[c]) mx[c] = v;
            }
        }
        LOGI("gpu: VERT BBOX x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f] height=%.3f k=%.5f",
             mn[0],mx[0],mn[1],mx[1],mn[2],mx[2], mx[1]-mn[1], k);

        for (uint32_t i = 0; i < nb && i < 22; i++) {
            int gi = gidx[i];
            const float* m = &ibm[(size_t)i*16];
            LOGI("gpu: map[%u] %s -> gidx=%d target=%s  ibmT=(%.2f,%.2f,%.2f)",
                 i, names[i].c_str(), gi,
                 (gi >= 0 && gi < (int)gameNorm.size()) ? gameNorm[gi].c_str() : "?",
                 -m[12], -m[13], -m[14]);
        }
        for (int v = 0; v < 3 && v < (int)nv; v++) {
            LOGI("gpu: vert[%d] pos=(%.2f,%.2f,%.2f) j=%d,%d,%d,%d w=%.2f,%.2f,%.2f,%.2f -> g=%d,%d,%d,%d",
                 v, pos[v*3],pos[v*3+1],pos[v*3+2],
                 joints[v*4],joints[v*4+1],joints[v*4+2],joints[v*4+3],
                 weights[v*4],weights[v*4+1],weights[v*4+2],weights[v*4+3],
                 gidx[joints[v*4]], gidx[joints[v*4+1]],
                 gidx[joints[v*4+2]], gidx[joints[v*4+3]]);
        }
    }

    // submesh 收成 1 个
    const MethodInfo* setSub = find_method_any(MeshCls, "set_subMeshCount");
    if (setSub) { int one = 1; void* a[1] = { &one }; invoke_inst(setSub, mesh, a); }

    Il2CppArray* va = g_api.array_new(V3Cls, nv); memcpy(array_addr(va,12,0), pos.data(), (size_t)nv*12);
    Il2CppArray* na = g_api.array_new(V3Cls, nv); memcpy(array_addr(na,12,0), nrm.data(), (size_t)nv*12);
    Il2CppArray* ua = g_api.array_new(V2Cls, nv); memcpy(array_addr(ua, 8,0), uv.data(),  (size_t)nv*8);
    Il2CppArray* ia = g_api.array_new(I32Cls, ni); memcpy(array_addr(ia, 4,0), idx.data(), (size_t)ni*4);
    const MethodInfo* setV = find_method_any(MeshCls, "set_vertices");
    const MethodInfo* setN = find_method_any(MeshCls, "set_normals");
    const MethodInfo* setU = find_method_any(MeshCls, "set_uv");
    const MethodInfo* setT = find_method_any(MeshCls, "set_triangles");
    if (!setV || !setT) { LOGE("gpu: rewrite: set_vertices/triangles missing"); return 0; }
    { void* a[1] = { va }; invoke_chk(setV, mesh, a); if (g_last_exc) { LOGE("gpu: set_vertices threw"); return 0; } }
    if (setN) { void* a[1] = { na }; invoke_inst(setN, mesh, a); }
    if (setU) { void* a[1] = { ua }; invoke_inst(setU, mesh, a); }
    { void* a[1] = { ia }; invoke_chk(setT, mesh, a); if (g_last_exc) { LOGE("gpu: set_triangles threw"); return 0; } }

    // ---- 用目标骨骼的实际跨度重算 k ----
    // 之前硬编码 1.7/20.214=0.084，但目标角色的骨骼跨度可能是 7 米，
    // 差 11 倍 -> 顶点全被压到骨骼间距的 1/11，接缝撕裂。
    if (boneWorldY) {
        int pi = -1, hi = -1;
        for (uint32_t i = 0; i < nb; i++) {
            if (gameNorm[gidx[i]].find("pelvis") != std::string::npos) pi = gidx[i];
            if (gameNorm[gidx[i]].find("head") != std::string::npos) hi = gidx[i];
        }
        float pmxSpan = 0.0f;
        for (uint32_t i = 0; i < nb; i++) {
            if (gameNorm[gidx[i]].find("pelvis") != std::string::npos)
                pmxSpan -= ibm[(size_t)i*16 + 13];      // p_y
            if (gameNorm[gidx[i]].find("head") != std::string::npos)
                pmxSpan += ibm[(size_t)i*16 + 13];
        }
        float gameSpan = 0.0f;
        if (pi >= 0 && hi >= 0 && pi < N && hi < N)
            gameSpan = boneWorldY[hi] - boneWorldY[pi];
        LOGI("gpu: k-auto pelvisBin=%d headBin=%d pmxSpan=%.3f gameSpan=%.3f", pi, hi, pmxSpan, gameSpan);
        if (pmxSpan > 0.01f && gameSpan > 0.01f) {
            float knew = gameSpan / pmxSpan;
            LOGI("gpu: k auto %.5f -> %.5f", k, knew);
            k = knew;
        } else {
            LOGE("gpu: k auto failed, keep %.5f", k);
        }
    }

    // bindposes 长度必须 = 骨骼数
    const MethodInfo* setBP = find_method_any(MeshCls, "set_bindposes");
    if (!setBP) { LOGE("gpu: rewrite: set_bindposes stripped"); return 0; }
    {
        int m4 = array_elem_size(M4Cls, 64);
        Il2CppArray* bpa = g_api.array_new(M4Cls, (size_t)N);
        if (!bpa) return 0;
        float* d = (float*)array_addr(bpa, (size_t)m4, 0);
        for (int i = 0; i < N*16; i++) d[i] = ((i % 5) == 0) ? 1.0f : 0.0f;
        for (uint32_t i = 0; i < nb; i++) {
            int gi = gidx[i];
            if (gi < 0 || gi >= N) continue;
            const float* m = &ibm[(size_t)i*16];
            float* o = d + (size_t)gi*16;
            for (int j = 0; j < 16; j++) {
                int r = j % 4;
                float sc = (r == 3) ? 1.0f : ((r == 0 || r == 2) ? -k : k);
                o[j] = m[j] * sc;
            }
        }
        void* a[1] = { bpa };
        invoke_chk(setBP, mesh, a);
        if (g_last_exc) { LOGE("gpu: set_bindposes threw"); return 0; }
    }

    // boneWeights
    const MethodInfo* setBW = find_method_any(MeshCls, "set_boneWeights");
    if (!setBW) { LOGE("gpu: rewrite: set_boneWeights stripped"); return 0; }
    {
        int stride = array_elem_size(BWCls, bwL.size);
        Il2CppArray* bwa = g_api.array_new(BWCls, (size_t)nv);
        if (!bwa) return 0;
        unsigned char* base = (unsigned char*)array_addr(bwa, (size_t)stride, 0);
        for (uint32_t i = 0; i < nv; i++) {
            int bi[4]; float bw[4]; float sum = 0;
            for (int q = 0; q < 4; q++) {
                int ji = joints[i*4+q];
                float w = weights[i*4+q];
                if (w <= 0.0001f) { bi[q] = fb; bw[q] = 0; continue; }
                if (ji < 0 || (uint32_t)ji >= nb) ji = 0;
                bi[q] = (gidx[ji] >= 0) ? gidx[ji] : fb;
                bw[q] = w; sum += w;
            }
            if (sum <= 0.00001f) { bi[0]=fb; bw[0]=1; sum=1; for (int q=1;q<4;q++){bi[q]=fb;bw[q]=0;} }
            for (int q = 0; q < 4; q++) bw[q] /= sum;
            unsigned char* e = base + (size_t)i * (size_t)stride;
            for (int q = 0; q < 4; q++) {
                *(int*)(e + bwL.idx[q])   = bi[q];
                *(float*)(e + bwL.wgt[q]) = bw[q];
            }
        }
        void* a[1] = { bwa };
        invoke_chk(setBW, mesh, a);
        if (g_last_exc) { LOGE("gpu: set_boneWeights threw"); return 0; }
    }

    const MethodInfo* recalc = find_method_any(MeshCls, "RecalculateBounds");
    if (recalc) invoke_inst(recalc, mesh, nullptr);

    if (outNv) *outNv = (int)nv;
    LOGI("gpu: rewrite applied to existing mesh nv=%u", nv);
    return 1;
}

static Il2CppObject* build_skinned_mesh(const char* meshPath,
                                        const std::vector<std::string>& gameNorm,
                                        int N, int fallback, float k,
                                        int* outNv, int* outFound) {
    Il2CppClass* MeshCls = find_class_any("UnityEngine", "Mesh");
    Il2CppClass* V3Cls   = find_class_any("UnityEngine", "Vector3");
    Il2CppClass* V2Cls   = find_class_any("UnityEngine", "Vector2");
    Il2CppClass* I32Cls  = find_class_any("System", "Int32");
    Il2CppClass* BWCls   = find_class_any("UnityEngine", "BoneWeight");
    Il2CppClass* M4Cls   = find_class_any("UnityEngine", "Matrix4x4");
    if (!MeshCls || !V3Cls || !V2Cls || !I32Cls || !BWCls || !M4Cls) {
        LOGE("gpu: mesh class missing"); return nullptr;
    }
    BwLayout bwL = bw_layout(BWCls);
    if (!bwL.ok) { LOGE("gpu: BW layout unknown"); return nullptr; }

    FILE* f = fopen(meshPath, "rb");
    if (!f) { LOGE("gpu: open fail %s", meshPath); return nullptr; }
    char magic[4]; uint32_t nv = 0, ni = 0, flags = 0;
    if (fread(magic,1,4,f) != 4 || memcmp(magic,"MMS2",4) != 0) { fclose(f); return nullptr; }
    if (fread(&nv,4,1,f) != 1 || fread(&ni,4,1,f) != 1 || fread(&flags,4,1,f) != 1) { fclose(f); return nullptr; }
    if (nv == 0 || nv > 600000 || ni == 0) { fclose(f); return nullptr; }

    std::vector<float> pos((size_t)nv*3), nrm((size_t)nv*3), uv((size_t)nv*2);
    std::vector<uint32_t> idx(ni);
    if (fread(pos.data(),4,(size_t)nv*3,f) != (size_t)nv*3) { fclose(f); return nullptr; }
    if (fread(nrm.data(),4,(size_t)nv*3,f) != (size_t)nv*3) { fclose(f); return nullptr; }
    if (fread(uv.data(), 4,(size_t)nv*2,f) != (size_t)nv*2) { fclose(f); return nullptr; }
    if (fread(idx.data(),4,ni,f) != ni) { fclose(f); return nullptr; }

    uint32_t nb = 0;
    if (fread(&nb,4,1,f) != 1 || nb == 0 || nb > 256) { fclose(f); return nullptr; }
    std::vector<std::string> names;
    for (uint32_t i = 0; i < nb; i++) {
        uint32_t len = 0;
        if (fread(&len,4,1,f) != 1 || len > 128) { fclose(f); return nullptr; }
        std::vector<char> b(len + 1, 0);
        if (fread(b.data(),1,len,f) != len) { fclose(f); return nullptr; }
        names.push_back(std::string(b.data(), len));
    }
    uint32_t jc = 0;
    if (fread(&jc,4,1,f) != 1 || jc != nb) { fclose(f); return nullptr; }
    std::vector<int>   joints((size_t)nv*4);
    std::vector<float> weights((size_t)nv*4);
    for (uint32_t i = 0; i < nv; i++) {
        uint8_t j4[4]; float w4[4];
        if (fread(j4,1,4,f) != 4 || fread(w4,4,4,f) != 4) { fclose(f); return nullptr; }
        for (int q = 0; q < 4; q++) { joints[i*4+q] = j4[q]; weights[i*4+q] = w4[q]; }
    }
    std::vector<float> ibm((size_t)nb*16);
    if (fread(ibm.data(),4,(size_t)nb*16,f) != (size_t)nb*16) { fclose(f); return nullptr; }
    fclose(f);

    // 骨骼名 -> 目标数组下标（模糊匹配）
    std::vector<int> gidx;
    int found = map_bones(names, gameNorm, &gidx);
    int fb = -1;
    for (uint32_t i = 0; i < nb; i++) if (gidx[i] >= 0) { fb = gidx[i]; break; }
    if (fb < 0) fb = fallback;
    for (uint32_t i = 0; i < nb; i++) if (gidx[i] < 0) gidx[i] = fb;
    LOGI("gpu: %s nv=%u nb=%u matched=%d/%u targetBones=%d", meshPath, nv, nb, found, nb, N);
    if (outFound) *outFound = found;
    if (found * 4 < (int)nb * 3) { LOGE("gpu: bone match too low"); return nullptr; }

    Il2CppObject* mesh = g_api.object_new(MeshCls);
    const MethodInfo* mctor = find_method_any(MeshCls, ".ctor");
    if (mctor) invoke_inst(mctor, mesh, nullptr);
    if (!*(void**)((char*)mesh + 16)) { LOGE("gpu: Mesh ctor gave no native"); return nullptr; }

    Il2CppArray* va = g_api.array_new(V3Cls, nv); memcpy(array_addr(va,12,0), pos.data(), (size_t)nv*12);
    Il2CppArray* na = g_api.array_new(V3Cls, nv); memcpy(array_addr(na,12,0), nrm.data(), (size_t)nv*12);
    Il2CppArray* ua = g_api.array_new(V2Cls, nv); memcpy(array_addr(ua, 8,0), uv.data(),  (size_t)nv*8);
    Il2CppArray* ia = g_api.array_new(I32Cls, ni); memcpy(array_addr(ia, 4,0), idx.data(), (size_t)ni*4);
    const MethodInfo* setV = find_method_any(MeshCls, "set_vertices");
    const MethodInfo* setN = find_method_any(MeshCls, "set_normals");
    const MethodInfo* setU = find_method_any(MeshCls, "set_uv");
    const MethodInfo* setT = find_method_any(MeshCls, "set_triangles");
    if (!setV || !setT) { LOGE("gpu: set_vertices/triangles missing"); return nullptr; }
    { void* a[1] = { va }; invoke_inst(setV, mesh, a); }
    if (setN) { void* a[1] = { na }; invoke_inst(setN, mesh, a); }
    if (setU) { void* a[1] = { ua }; invoke_inst(setU, mesh, a); }
    { void* a[1] = { ia }; invoke_inst(setT, mesh, a); }

    // bindposes：长度必须 = N
    const MethodInfo* setBP = find_method_any(MeshCls, "set_bindposes");
    if (!setBP) { LOGE("gpu: set_bindposes stripped"); return nullptr; }
    {
        int m4 = array_elem_size(M4Cls, 64);
        Il2CppArray* bpa = g_api.array_new(M4Cls, (size_t)N);
        if (!bpa) return nullptr;
        float* d = (float*)array_addr(bpa, (size_t)m4, 0);
        for (int i = 0; i < N*16; i++) d[i] = ((i % 5) == 0) ? 1.0f : 0.0f;  // 列主序单位阵
        for (uint32_t i = 0; i < nb; i++) {
            int gi = gidx[i];
            if (gi < 0 || gi >= N) continue;
            const float* m = &ibm[(size_t)i*16];
            float* o = d + (size_t)gi*16;
            for (int j = 0; j < 16; j++) {
                int r = j % 4;
                float s = (r == 3) ? 1.0f : ((r == 0 || r == 2) ? -k : k);
                o[j] = m[j] * s;
            }
        }
        void* a[1] = { bpa };
        invoke_chk(setBP, mesh, a);
        if (g_last_exc) { LOGE("gpu: set_bindposes threw"); return nullptr; }
    }

    // boneWeights：步长问 il2cpp，字段按名写
    const MethodInfo* setBW = find_method_any(MeshCls, "set_boneWeights");
    if (!setBW) { LOGE("gpu: set_boneWeights stripped"); return nullptr; }
    {
        int stride = array_elem_size(BWCls, bwL.size);
        if (stride < bwL.size) { LOGE("gpu: stride %d < span %d", stride, bwL.size); return nullptr; }
        Il2CppArray* bwa = g_api.array_new(BWCls, (size_t)nv);
        if (!bwa) return nullptr;
        unsigned char* base = (unsigned char*)array_addr(bwa, (size_t)stride, 0);
        for (uint32_t i = 0; i < nv; i++) {
            int bi[4]; float bw[4]; float sum = 0;
            for (int q = 0; q < 4; q++) {
                int ji = joints[i*4+q];
                float w = weights[i*4+q];
                if (w <= 0.0001f) { bi[q] = fb; bw[q] = 0; continue; }
                if (ji < 0 || (uint32_t)ji >= nb) ji = 0;
                bi[q] = (gidx[ji] >= 0) ? gidx[ji] : fb;
                bw[q] = w; sum += w;
            }
            if (sum <= 0.00001f) { bi[0]=fb; bw[0]=1; sum=1; for (int q=1;q<4;q++){bi[q]=fb;bw[q]=0;} }
            for (int q = 0; q < 4; q++) bw[q] /= sum;
            unsigned char* e = base + (size_t)i * (size_t)stride;
            for (int q = 0; q < 4; q++) {
                *(int*)(e + bwL.idx[q])   = bi[q];
                *(float*)(e + bwL.wgt[q]) = bw[q];
            }
        }
        void* a[1] = { bwa };
        invoke_chk(setBW, mesh, a);
        if (g_last_exc) { LOGE("gpu: set_boneWeights threw"); return nullptr; }
    }

    const MethodInfo* recalc = find_method_any(MeshCls, "RecalculateBounds");
    if (recalc) invoke_inst(recalc, mesh, nullptr);

    if (outNv) *outNv = (int)nv;
    LOGI("gpu: mesh built nv=%u ni=%u", nv, ni);
    return mesh;
}

// ---------------------------------------------------------------
// 给原 SMR 换材质（克隆它自己的材质再换贴图）
// ---------------------------------------------------------------
static bool retint_material(Il2CppObject* smr, const char* texPath) {
    Il2CppClass* MatCls = find_class_any("UnityEngine", "Material");
    Il2CppClass* TexCls = find_class_any("UnityEngine", "Texture2D");
    Il2CppClass* ByteCls = find_class_any("System", "Byte");
    Il2CppClass* RCls = find_class_any("UnityEngine", "Renderer");
    if (!MatCls || !RCls) return false;
    const MethodInfo* gm = find_method_any(RCls, "get_sharedMaterial");
    Il2CppObject* src = gm ? invoke_inst(gm, smr, nullptr) : nullptr;
    if (!src) { LOGE("gpu: no source material"); return false; }

    Il2CppObject* mat = g_api.object_new(MatCls);
    const MethodInfo* mctor1 = find_method(MatCls, ".ctor", 1);
    if (mctor1) { void* b[1] = { src }; invoke_inst(mctor1, mat, b); }
    const MethodInfo* setMat = find_method(RCls, "set_sharedMaterial", 1);
    if (setMat) { void* a[1] = { mat }; invoke_inst(setMat, smr, a); }
    const MethodInfo* setCol = find_method(MatCls, "set_color", 1);
    if (setCol) { Color4 wc = {1,1,1,1}; void* ca[1] = { &wc }; invoke_inst(setCol, mat, ca); }

    FILE* tf = fopen(texPath, "rb");
    if (!tf) { LOGE("gpu: tex open fail %s", texPath); return true; }
    char tm[4]; uint32_t tw = 0, th = 0;
    if (fread(tm,1,4,tf) != 4 || memcmp(tm,"MMTX",4) != 0 ||
        fread(&tw,4,1,tf) != 1 || fread(&th,4,1,tf) != 1) { fclose(tf); return true; }
    if (tw == 0 || th == 0 || tw > 4096 || th > 4096) { fclose(tf); return true; }
    size_t nbytes = (size_t)tw * th * 4;
    std::vector<unsigned char> px(nbytes);
    if (fread(px.data(),1,nbytes,tf) != nbytes) { fclose(tf); return true; }
    fclose(tf);
    if (!TexCls || !ByteCls) return true;

    const MethodInfo* tctor4 = find_method(TexCls, ".ctor", 4);
    Il2CppObject* tex = g_api.object_new(TexCls);
    if (tctor4) {
        int wi = (int)tw, hi = (int)th, fmt = 4; bool mip = false;
        void* ta[4] = { &wi, &hi, &fmt, &mip };
        invoke_inst(tctor4, tex, ta);
    }
    const MethodInfo* loadRaw = find_method(TexCls, "LoadRawTextureData", 1);
    const MethodInfo* apply1 = find_method(TexCls, "Apply", 1);
    if (tex && loadRaw) {
        Il2CppArray* barr = g_api.array_new(ByteCls, nbytes);
        memcpy(array_addr(barr,1,0), px.data(), nbytes);
        void* la[1] = { barr };
        invoke_inst(loadRaw, tex, la);
        if (apply1) { bool upd = false; void* aa[1] = { &upd }; invoke_inst(apply1, tex, aa); }
        const MethodInfo* setMain = find_method(MatCls, "set_mainTexture", 1);
        if (setMain) { void* sa[1] = { tex }; invoke_inst(setMain, mat, sa); }
        LOGI("gpu: tex ok %ux%u", tw, th);
    }
    return true;
}

static Il2CppObject* g_replacedSmr = nullptr;

static void destroy_one_obj(Il2CppObject* o) {
    if (!o) return;
    if (!*(void**)((char*)o + 16)) return;
    Il2CppClass* ObjectCls = find_class_any("UnityEngine", "Object");
    const MethodInfo* destroy = ObjectCls ? find_method(ObjectCls, "Destroy", 1) : nullptr;
    if (!destroy) return;
    void* a[1] = { o };
    invoke_inst(destroy, nullptr, a);
}

static void destroy_our_smrs() {
    // v116 不再克隆，只有还原逻辑；这里保留接口给外部调用
    g_replacedSmr = nullptr;
    g_ourSmrs.clear();
}

// ---------------------------------------------------------------
// 直接在原 SMR 上换 mesh
// ---------------------------------------------------------------
static int replace_original_smr(Il2CppObject* SMRCls_unused, Il2CppObject* smr,
                                const char* meshPath, const char* texPath,
                                int N, const std::vector<std::string>& gameNorm,
                                float k) {
    (void)SMRCls_unused;
    Il2CppClass* SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    const MethodInfo* getSM = SMRCls ? find_method_any(SMRCls, "get_sharedMesh") : nullptr;
    if (!getSM) { LOGE("gpu: no get_sharedMesh"); return 0; }

    Il2CppObject* origMesh = invoke_inst(getSM, smr, nullptr);
    void* origNat = origMesh ? *(void**)((char*)origMesh + 16) : nullptr;
    LOGI("gpu: original mesh=%p nat=%p", origMesh, origNat);
    if (!origMesh || !origNat) { LOGE("gpu: no original mesh"); return 0; }

    // 先读一遍每根骨骼的世界 Y（用来算自适应缩放 k）
    std::vector<float> boneY;
    {
        Il2CppClass* TrCls = find_class_any("UnityEngine", "Transform");
        const MethodInfo* gb4 = SMRCls ? find_method_any(SMRCls, "get_bones") : nullptr;
        const MethodInfo* l2w = TrCls ? find_method_any(TrCls, "get_localToWorldMatrix") : nullptr;
        Il2CppArray* ba = gb4 ? (Il2CppArray*)invoke_inst(gb4, smr, nullptr) : nullptr;
        if (ba && l2w) {
            boneY.assign((size_t)N, 0.0f);
            for (int i = 0; i < N && i < (int)ba->max_length; i++) {
                Il2CppObject* b = *(Il2CppObject**)array_addr(ba, 8, (size_t)i);
                if (!b) continue;
                Il2CppObject* m = invoke_inst(l2w, b, nullptr);
                if (!m) continue;
                const float* mm = (const float*)((char*)m + 16);
                boneY[i] = mm[13];
            }
        }
    }

    // ===== 方案 A：直接把原 mesh 的内容重写成我们的模型（不换引用）=====
    int nv = 0, found = 0;
    int okA = rewrite_original_mesh(origMesh, meshPath, gameNorm, N, 0, k, &nv, &found,
                                    boneY.empty() ? nullptr : boneY.data());
    if (okA) {
        Il2CppObject* back = invoke_inst(getSM, smr, nullptr);
        LOGI("gpu: sharedMesh after rewrite = %p (same obj? %d)", back, back == origMesh);
        LOGI("gpu: *** REWRITE OK *** nv=%d bones=%d/%d", nv, found, N);

        // 关键：mesh 的顶点数/bindposes 数都变了，SMR 内部缓存的蒙皮缓冲要重建，
        // 否则下一帧渲染会拿旧缓冲读新数据 -> 越界 -> SIGSEGV
        Il2CppClass* BehCls = find_class_any("UnityEngine", "Behaviour");
        const MethodInfo* setEn = BehCls ? find_method(BehCls, "set_enabled", 1) : nullptr;
        if (setEn) {
            LOGI("gpu: step1 disable renderer");
            uint8_t off = 0; void* a1[1] = { &off }; invoke_inst(setEn, smr, a1);
            LOGI("gpu: step2 enable renderer");
            uint8_t on = 1;  void* a2[1] = { &on };  invoke_inst(setEn, smr, a2);
            LOGI("gpu: step3 renderer re-enabled");
        } else {
            LOGI("gpu: no set_enabled, skipping rebuild");
        }

        // 诊断：读回 bindposes 和骨骼世界矩阵，看 bind 时乘积是否接近单位阵
        {
            Il2CppClass* MeshCls2 = find_class_any("UnityEngine", "Mesh");
            Il2CppClass* TrCls = find_class_any("UnityEngine", "Transform");
            const MethodInfo* gbp = MeshCls2 ? find_method_any(MeshCls2, "get_bindposes") : nullptr;
            const MethodInfo* gb3 = SMRCls ? find_method_any(SMRCls, "get_bones") : nullptr;
            const MethodInfo* l2w = TrCls ? find_method_any(TrCls, "get_localToWorldMatrix") : nullptr;
            Il2CppArray* bp = gbp ? (Il2CppArray*)invoke_inst(gbp, origMesh, nullptr) : nullptr;
            Il2CppArray* ba = gb3 ? (Il2CppArray*)invoke_inst(gb3, smr, nullptr) : nullptr;
            LOGI("gpu: diag bindposes=%p len=%d, bones=%p len=%d",
                 bp, bp ? (int)bp->max_length : -1, ba, ba ? (int)ba->max_length : -1);
            for (int i = 0; i < 4 && bp && ba; i++) {
                if (i >= (int)bp->max_length || i >= (int)ba->max_length) break;
                const float* B = (const float*)array_addr(bp, 64, (size_t)i);
                Il2CppObject* bone = *(Il2CppObject**)array_addr(ba, 8, (size_t)i);
                LOGI("gpu: BP[%d] T=(%.3f %.3f %.3f) diag=(%.3f %.3f %.3f)",
                     i, B[12], B[13], B[14], B[0], B[5], B[10]);
                if (bone && l2w) {
                    Il2CppObject* mtx = invoke_inst(l2w, bone, nullptr);
                    if (mtx) {
                        const float* W = (const float*)((char*)mtx + 16);
                        LOGI("gpu: BW[%d] T=(%.3f %.3f %.3f) diag=(%.3f %.3f %.3f)",
                             i, W[12], W[13], W[14], W[0], W[5], W[10]);
                    }
                }
            }
        }

        // 材质先不动：retint_material 里有 fopen，正是 v112 fdsan 的老病根。
        // 先确认 mesh 改动本身不崩，贴图下一步再上。
        g_replacedSmr = smr;
        LOGI("gpu: REWRITE path finished (material left untouched)");
        return 1;
    }
    LOGE("gpu: rewrite on existing mesh failed, trying pointer swap");

    // ===== 方案 B：换 mesh 引用（裸指针再 instanceID）=====
    int nv2 = 0, found2 = 0;
    Il2CppObject* ours = build_skinned_mesh(meshPath, gameNorm, N, 0, k, &nv2, &found2);
    if (!ours) { LOGE("gpu: build_skinned_mesh failed"); return 0; }
    void* oursNat = *(void**)((char*)ours + 16);

    swap_mesh_native(smr, origNat, oursNat, "ptr");
    Il2CppObject* back = invoke_inst(getSM, smr, nullptr);
    void* backNat = back ? *(void**)((char*)back + 16) : nullptr;
    if (backNat == oursNat) {
        LOGI("gpu: *** REPLACE OK (ptr) ***");
        retint_material(smr, texPath);
        g_replacedSmr = smr;
        return 1;
    }
    LOGI("gpu: ptr swap no effect (backNat=%p oursNat=%p)", backNat, oursNat);
    restore_mesh_native(smr, origNat);

    int oldId = get_instance_id(origMesh);
    int newId = get_instance_id(ours);
    LOGI("gpu: instanceID orig=%d ours=%d", oldId, newId);
    if (oldId && newId) {
        int h2 = patch_by_instance_id(smr, oldId, newId, "id");
        if (h2) {
            back = invoke_inst(getSM, smr, nullptr);
            backNat = back ? *(void**)((char*)back + 16) : nullptr;
            if (backNat == oursNat) {
                LOGI("gpu: *** REPLACE OK (instanceID) ***");
                retint_material(smr, texPath);
                g_replacedSmr = smr;
                return 1;
            }
            LOGE("gpu: id swap readback nat=%p want=%p -> FAIL", backNat, oursNat);
            restore_by_instance_id(smr, oldId);
        }
    }
    LOGE("gpu: REPLACE FAIL -> fallback static");
    return 0;
}

// ---------------------------------------------------------------

// =====================================================================
// v123 刚性分段：不蒙皮，每块模型挂到对应骨骼的 Transform 下面
//
// 之前所有方案都死在 Unity 的 bindposes 语义上：官方定义是「骨骼 bind 姿态
// 世界矩阵的逆」，我实际用的是「把顶点移到骨骼原点的平移」。单骨骼看不出差别，
// 多骨骼权重的顶点在不同骨骼下算出不同位置，接缝必然裂。
//
// 这个方案完全不碰蒙皮：
//   Python 已经把模型切成 27 块（每块是一个骨骼的顶点，坐标已减去该骨骼的 bind 位置）
//   C++ 每块建一个 GameObject，parent 到那根骨骼的 Transform，localPosition=0
//   骨骼怎么动，块就怎么动 —— 引擎的父子变换，没有我能算错的地方
// =====================================================================


// ---------------------------------------------------------------
// 按骨骼相对位置匹配（名字对不上时用）
//   PMX 骨骼位置和游戏骨骼世界位置各自归一化（减去均值、除以最大半径），
//   找最近的一根；带左右约束避免 L/R 互换。
// ---------------------------------------------------------------
static int match_bones_by_position(const char* dir,
                                   const std::vector<std::string>& gnorm,
                                   const std::vector<Il2CppObject*>& bones, int N,
                                   std::vector<int>& gidx) {
    char path[400];
    snprintf(path, sizeof(path), "%sbonepos.txt", dir);
    FILE* f = fopen(path, "r");
    if (!f) { LOGE("rigid: bonepos.txt not found, skip position matching"); return 0; }
    std::vector<float> ppos;
    std::vector<std::string> pnames;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int idx; char nm[128]; float x, y, z;
        if (sscanf(line, "%d %127s %f %f %f", &idx, nm, &x, &y, &z) == 5) {
            pnames.push_back(std::string(nm));
            ppos.push_back(x); ppos.push_back(y); ppos.push_back(z);
        }
    }
    fclose(f);
    int nb = (int)pnames.size();
    if (nb == 0) return 0;

    Il2CppClass* TrCls = find_class_any("UnityEngine", "Transform");
    const MethodInfo* l2w = TrCls ? find_method_any(TrCls, "get_localToWorldMatrix") : nullptr;
    if (!l2w) return 0;

    std::vector<float> gpos((size_t)N*3, 0.0f);
    std::vector<char> gok((size_t)N, 0);
    for (int j = 0; j < N; j++) {
        if (!bones[j]) continue;
        Il2CppObject* m = invoke_inst(l2w, bones[j], nullptr);
        if (!m) continue;
        const float* mm = (const float*)((char*)m + 16);
        gpos[j*3] = mm[12]; gpos[j*3+1] = mm[13]; gpos[j*3+2] = mm[14];
        gok[j] = 1;
    }

    float pc[3] = {0,0,0}, gc[3] = {0,0,0};
    int pn = nb, gc2 = 0;
    for (int i = 0; i < nb; i++) { pc[0]+=ppos[i*3]; pc[1]+=ppos[i*3+1]; pc[2]+=ppos[i*3+2]; }
    for (int j = 0; j < N; j++) if (gok[j]) { gc[0]+=gpos[j*3]; gc[1]+=gpos[j*3+1]; gc[2]+=gpos[j*3+2]; gc2++; }
    if (!pn || !gc2) return 0;
    for (int k = 0; k < 3; k++) { pc[k] /= pn; gc[k] /= gc2; }

    float pr = 0.0f, gr = 0.0f;
    for (int i = 0; i < nb; i++) {
        float dx=ppos[i*3]-pc[0], dy=ppos[i*3+1]-pc[1], dz=ppos[i*3+2]-pc[2];
        float d = sqrtf(dx*dx+dy*dy+dz*dz); if (d > pr) pr = d;
    }
    for (int j = 0; j < N; j++) if (gok[j]) {
        float dx=gpos[j*3]-gc[0], dy=gpos[j*3+1]-gc[1], dz=gpos[j*3+2]-gc[2];
        float d = sqrtf(dx*dx+dy*dy+dz*dz); if (d > gr) gr = d;
    }
    if (pr < 0.001f || gr < 0.001f) return 0;

    // 名字已经占掉的骨骼先标成已用（同名的多对一是合理的，比如 Spine/Spine1/Spine2）
    std::vector<char> used((size_t)N, 0);
    int matched = 0;
    for (int i = 0; i < nb; i++) if (gidx[i] >= 0) { used[gidx[i]] = 1; matched++; }

    // 只保留严格一对一（0.03）。放宽阈值会猜错，猜错就会让整块跑到错误位置。
    const float thr[2] = {0.03f, 0.03f};
    for (int round = 0; round < 1; round++) {
        for (int i = 0; i < nb; i++) {
            if (gidx[i] >= 0) continue;
            float px=(ppos[i*3]-pc[0])/pr, py=(ppos[i*3+1]-pc[1])/pr, pz=(ppos[i*3+2]-pc[2])/pr;
            int best = -1; float bd = 1e18f;
            for (int j = 0; j < N; j++) {
                if (!gok[j]) continue;
                if (round == 0 && used[j]) continue;        // 严格轮：不许抢已占的骨骼
                float qx=(gpos[j*3]-gc[0])/gr, qy=(gpos[j*3+1]-gc[1])/gr, qz=(gpos[j*3+2]-gc[2])/gr;
                if (px > 0.05f && qx < -0.05f) continue;    // 左右不许互换（收紧）
                if (px < -0.05f && qx > 0.05f) continue;
                float dx=px-qx, dy=py-qy, dz=pz-qz;
                float d = dx*dx+dy*dy+dz*dz;
                if (d < bd) { bd = d; best = j; }
            }
            if (best >= 0 && bd < thr[round]) {
                gidx[i] = best; used[best] = 1; matched++;
                LOGI("rigid: posmap[%2d] %-18s -> bone[%2d](%s) d2=%.4f r%d",
                     i, pnames[i].c_str(), best, gnorm[best].c_str(), bd, round);
            } else if (round == 1) {
                LOGI("rigid: posmap[%2d] %-18s -> none (best d2=%.4f)", i, pnames[i].c_str(), bd);
            }
        }
        if (matched >= nb * 3 / 4) break;
    }
    LOGI("rigid: position matching done, %d/%d mapped", matched, nb);
    return matched;
}


// ---------------------------------------------------------------
// 方案 A：用 Unity 的 Humanoid 骨骼语义 API 拿骨骼
// Animator.GetBoneTransform(HumanBodyBones) 直接按"语义"返回 Transform，
// 完全不用管骨骼叫什么名字、在哪个位置。前提是角色用 Humanoid rig。
// 枚举值（Unity 2018.4）：
//   0 Hips 1 LUpperLeg 2 RUpperLeg 3 LLowerLeg 4 RLowerLeg 5 LFoot 6 RFoot
//   7 Spine 8 Chest 9 Neck 10 Head 11 LShoulder 12 RShoulder
//   13 LUpperArm 14 RUpperArm 15 LLowerArm 16 RLowerArm 17 LHand 18 RHand
//   19 LToes 20 RToes
// ---------------------------------------------------------------
static const int kHbbMap[22] = {
    0,   // 0  Pelvis      -> Hips
    7,   // 1  Spine       -> Spine
    8,   // 2  Spine1      -> Chest
    8,   // 3  Spine2      -> Chest（2018 没有 UpperChest，重复用）
    9,   // 4  Neck        -> Neck
    10,  // 5  Head        -> Head
    1,   // 6  L Thigh     -> LeftUpperLeg
    3,   // 7  L Calf      -> LeftLowerLeg
    5,   // 8  L Foot      -> LeftFoot
    19,  // 9  L Toe0      -> LeftToes
    2,   // 10 R Thigh     -> RightUpperLeg
    4,   // 11 R Calf      -> RightLowerLeg
    6,   // 12 R Foot      -> RightFoot
    20,  // 13 R Toe0      -> RightToes
    11,  // 14 L Clavicle  -> LeftShoulder
    13,  // 15 L UpperArm  -> LeftUpperArm
    15,  // 16 L Forearm   -> LeftLowerArm
    17,  // 17 L Hand      -> LeftHand
    12,  // 18 R Clavicle  -> RightShoulder
    14,  // 19 R UpperArm  -> RightUpperArm
    16,  // 20 R Forearm   -> RightLowerArm
    18,  // 21 R Hand      -> RightHand
};

static int try_humanoid_bones(Il2CppObject* origSmr, std::vector<Il2CppObject*>& out) {
    out.assign(22, nullptr);
    Il2CppClass* AnimCls = find_class_any("UnityEngine", "Animator");
    if (!AnimCls) { LOGE("rigid: no Animator class"); return 0; }
    const MethodInfo* gbt = find_method_any(AnimCls, "GetBoneTransform");
    if (!gbt) { LOGE("rigid: Animator.GetBoneTransform STRIPPED"); return 0; }

    Il2CppClass* CompCls = find_class_any("UnityEngine", "Component");
    const MethodInfo* getGO = CompCls ? find_method_any(CompCls, "get_gameObject") : nullptr;
    const MethodInfo* getComp = CompCls ? find_method(CompCls, "GetComponent", 1) : nullptr;
    const MethodInfo* getCompParent = CompCls ? find_method_any(CompCls, "GetComponentInParent") : nullptr;
    if (!getGO || !getComp) { LOGE("rigid: component api missing"); return 0; }

    void* t[1] = { g_api.type_get_object(g_api.class_get_type(AnimCls)) };
    Il2CppObject* anim = nullptr;
    Il2CppObject* go = invoke_inst(getGO, origSmr, nullptr);
    if (go) anim = invoke_inst(getComp, go, t);
    if (!anim && getCompParent) anim = invoke_inst(getCompParent, origSmr, t);
    if (!anim) { LOGE("rigid: no Animator on/near SMR"); return 0; }
    LOGI("rigid: animator=%p", anim);

    int got = 0;
    for (int i = 0; i < 22; i++) {
        int id = kHbbMap[i];
        void* b[1] = { &id };
        Il2CppObject* tr = invoke_inst(gbt, anim, b);
        out[i] = tr;
        if (tr) {
            got++;
            LOGI("rigid: hbb[%2d] id=%-2d -> %s", i, id, obj_name(tr).c_str());
        } else {
            LOGI("rigid: hbb[%2d] id=%-2d -> NULL", i, id);
        }
    }
    LOGI("rigid: humanoid bones %d/22", got);
    return got;
}


// ---------------------------------------------------------------
// 把目标角色的骨骼层级整棵树打出来（名字 + 深度 + 子骨骼数 + 世界坐标）
// 人形骨架拓扑是固定的：根只有 3 个子（脊椎/左腿/右腿），腿链 4 层，臂链从肩往下 4 层。
// 有了这棵树，匹配就不需要猜名字了 —— 看形状就知道哪根是哪根。
// ---------------------------------------------------------------
static void dump_bone_tree(Il2CppObject* origSmr) {
    Il2CppClass* SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    if (!SMRCls) return;
    const MethodInfo* gb = find_method_any(SMRCls, "get_bones");
    Il2CppArray* ba = gb ? (Il2CppArray*)invoke_inst(gb, origSmr, nullptr) : nullptr;
    if (!ba || ba->max_length == 0) { LOGE("bt: no bones"); return; }
    int N = (int)ba->max_length;

    Il2CppClass* TrCls = find_class_any("UnityEngine", "Transform");
    if (!TrCls) return;
    const MethodInfo* getParent = find_method_any(TrCls, "get_parent");
    const MethodInfo* getCount  = find_method_any(TrCls, "get_childCount");
    const MethodInfo* l2w       = find_method_any(TrCls, "get_localToWorldMatrix");

    std::vector<Il2CppObject*> bones((size_t)N, nullptr);
    std::set<Il2CppObject*> bset;
    for (int i = 0; i < N; i++) {
        Il2CppObject* b = *(Il2CppObject**)array_addr(ba, 8, (size_t)i);
        bones[i] = b;
        if (b) bset.insert(b);
    }

    std::map<Il2CppObject*, std::vector<Il2CppObject*> > tree;
    std::vector<Il2CppObject*> roots;
    for (int i = 0; i < N; i++) {
        Il2CppObject* b = bones[i];
        if (!b) continue;
        Il2CppObject* par = getParent ? invoke_inst(getParent, b, nullptr) : nullptr;
        if (par && bset.count(par)) tree[par].push_back(b);
        else roots.push_back(b);
    }

    const MethodInfo* getLP = find_method_any(TrCls, "get_localPosition");
    const MethodInfo* getLR = find_method_any(TrCls, "get_localRotation");
    const MethodInfo* getLS = find_method_any(TrCls, "get_localScale");

    LOGI("=== BONE TREE %d bones / %d roots ===", N, (int)roots.size());

    struct Item { Il2CppObject* t; int depth; };
    std::vector<Item> stack;
    for (int i = (int)roots.size() - 1; i >= 0; i--) { Item it; it.t = roots[i]; it.depth = 0; stack.push_back(it); }

    int printed = 0;
    while (!stack.empty() && printed < 260) {
        Item it = stack.back(); stack.pop_back();
        std::string nm = obj_name(it.t);
        int nc = 0;
        if (getCount) { Il2CppObject* r = invoke_inst(getCount, it.t, nullptr); if (r) nc = *(int*)((char*)r + 16); }
        float wx = 0, wy = 0, wz = 0;
        if (l2w) {
            Il2CppObject* m = invoke_inst(l2w, it.t, nullptr);
            if (m) { const float* mm = (const float*)((char*)m + 16); wx = mm[12]; wy = mm[13]; wz = mm[14]; }
        }
        char indent[80]; int k = 0;
        for (int d = 0; d < it.depth && k < 74; d++) { indent[k++] = '.'; indent[k++] = ' '; }
        indent[k] = 0;

        // 父骨骼名
        Il2CppObject* par = getParent ? invoke_inst(getParent, it.t, nullptr) : nullptr;
        std::string pname = par ? obj_name(par) : std::string("ROOT");

        // localPosition / localRotation / localScale
        float lx = 0, ly = 0, lz = 0, qx = 0, qy = 0, qz = 0, qw = 1, sx = 1, sy = 1, sz = 1;
        if (getLP) { Il2CppObject* r = invoke_inst(getLP, it.t, nullptr); if (r) { lx=*(float*)((char*)r+16); ly=*(float*)((char*)r+20); lz=*(float*)((char*)r+24); } }
        if (getLR) { Il2CppObject* r = invoke_inst(getLR, it.t, nullptr); if (r) { qx=*(float*)((char*)r+16); qy=*(float*)((char*)r+20); qz=*(float*)((char*)r+24); qw=*(float*)((char*)r+28); } }
        if (getLS) { Il2CppObject* r = invoke_inst(getLS, it.t, nullptr); if (r) { sx=*(float*)((char*)r+16); sy=*(float*)((char*)r+20); sz=*(float*)((char*)r+24); } }

        LOGI("BT %s%s  ch=%d  par=%s", indent, nm.c_str(), nc, pname.c_str());
        LOGI("    lpos=(%.3f,%.3f,%.3f) lrot=(%.3f,%.3f,%.3f,%.3f) lscl=(%.3f,%.3f,%.3f) wpos=(%.2f,%.2f,%.2f)",
             lx, ly, lz, qx, qy, qz, qw, sx, sy, sz, wx, wy, wz);
        printed++;

        std::vector<Il2CppObject*>& ch = tree[it.t];
        for (int i = (int)ch.size() - 1; i >= 0; i--) { Item c; c.t = ch[i]; c.depth = it.depth + 1; stack.push_back(c); }
    }
    LOGI("=== BONE TREE END (%d printed) ===", printed);
}


// ---------------------------------------------------------------
// 角色的骨骼分散在多个 SkinnedMeshRenderer 里：
// 躯干那个 SMR 的 bones 只有 Pelvis/Spine/Neck/Head/Clavicle 这几根，
// 四肢的主骨骼在别的 SMR 里。只取一个 SMR 的 bones 永远凑不齐 22 根。
// 这里把同一角色根节点下所有 SMR 的 bones 并起来。
// ---------------------------------------------------------------
static int collect_role_bones(Il2CppObject* anySmr,
                              std::vector<Il2CppObject*>& outBones,
                              std::vector<std::string>& outNorm) {
    outBones.clear();
    outNorm.clear();
    Il2CppClass* SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    if (!SMRCls) return 0;
    const MethodInfo* gb = find_method_any(SMRCls, "get_bones");
    if (!gb) return 0;

    Il2CppObject* roleRoot = find_role_root(anySmr);
    LOGI("rigid: collect_role_bones root=%p", roleRoot);

    std::vector<Fingerprint> list;
    collect_fingerprints(SMRCls, list);
    LOGI("rigid: role has %d SMRs", (int)list.size());

    std::set<Il2CppObject*> seen;
    for (size_t i = 0; i < list.size(); i++) {
        Il2CppObject* sm = list[i].smr;
        if (!sm || !*(void**)((char*)sm + 16)) continue;
        // 同角色才算（角色根节点相同）
        if (roleRoot) {
            Il2CppObject* r2 = find_role_root(sm);
            if (r2 != roleRoot) continue;
        }
        Il2CppArray* a = (Il2CppArray*)invoke_inst(gb, sm, nullptr);
        if (!a) continue;
        int n = (int)a->max_length;
        int added = 0;
        for (int j = 0; j < n; j++) {
            Il2CppObject* b = *(Il2CppObject**)array_addr(a, 8, (size_t)j);
            if (!b) continue;
            if (seen.count(b)) continue;
            seen.insert(b);
            outBones.push_back(b);
            outNorm.push_back(norm_bone(obj_name(b), false));
            added++;
        }
        LOGI("rigid:   smr[%d] %s bones=%d new=%d", (int)i, list[i].chain.c_str(), n, added);
    }
    LOGI("rigid: union bones = %d", (int)outBones.size());
    return (int)outBones.size();
}


// ---------------------------------------------------------------
// 按父子层级补匹配。
// 目标角色把前臂/手/脚改名叫 Bone12/Bone16/Bone41 之类，名字匹配永远对不上，
// 但这些骨骼的父级（UpperArm / Calf / Spine1）都已经被名字匹配到了。
// 所以：从已匹配的父骨骼出发，沿子节点往下走，就能把剩下的补齐。
// ---------------------------------------------------------------
static const int kParentIdx[22] = {
    -1,  // 0  Pelvis
     0,  // 1  Spine      <- Pelvis
     1,  // 2  Spine1     <- Spine
     2,  // 3  Spine2     <- Spine1
     3,  // 4  Neck       <- Spine2
     4,  // 5  Head       <- Neck
     0,  // 6  L Thigh    <- Pelvis
     6,  // 7  L Calf     <- L Thigh
     7,  // 8  L Foot     <- L Calf
     8,  // 9  L Toe0     <- L Foot
     0,  // 10 R Thigh    <- Pelvis
    10,  // 11 R Calf     <- R Thigh
    11,  // 12 R Foot     <- R Calf
    12,  // 13 R Toe0     <- R Foot
     3,  // 14 L Clavicle <- Spine2
    14,  // 15 L UpperArm <- L Clavicle
    15,  // 16 L Forearm  <- L UpperArm
    16,  // 17 L Hand     <- L Forearm
     3,  // 18 R Clavicle <- Spine2
    18,  // 19 R UpperArm <- R Clavicle
    19,  // 20 R Forearm  <- R UpperArm
    20,  // 21 R Hand     <- R Forearm
};

static int match_by_hierarchy(Il2CppClass* TrCls,
                              std::vector<Il2CppObject*>& bones, int N,
                              std::vector<int>& gidx) {
    if (!TrCls) return 0;
    const MethodInfo* getCount = find_method_any(TrCls, "get_childCount");
    const MethodInfo* getChild = find_method_any(TrCls, "GetChild");
    if (!getCount || !getChild) { LOGE("rigid: no child api"); return 0; }

    std::vector<char> used((size_t)N, 0);
    for (int i = 0; i < 22; i++)
        if (gidx[i] >= 0 && gidx[i] < N) used[gidx[i]] = 1;

    int added = 0;
    // kParentIdx 里父的索引一定小于子，顺序遍历就能保证父先处理
    for (int pass = 0; pass < 2; pass++) {
        bool progress = false;
        for (int i = 0; i < 22; i++) {
            if (gidx[i] >= 0) continue;
            int pi = kParentIdx[i];
            if (pi < 0 || gidx[pi] < 0 || gidx[pi] >= N) continue;
            Il2CppObject* par = bones[gidx[pi]];
            if (!par) continue;

            Il2CppObject* r = invoke_inst(getCount, par, nullptr);
            int nc = r ? *(int*)((char*)r + 16) : 0;
            if (nc <= 0 || nc > 64) continue;

            // 收集还没被占用的子骨骼
            std::vector<int> cand;
            for (int c = 0; c < nc; c++) {
                int ci = c;
                void* a[1] = { &ci };
                Il2CppObject* ch = invoke_inst(getChild, par, a);
                if (!ch) continue;
                for (int k = 0; k < N; k++) {
                    if (bones[k] == ch) {
                        if (!used[k]) cand.push_back(k);
                        break;
                    }
                }
            }
            if (cand.size() != 1) {
                // 0 个：父这边没有可用子；多个：分不清是哪个（比如 Spine1 下面
                // 同时挂着 Spine2 和一堆装饰骨），留给下一步，别乱猜
                continue;
            }
            gidx[i] = cand[0];
            used[cand[0]] = 1;
            added++;
            progress = true;
            LOGI("rigid: hier[%2d] <- bone[%d](%s) via parent[%d]",
                 i, cand[0], obj_name(bones[cand[0]]).c_str(), pi);
        }
        if (!progress) break;
    }
    LOGI("rigid: hierarchy added %d", added);
    return added;
}


// ---------------------------------------------------------------
// 雷霆大导出：把场景里所有 SMR、所有骨骼、完整层级一次性写到文件。
// 不走 logcat（会被截断），直接 fopen 写到游戏私有目录，再用 su 拷出来分析。
// ---------------------------------------------------------------
static void dump_tree_to(FILE* f, Il2CppObject* smr, const char* tag) {
    Il2CppClass* SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    Il2CppClass* TrCls  = find_class_any("UnityEngine", "Transform");
    if (!SMRCls || !TrCls) return;
    const MethodInfo* gb        = find_method_any(SMRCls, "get_bones");
    const MethodInfo* getParent = find_method_any(TrCls, "get_parent");
    const MethodInfo* getCount  = find_method_any(TrCls, "get_childCount");
    const MethodInfo* l2w       = find_method_any(TrCls, "get_localToWorldMatrix");
    const MethodInfo* getLP     = find_method_any(TrCls, "get_localPosition");
    const MethodInfo* getLR     = find_method_any(TrCls, "get_localRotation");
    const MethodInfo* getLS     = find_method_any(TrCls, "get_localScale");

    Il2CppArray* ba = gb ? (Il2CppArray*)invoke_inst(gb, smr, nullptr) : nullptr;
    if (!ba) { fprintf(f, "%s bones=NULL\n", tag); return; }
    int N = (int)ba->max_length;

    std::vector<Il2CppObject*> bones((size_t)N, nullptr);
    std::set<Il2CppObject*> bset;
    for (int i = 0; i < N; i++) {
        Il2CppObject* b = *(Il2CppObject**)array_addr(ba, 8, (size_t)i);
        bones[i] = b;
        if (b) bset.insert(b);
    }

    std::map<Il2CppObject*, std::vector<Il2CppObject*> > tree;
    std::vector<Il2CppObject*> roots;
    for (int i = 0; i < N; i++) {
        Il2CppObject* b = bones[i];
        if (!b) continue;
        Il2CppObject* par = getParent ? invoke_inst(getParent, b, nullptr) : nullptr;
        if (par && bset.count(par)) tree[par].push_back(b);
        else roots.push_back(b);
    }

    fprintf(f, "\n===== %s  bones=%d roots=%d =====\n", tag, N, (int)roots.size());

    struct Item { Il2CppObject* t; int depth; };
    std::vector<Item> stack;
    for (int i = (int)roots.size() - 1; i >= 0; i--) { Item it; it.t = roots[i]; it.depth = 0; stack.push_back(it); }

    int printed = 0;
    while (!stack.empty() && printed < 400) {
        Item it = stack.back(); stack.pop_back();
        std::string nm = obj_name(it.t);
        int nc = 0;
        if (getCount) { Il2CppObject* r = invoke_inst(getCount, it.t, nullptr); if (r) nc = *(int*)((char*)r + 16); }

        float wx = 0, wy = 0, wz = 0, lx = 0, ly = 0, lz = 0, qx = 0, qy = 0, qz = 0, qw = 1;
        if (l2w) { Il2CppObject* m = invoke_inst(l2w, it.t, nullptr); if (m) { const float* mm = (const float*)((char*)m+16); wx=mm[12]; wy=mm[13]; wz=mm[14]; } }
        if (getLP) { Il2CppObject* r = invoke_inst(getLP, it.t, nullptr); if (r) { lx=*(float*)((char*)r+16); ly=*(float*)((char*)r+20); lz=*(float*)((char*)r+24); } }
        if (getLR) { Il2CppObject* r = invoke_inst(getLR, it.t, nullptr); if (r) { qx=*(float*)((char*)r+16); qy=*(float*)((char*)r+20); qz=*(float*)((char*)r+24); qw=*(float*)((char*)r+28); } }

        // 在 bones[] 里的下标
        int idx = -1;
        for (int k = 0; k < N; k++) if (bones[k] == it.t) { idx = k; break; }

        for (int d = 0; d < it.depth && d < 20; d++) fputc(' ', f);
        fprintf(f, "[%3d] %-26s ch=%-2d wpos=(%8.2f,%8.2f,%8.2f) lpos=(%7.3f,%7.3f,%7.3f) lrot=(%6.3f,%6.3f,%6.3f,%6.3f)\n",
                idx, nm.c_str(), nc, wx, wy, wz, lx, ly, lz, qx, qy, qz, qw);
        printed++;

        std::vector<Il2CppObject*>& ch = tree[it.t];
        for (int i = (int)ch.size() - 1; i >= 0; i--) { Item c; c.t = ch[i]; c.depth = it.depth + 1; stack.push_back(c); }
    }
    if (printed >= 400) fprintf(f, "  ... truncated at 400\n");
}

static void dump_everything() {
    const char* paths[2] = {
        "/data/user/0/com.hermes.h1game/files/bonedb.txt",
        "/data/data/com.hermes.h1game/files/bonedb.txt",
    };
    FILE* f = nullptr;
    for (int i = 0; i < 2 && !f; i++) f = fopen(paths[i], "w");
    if (!f) { LOGE("dump_everything: cannot open file"); return; }

    Il2CppClass* SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    std::vector<Fingerprint> list;
    if (SMRCls) collect_fingerprints(SMRCls, list);

    fprintf(f, "# ModelMod bonedb  smrs=%d\n", (int)list.size());

    // 我们的 22 根（从 bonepos.txt 读过来对照）
    fprintf(f, "\n##### OURS (PMX 22 bones, p = -bindpose.T) #####\n");
    {
        const char* bp[2] = {
            "/data/user/0/com.hermes.h1game/files/bonepos.txt",
            "/data/data/com.hermes.h1game/files/bonepos.txt",
        };
        FILE* g = nullptr;
        for (int i = 0; i < 2 && !g; i++) g = fopen(bp[i], "r");
        if (g) {
            char line[256];
            while (fgets(line, sizeof(line), g)) fprintf(f, "  %s", line);
            fclose(g);
        } else {
            fprintf(f, "  (bonepos.txt not found)\n");
        }
    }

    // 每个 SMR 一份完整树
    for (size_t i = 0; i < list.size(); i++) {
        Il2CppObject* sm = list[i].smr;
        if (!sm || !*(void**)((char*)sm + 16)) continue;
        char tag[512];
        snprintf(tag, sizeof(tag), "SMR[%d] %s", (int)i, list[i].chain.c_str());
        dump_tree_to(f, sm, tag);
    }

    fclose(f);
    LOGI("dump_everything -> bonedb.txt (%d smrs)", (int)list.size());
    // 标记到 log，方便确认执行了
}


// ---------------------------------------------------------------
// 基于 bonedb.txt 人工分析得出的硬映射（只对这名角色有效，名字对不上时兜底）
// 依据：11001_U_1/Body 的骨骼树
//   L Calf → Bone19 → Bone18 → Bone17       (脚 / 脚趾 / 末端)
//   R Calf → Bone16 → Bone15 → Bone14
//   Bone57 是独立根，wpos 在左手末端
//   Bone38 是独立根，wpos 在右手末端
// ---------------------------------------------------------------
// kHardMap 已上移到 load_skin_part 之前

static int apply_hard_map(const std::vector<std::string>& gnorm, int N, std::vector<int>& gidx) {
    int added = 0;
    int cnt = (int)(sizeof(kHardMap) / sizeof(kHardMap[0]));
    for (int h = 0; h < cnt; h++) {
        int slot = kHardMap[h].slot;
        if (slot < 0 || slot >= 22) continue;
        if (gidx[slot] >= 0) continue;                 // 名字已经匹配上了就别覆盖
        for (int j = 0; j < N; j++) {
            if (gnorm[j] == kHardMap[h].target) {
                gidx[slot] = j;
                added++;
                LOGI("rigid: hardmap[%2d] <- bone[%d](%s)", slot, j, gnorm[j].c_str());
                break;
            }
        }
    }
    LOGI("rigid: hard map added %d", added);
    return added;
}

// ---------------------------------------------------------------
// v141: 从 bindposes 反推「骨骼 bind 那一帧」的世界姿态
//
// 块的 localRotation/localScale 必须抵消 bind 姿态，不是 spawn 那一瞬的姿态。
// 之前的写法是 inverse(骨骼实时 worldRotation)：角色只要不在 bind pose，
// 每块就整体带着 bind→现在的增量旋转——位置对、方向歪就是这么来的。
//
// bindposes[i] = inverse(bone.localToWorldMatrix at bind)
//   → 真 3x3 = (1/s) * R_bind^T
//   → 列长 l = 1/s，列归一化后就是 R_bind^T，正是要的 localRotation
//   → localScale = kBase * l
// ---------------------------------------------------------------
struct BindRef { Quat rot; float sc; bool ok; };

static void quat_from_basis(const float c[3][3], Quat& q) {
    // c[列][行]
    float m00 = c[0][0], m01 = c[1][0], m02 = c[2][0];
    float m10 = c[0][1], m11 = c[1][1], m12 = c[2][1];
    float m20 = c[0][2], m21 = c[1][2], m22 = c[2][2];
    float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s; q.x = (m21 - m12) / s; q.y = (m02 - m20) / s; q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s; q.x = 0.25f * s; q.y = (m01 + m10) / s; q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s; q.x = (m01 + m10) / s; q.y = 0.25f * s; q.z = (m12 + m21) / s;
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s; q.x = (m02 + m20) / s; q.y = (m12 + m21) / s; q.z = 0.25f * s;
    }
}

static void build_bindref_map(Il2CppClass* SMRCls, std::map<void*, BindRef>& out) {
    out.clear();
    if (!SMRCls) return;
    Il2CppClass* ObjectCls = find_class_any("UnityEngine", "Object");
    Il2CppClass* MeshCls   = find_class_any("UnityEngine", "Mesh");
    Il2CppClass* Mat4Cls   = find_class_any("UnityEngine", "Matrix4x4");
    if (!ObjectCls || !MeshCls) { LOGE("bindref: class missing"); return; }
    const MethodInfo* find = find_method(ObjectCls, "FindObjectsOfType", 1);
    const MethodInfo* gb   = find_method_any(SMRCls, "get_bones");
    const MethodInfo* gm   = find_method_any(SMRCls, "get_sharedMesh");
    const MethodInfo* gbp  = find_method_any(MeshCls, "get_bindposes");
    if (!find || !gb || !gm || !gbp) {
        LOGE("bindref: method missing find=%p bones=%p mesh=%p bp=%p",
             (void*)find, (void*)gb, (void*)gm, (void*)gbp);
        return;
    }
    int mstride = array_elem_size(Mat4Cls, 64);
    void* a[1] = { g_api.type_get_object(g_api.class_get_type(SMRCls)) };
    Il2CppArray* arr = (Il2CppArray*)invoke_static(find, a);
    if (!arr) { LOGE("bindref: FindObjectsOfType null"); return; }

    int smrSeen = 0, badLen = 0;
    for (size_t i = 0; i < arr->max_length; i++) {
        Il2CppObject* smr = *(Il2CppObject**)array_addr(arr, sizeof(void*), i);
        if (!smr) continue;
        Il2CppArray* bones = (Il2CppArray*)invoke_inst(gb, smr, nullptr);
        Il2CppObject* mesh  = invoke_inst(gm, smr, nullptr);
        if (!bones || !mesh || bones->max_length == 0) continue;
        Il2CppArray* bps = (Il2CppArray*)invoke_inst(gbp, mesh, nullptr);
        if (!bps || bps->max_length != bones->max_length) { badLen++; continue; }
        smrSeen++;
        for (size_t k = 0; k < bones->max_length; k++) {
            Il2CppObject* b = *(Il2CppObject**)array_addr(bones, sizeof(void*), k);
            if (!b) continue;
            if (out.find((void*)b) != out.end()) continue;   // 同一根骨骼多个 SMR 都有，取第一个
            const float* m = (const float*)array_addr(bps, (size_t)mstride, k);
            // 列主序：第一列 m[0..2]，第二列 m[4..6]，第三列 m[8..10]
            float c[3][3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
            BindRef r; r.ok = false; r.sc = 1.0f; r.rot.x = 0; r.rot.y = 0; r.rot.z = 0; r.rot.w = 1;
            float l0 = sqrtf(c[0][0]*c[0][0] + c[0][1]*c[0][1] + c[0][2]*c[0][2]);
            float l1 = sqrtf(c[1][0]*c[1][0] + c[1][1]*c[1][1] + c[1][2]*c[1][2]);
            float l2 = sqrtf(c[2][0]*c[2][0] + c[2][1]*c[2][1] + c[2][2]*c[2][2]);
            if (l0 > 1e-5f && l1 > 1e-5f && l2 > 1e-5f) {
                float n[3][3];
                for (int q = 0; q < 3; q++) {
                    n[0][q] = c[0][q] / l0;
                    n[1][q] = c[1][q] / l1;
                    n[2][q] = c[2][q] / l2;
                }
                quat_from_basis(n, r.rot);
                r.sc = (l0 + l1 + l2) / 3.0f;
                r.ok = true;
            }
            out[(void*)b] = r;
        }
    }
    LOGI("bindref: %d SMRs usable (%d length mismatches), %d bones with bind pose",
         smrSeen, badLen, (int)out.size());
}

// ---------------------------------------------------------------
// v141: 原生蒙皮可行性验证
// cmd 1 = 顶点整体 x0.5 写回   （验 set_vertices）
// cmd 2 = 全部顶点绑 0 号骨    （验 set_boneWeights）
// cmd 3 = 还原这两项
// ---------------------------------------------------------------
static Il2CppObject* test_target_smr() {
    Il2CppClass* SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    if (!SMRCls) { LOGE("test: no SMR class"); return nullptr; }
    if (g_tgt_sel >= 0) {
       Il2CppObject* s = nullptr;
        pthread_mutex_lock(&g_tgt_mtx);
        if (g_tgt_sel < (int)g_tgt_smrs.size()) s = g_tgt_smrs[g_tgt_sel];
        pthread_mutex_unlock(&g_tgt_mtx);
        if (s && *(void**)((char*)s + 16)) {
            LOGI("test: using picked target idx=%d", (int)g_tgt_sel);
            return s;
        }
    }
    std::vector<Fingerprint> fps;
    collect_fingerprints(SMRCls, fps);
    Il2CppObject* best = nullptr;
    int bestv = 0;
    for (size_t i = 0; i < fps.size(); i++) {
        if (!fps[i].smr) continue;
        if (fps[i].verts <= 100 || fps[i].verts > 200000) continue;
        if (!under_player_root(fps[i].smr)) continue;
        if (fps[i].verts > bestv) { bestv = fps[i].verts; best = fps[i].smr; }
    }
    if (best) LOGI("test: auto target verts=%d", bestv);
    else LOGE("test: no player SMR found (pick a target first)");
    return best;
}

// ---------------------------------------------------------------
// v143: mesh 元信息 + SMR setter 全量探测
// ---------------------------------------------------------------
static void dump_mesh_meta(Il2CppObject* mesh, Il2CppClass* meshCls) {
    const char* names[] = {"get_isReadable", "get_vertexCount", "get_subMeshCount",
                           "get_bindposes", "get_boneWeights", "get_triangles",
                           "get_normals", "get_uv", "get_vertexBufferCount"};
    Il2CppClass* arrCls = find_class_any("System", "Array");
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const MethodInfo* m = find_method_any(meshCls, names[i]);
        if (!m) { LOGI("meta: %-20s MISSING", names[i]); continue; }
        Il2CppObject* r = invoke_inst(m, mesh, nullptr);
        if (!r) { LOGI("meta: %-20s -> null", names[i]); continue; }
        Il2CppClass* rc = g_api.object_get_class(r);
        const char* rn = rc ? g_api.class_get_name(rc) : "?";
        bool isArr = (rc && arrCls && g_api.class_is_assignable_from(rc, arrCls));
        if (isArr) {
            LOGI("meta: %-20s -> ARRAY len=%llu", names[i], (unsigned long long)((Il2CppArray*)r)->max_length);
        } else {
            LOGI("meta: %-20s -> %p cls=%s val16=%d", names[i], (void*)r, rn ? rn : "?", *(int*)((char*)r + 16));
        }
    }
}

static void dump_smr_setters(Il2CppClass* smrCls) {
    const char* names[] = {"set_sharedMesh", "get_sharedMesh", "set_bones", "get_bones",
                           "set_rootBone", "get_rootBone", "set_localBounds", "get_localBounds",
                           "set_updateWhenOffscreen", "get_updateWhenOffscreen",
                           "set_sharedMaterials", "get_sharedMaterials", "BakeMesh",
                           "set_mesh", "get_mesh", "set_skinnedMotionVectors"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const MethodInfo* m = find_method_any(smrCls, names[i]);
        LOGI("smrset: %-24s %p argc=%u", names[i], (void*)m,
             m ? g_api.method_get_param_count(m) : 0);
    }
}

static void run_mesh_test(int cmd) {
    LOGI("===== MESHTEST cmd=%d BEGIN =====", cmd);
    Il2CppClass* MeshCls = find_class_any("UnityEngine", "Mesh");
    Il2CppClass* V3Cls   = find_class_any("UnityEngine", "Vector3");
    Il2CppClass* BWCls   = find_class_any("UnityEngine", "BoneWeight");
    if (!MeshCls || !V3Cls) { LOGE("test: class missing M=%p V3=%p", (void*)MeshCls, (void*)V3Cls); return; }
    int v3stride = array_elem_size(V3Cls, 12);

    if (cmd == 5) { probe_native_skinning(); return; }

    if (cmd == 3) {
        if (!g_bak_mesh || g_bak_nv <= 0 || g_bak_verts.empty()) {
            LOGE("test3: nothing backed up");
            return;
        }
        Il2CppClass* mc3 = g_api.object_get_class(g_bak_mesh);
        if (!mc3) mc3 = MeshCls;
        Il2CppArray* va3 = g_api.array_new(V3Cls, (size_t)g_bak_nv);
        if (!va3) { LOGE("test3: array_new failed"); return; }
        memcpy(array_addr(va3, (size_t)v3stride, 0), g_bak_verts.data(),
               (size_t)g_bak_nv * 12 < g_bak_verts.size() ? (size_t)g_bak_nv * 12 : g_bak_verts.size());
        const MethodInfo* sv3 = find_method_any(mc3, "set_vertices");
        if (sv3) { void* a[1] = { va3 }; invoke_inst(sv3, g_bak_mesh, a); LOGI("test3: vertices restored %d", g_bak_nv); }
        else LOGE("test3: set_vertices missing");
        if (!g_bak_bw.empty() && BWCls) {
            int bwstride = array_elem_size(BWCls, 32);
            Il2CppArray* bwa = g_api.array_new(BWCls, (size_t)g_bak_nv);
            if (bwa) {
                memcpy(array_addr(bwa, (size_t)bwstride, 0), g_bak_bw.data(),
                       g_bak_bw.size() < (size_t)g_bak_nv * bwstride ? g_bak_bw.size() : (size_t)g_bak_nv * bwstride);
                const MethodInfo* sw = find_method_any(mc3, "set_boneWeights");
                if (sw) { void* a[1] = { bwa }; invoke_inst(sw, g_bak_mesh, a); LOGI("test3: boneWeights restored"); }
                else LOGE("test3: set_boneWeights missing");
            }
        }
        const MethodInfo* rb3 = find_method_any(mc3, "RecalculateBounds");
        if (rb3) invoke_inst(rb3, g_bak_mesh, nullptr);
        LOGI("===== MESHTEST cmd=3 END =====");
        return;
    }

    Il2CppObject* smr = test_target_smr();
    if (!smr) { LOGI("===== MESHTEST ABORT ====="); return; }

    // 测试要看得见原角色：把我们的块全拆了，总开关也关掉
    g_custom_on = 0;
    destroy_our_smrs();

    Il2CppClass* mc = g_api.object_get_class(smr);
    if (!mc) { LOGE("test: cannot get SMR class"); return; }
    const MethodInfo* gm = find_method_any(mc, "get_sharedMesh");
    Il2CppObject* mesh = gm ? invoke_inst(gm, smr, nullptr) : nullptr;
    if (!mesh) { LOGE("test: sharedMesh null"); return; }
    Il2CppClass* meshCls = g_api.object_get_class(mesh);
    if (!meshCls) meshCls = MeshCls;

    // 探：这些方法到底在不在
    const MethodInfo* gv = find_method_any(meshCls, "get_vertices");
    LOGI("test: mesh=%p meshCls=%p get_vertices=%p argc=%u", (void*)mesh, (void*)meshCls,
         (void*)gv, gv ? g_api.method_get_param_count(gv) : 0);

    Il2CppArray* va = gv ? (Il2CppArray*)invoke_inst(gv, mesh, nullptr) : nullptr;
    int nv = 0;
    if (va) {
        const unsigned char* h = (const unsigned char*)va;
        char b0[20], b1[20], b2[20], b3[20], b4[20];
        snprintf(b0, sizeof(b0), "%016llx", (unsigned long long)(*(const unsigned long long*)(h +  0)));
        snprintf(b1, sizeof(b1), "%016llx", (unsigned long long)(*(const unsigned long long*)(h +  8)));
        snprintf(b2, sizeof(b2), "%016llx", (unsigned long long)(*(const unsigned long long*)(h + 16)));
        snprintf(b3, sizeof(b3), "%016llx", (unsigned long long)(*(const unsigned long long*)(h + 24)));
        snprintf(b4, sizeof(b4), "%016llx", (unsigned long long)(*(const unsigned long long*)(h + 32)));
        LOGI("test: hdr %s %s %s %s %s", b0, b1, b2, b3, b4);
        unsigned long long ml = va->max_length;
        LOGI("test: get_vertices len=%llu", ml);
        if (va->max_length > 0 && va->max_length < 500000) nv = (int)va->max_length;
    } else {
        LOGI("test: get_vertices returned null");
    }

    if (nv == 0) {
        const MethodInfo* gvc = find_method_any(meshCls, "get_vertexCount");
        Il2CppObject* boxed = gvc ? invoke_inst(gvc, mesh, nullptr) : nullptr;
        nv = unbox_i32(boxed);
        LOGI("test: fallback vertexCount=%d (get_vertexCount=%p)", nv, (void*)gvc);
        va = nullptr;
    }
    if (nv <= 0 || nv > 500000) { LOGE("test: bad nv=%d", nv); return; }

    if (cmd == 4) {
        dump_mesh_meta(mesh, meshCls);
        LOGI("---- smr setters ----");
        dump_smr_setters(mc);
        LOGI("---- try to flip readable ----");
        const MethodInfo* md = find_method_any(meshCls, "MarkDynamic");
        if (md) { invoke_inst(md, mesh, nullptr); LOGI("test4: MarkDynamic ok"); }
        else LOGI("test4: MarkDynamic missing");
        const MethodInfo* up = find_method_any(meshCls, "UploadMeshData");
        if (up) {
            bool noLonger = false;
            void* a[1] = { &noLonger };
            invoke_inst(up, mesh, a);
            LOGI("test4: UploadMeshData(false) ok");
        } else LOGI("test4: UploadMeshData missing");
        LOGI("---- meta after flip ----");
        dump_mesh_meta(mesh, meshCls);
        Il2CppArray* za = g_api.array_new(V3Cls, (size_t)nv);
        if (za) {
            float* zp = (float*)array_addr(za, (size_t)v3stride, 0);
            for (size_t i = 0; i < (size_t)nv * 3; i++) zp[i] = 0.0f;
            const MethodInfo* sv4 = find_method_any(meshCls, "set_vertices");
            if (sv4) { void* a2[1] = { za }; invoke_inst(sv4, mesh, a2); LOGI("test4: set_vertices(zeros) after flip, nv=%d", nv); }
            else LOGE("test4: set_vertices missing");
        }
        LOGI("===== MESHTEST cmd=4 END =====");
        return;
    }

    bool hadOrig = (va != nullptr);
    if (hadOrig && (g_bak_mesh != mesh || g_bak_nv != nv)) {
        g_bak_mesh = mesh;
        g_bak_nv = nv;
        g_bak_verts.assign((size_t)nv * 12, 0);
        memcpy(g_bak_verts.data(), array_addr(va, (size_t)v3stride, 0), (size_t)nv * 12);
        LOGI("test: backed up %d original verts", nv);
    }

    if (cmd == 1) {
        if (!va) {
            va = g_api.array_new(V3Cls, (size_t)nv);
            if (!va) { LOGE("test1: array_new failed"); return; }
        }
        float* pp = (float*)array_addr(va, (size_t)v3stride, 0);
        if (hadOrig) {
            for (size_t i = 0; i < (size_t)nv * 3; i++) pp[i] *= 0.5f;
            LOGI("test1: %d original verts scaled by 0.5", nv);
        } else {
            // 读不到原顶点就只能写死——全填 0，角色应该缩成一个点
            for (size_t i = 0; i < (size_t)nv * 3; i++) pp[i] = 0.0f;
            LOGI("test1: no original data, %d zero verts written (expect collapse)", nv);
        }
        const MethodInfo* sv = find_method_any(meshCls, "set_vertices");
        if (!sv) { LOGE("test1: set_vertices stripped"); return; }
        void* a[1] = { va };
        invoke_inst(sv, mesh, a);
        LOGI("test1: set_vertices called nv=%d mesh=%p", nv, (void*)mesh);
    } else if (cmd == 2) {
        if (!BWCls) { LOGE("test2: no BoneWeight class"); return; }
        BwLayout L = bw_layout(BWCls);
        int bwstride = array_elem_size(BWCls, 32);
        Il2CppArray* bwa = g_api.array_new(BWCls, (size_t)nv);
        if (!bwa) { LOGE("test2: array_new failed"); return; }
        unsigned char* base = (unsigned char*)array_addr(bwa, (size_t)bwstride, 0);
        for (int i = 0; i < nv; i++) {
            unsigned char* e = base + (size_t)i * (size_t)bwstride;
            for (int q = 0; q < 4; q++) {
                *(int*)(e + L.idx[q]) = 0;
                *(float*)(e + L.wgt[q]) = (q == 0) ? 1.0f : 0.0f;
            }
        }
        const MethodInfo* sw = find_method_any(meshCls, "set_boneWeights");
        if (!sw) { LOGE("test2: set_boneWeights stripped"); return; }
        void* a[1] = { bwa };
        invoke_inst(sw, mesh, a);
        LOGI("test2: all %d verts bound to bone 0, weight 1", nv);
    } else {
        LOGE("test: unknown cmd %d", cmd);
    }

    const MethodInfo* rb = find_method_any(meshCls, "RecalculateBounds");
    if (rb) invoke_inst(rb, mesh, nullptr);
    LOGI("===== MESHTEST cmd=%d END =====", cmd);
}

// ===============================================================
// v151: 原生蒙皮最小实验（GPT 大纲 任务 2-4）
//
// 目的只有一个：把 set_sharedMesh / set_bones 这两个闸门钉死。
// 不碰游戏的任何对象，全部自己 new：
//   GameObject + SkinnedMeshRenderer + 两根父子骨骼 + 12 顶点柱子
//   底层 4 顶点绑 A，中间 4 顶点 A/B 各 50%，顶层 4 顶点绑 B
// 每一步都打日志 + 读回验证，卡在哪一步就停在哪一步，
// 不再用"方法查找失败"这种二手结论下判断。
// ===============================================================
static Il2CppObject* g_skinTestGo = nullptr;

static void probe_native_skinning() {
    LOGI("========== [SKIN] NATIVE SKIN PROBE BEGIN ==========");

    Il2CppClass* SMRCls  = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    Il2CppClass* MeshCls = find_class_any("UnityEngine", "Mesh");
    Il2CppClass* GOCls   = find_class_any("UnityEngine", "GameObject");
    Il2CppClass* TrCls   = find_class_any("UnityEngine", "Transform");
    Il2CppClass* V3Cls   = find_class_any("UnityEngine", "Vector3");
    Il2CppClass* V2Cls   = find_class_any("UnityEngine", "Vector2");
    Il2CppClass* I32Cls  = find_class_any("System", "Int32");
    Il2CppClass* M4Cls   = find_class_any("UnityEngine", "Matrix4x4");
    Il2CppClass* BWCls   = find_class_any("UnityEngine", "BoneWeight");
    Il2CppClass* MRCls   = find_class_any("UnityEngine", "MeshRenderer");
    Il2CppClass* MatCls  = find_class_any("UnityEngine", "Material");
    Il2CppClass* ShCls   = find_class_any("UnityEngine", "Shader");
    Il2CppClass* ObjCls  = find_class_any("UnityEngine", "Object");
    if (!SMRCls || !MeshCls || !GOCls || !TrCls || !V3Cls || !I32Cls || !M4Cls || !BWCls) {
        LOGE("[IL2CPP] skintest: class missing SMR=%p Mesh=%p GO=%p Tr=%p V3=%p I32=%p M4=%p BW=%p",
             (void*)SMRCls, (void*)MeshCls, (void*)GOCls, (void*)TrCls,
             (void*)V3Cls, (void*)I32Cls, (void*)M4Cls, (void*)BWCls);
        return;
    }

    // ---- 步骤 0：SMR 类的方法全量清单（这一步回答"到底在不在"）----
    LOGI("[IL2CPP] ---- SkinnedMeshRenderer + parents, ALL methods ----");
    dump_class_methods(SMRCls);
    const char* keys[] = {"set_sharedMesh","get_sharedMesh","set_bones","get_bones",
                          "set_rootBone","get_rootBone","set_bounds","get_bounds",
                          "set_localBounds","BakeMesh","set_updateWhenOffscreen",
                          "set_sharedMaterials","get_sharedMaterials"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const MethodInfo* m = find_method_any(SMRCls, keys[i]);
        LOGI("[IL2CPP] resolve %-24s -> %p argc=%u", keys[i], (void*)m,
             m ? g_api.method_get_param_count(m) : 0);
    }

    // ---- 步骤 1：干净的独立 GameObject ----
    if (g_skinTestGo) {
        const MethodInfo* destroy = ObjCls ? find_method_any(ObjCls, "Destroy") : nullptr;
        if (destroy) { void* a[1] = { g_skinTestGo }; invoke_static(destroy, a); }
        g_skinTestGo = nullptr;
        LOGI("[UNITY] step1 previous test object destroyed");
    }
    const MethodInfo* goCtor = find_method_any(GOCls, ".ctor");
    const MethodInfo* getGOTr = find_method_any(GOCls, "get_transform");
    const MethodInfo* addComp = find_method_any(GOCls, "AddComponent");
    if (!goCtor || !getGOTr || !addComp) { LOGE("[UNITY] step1 GO api missing"); return; }
    void* an[1] = { g_api.string_new("mm_skintest") };
    Il2CppObject* go = g_api.object_new(GOCls);
    invoke_inst(goCtor, go, an);
    if (!go) { LOGE("[UNITY] step1 GameObject new failed"); return; }
    g_skinTestGo = go;
    Il2CppObject* goTr = invoke_inst(getGOTr, go, nullptr);
    LOGI("[UNITY] step1 GameObject=%p transform=%p", (void*)go, (void*)goTr);

    // ---- 步骤 2：两根父子骨骼 A(0,0,0) B(0,1,0) ----
    Il2CppObject* boneA = nullptr;
    Il2CppObject* boneB = nullptr;
    {
        void* n1[1] = { g_api.string_new("mm_boneA") };
        Il2CppObject* g1 = g_api.object_new(GOCls);
        invoke_inst(goCtor, g1, n1);
        boneA = g1 ? invoke_inst(getGOTr, g1, nullptr) : nullptr;
        void* n2[1] = { g_api.string_new("mm_boneB") };
        Il2CppObject* g2 = g_api.object_new(GOCls);
        invoke_inst(goCtor, g2, n2);
        boneB = g2 ? invoke_inst(getGOTr, g2, nullptr) : nullptr;
        const MethodInfo* setParent = find_method_any(TrCls, "SetParent");
        const MethodInfo* setLPos   = find_method_any(TrCls, "set_localPosition");
        if (boneA && boneB && setParent) { void* a[1] = { boneA }; invoke_inst(setParent, boneB, a); }
        if (setLPos) {
            Vec3 p0 = {0.0f, 0.0f, 0.0f};
            if (boneA) { void* a[1] = { &p0 }; invoke_inst(setLPos, boneA, a); }
            Vec3 p1 = {0.0f, 1.0f, 0.0f};
            if (boneB) { void* a[1] = { &p1 }; invoke_inst(setLPos, boneB, a); }
        }
        LOGI("[UNITY] step2 bones A=%p B=%p", (void*)boneA, (void*)boneB);
    }
    if (!boneA || !boneB) { LOGE("[UNITY] step2 bone creation failed"); return; }

    // ---- 步骤 3：12 顶点柱子（3 层 × 4 角），模型空间原点在骨骼 A ----
    const int NV = 12, NI = 48;
    Il2CppObject* mesh = g_api.object_new(MeshCls);
    const MethodInfo* meshCtor = find_method_any(MeshCls, ".ctor");
    if (meshCtor) invoke_inst(meshCtor, mesh, nullptr);
    if (!mesh) { LOGE("[UNITY] step3 Mesh new failed"); return; }
    {
        float v[NV * 3];
        for (int layer = 0; layer < 3; layer++) {
            float y = (float)layer;
            float* d = &v[layer * 12];
            d[0] = -0.2f; d[1] = y; d[2] = -0.2f;
            d[3] =  0.2f; d[4] = y; d[5] = -0.2f;
            d[6] =  0.2f; d[7] = y; d[8] =  0.2f;
            d[9] = -0.2f; d[10]= y; d[11]=  0.2f;
        }
        int id[NI] = {
            0,1,5, 0,5,4,   1,2,6, 1,6,5,   2,3,7, 2,7,6,   3,0,4, 3,4,7,
            4,5,9, 4,9,8,   5,6,10, 5,10,9, 6,7,11, 6,11,10, 7,4,8, 7,8,11
        };
        Il2CppArray* va = g_api.array_new(V3Cls, NV);
        memcpy(array_addr(va, 12, 0), v, sizeof(v));
        Il2CppArray* na = g_api.array_new(V3Cls, NV);
        {
            float* np = (float*)array_addr(na, 12, 0);
            for (int i = 0; i < NV; i++) { np[i*3+0]=0; np[i*3+1]=0; np[i*3+2]=-1.0f; }
        }
        Il2CppArray* ua = g_api.array_new(V2Cls, NV);
        {
            float* up = (float*)array_addr(ua, 8, 0);
            for (int i = 0; i < NV; i++) { up[i*2+0]=0.5f; up[i*2+1]=0.5f; }
        }
        Il2CppArray* ia = g_api.array_new(I32Cls, NI);
        memcpy(array_addr(ia, 4, 0), id, sizeof(id));
        const MethodInfo* sv = find_method_any(MeshCls, "set_vertices");
        const MethodInfo* sn = find_method_any(MeshCls, "set_normals");
        const MethodInfo* su = find_method_any(MeshCls, "set_uv");
        const MethodInfo* st = find_method_any(MeshCls, "set_triangles");
        if (sv) { void* a[1]={va}; invoke_inst(sv, mesh, a); }
        if (sn) { void* a[1]={na}; invoke_inst(sn, mesh, a); }
        if (su) { void* a[1]={ua}; invoke_inst(su, mesh, a); }
        if (st) { void* a[1]={ia}; invoke_inst(st, mesh, a); }
        LOGI("[UNITY] step3 mesh verts=%d idx=%d setV=%p setN=%p setU=%p setT=%p",
             NV, NI, (void*)sv, (void*)sn, (void*)su, (void*)st);
    }

    // ---- 步骤 4：bindposes（A=identity，B=平移 -1）----
    {
        Il2CppArray* bps = g_api.array_new(M4Cls, 2);
        int ms = array_elem_size(M4Cls, 64);
        float* m0 = (float*)array_addr(bps, (size_t)ms, 0);
        float* m1 = (float*)array_addr(bps, (size_t)ms, 1);
        for (int i = 0; i < 16; i++) { m0[i] = 0.0f; m1[i] = 0.0f; }
        m0[0] = m0[5] = m0[10] = m0[15] = 1.0f;      // A 在原点，bind 逆 = 单位阵
        m1[0] = m1[5] = m1[10] = m1[15] = 1.0f;
        m1[13] = -1.0f;                               // B 在 y=1，bind 逆 = T(0,-1,0)
        const MethodInfo* sbp = find_method_any(MeshCls, "set_bindposes");
        if (sbp) { void* a[1] = { bps }; invoke_inst(sbp, mesh, a); }
        LOGI("[UNITY] step4 bindposes set=%p (stride=%d)", (void*)sbp, ms);
    }

    // ---- 步骤 5：BoneWeight（底4绑A / 中4各50% / 顶4绑B）----
    {
        BwLayout L = bw_layout(BWCls);
        int bs = array_elem_size(BWCls, 32);
        Il2CppArray* bwa = g_api.array_new(BWCls, NV);
        unsigned char* base = (unsigned char*)array_addr(bwa, (size_t)bs, 0);
        for (int i = 0; i < NV; i++) {
            unsigned char* e = base + (size_t)i * (size_t)bs;
            int bi[4]; float bw[4];
            if (i < 4)       { bi[0]=0; bw[0]=1.0f; bi[1]=0; bw[1]=0.0f; }
            else if (i < 8)  { bi[0]=0; bw[0]=0.5f; bi[1]=1; bw[1]=0.5f; }
            else             { bi[0]=1; bw[0]=1.0f; bi[1]=1; bw[1]=0.0f; }
            bi[2]=bi[0]; bi[3]=bi[0]; bw[2]=0.0f; bw[3]=0.0f;
            for (int q = 0; q < 4; q++) {
                *(int*)(e + L.idx[q]) = bi[q];
                *(float*)(e + L.wgt[q]) = bw[q];
            }
        }
        const MethodInfo* sbw = find_method_any(MeshCls, "set_boneWeights");
        if (sbw) { void* a[1] = { bwa }; invoke_inst(sbw, mesh, a); }
        LOGI("[UNITY] step5 boneWeights set=%p (stride=%d)", (void*)sbw, bs);
    }

    // ---- 步骤 6：AddComponent<SkinnedMeshRenderer> ----
    Il2CppObject* smr = nullptr;
    {
        void* t[1] = { g_api.type_get_object(g_api.class_get_type(SMRCls)) };
        smr = invoke_inst(addComp, go, t);
        LOGI("[UNITY] step6 AddComponent(SMR) -> %p", (void*)smr);
    }
    if (!smr) { LOGE("[UNITY] step6 SMR creation FAILED - native path dead here"); return; }
    Il2CppClass* smrReal = g_api.object_get_class(smr);
    LOGI("[UNITY] step6 smrReal class=%s", smrReal ? g_api.class_get_name(smrReal) : "?");

    // ---- 步骤 7：set_sharedMesh（闸门 1）----
    {
        const MethodInfo* sm = find_method_any(smrReal ? smrReal : SMRCls, "set_sharedMesh");
        const MethodInfo* gm = find_method_any(smrReal ? smrReal : SMRCls, "get_sharedMesh");
        LOGI("[IL2CPP] step7 set_sharedMesh=%p get_sharedMesh=%p", (void*)sm, (void*)gm);
        if (sm) {
            void* a[1] = { mesh };
            invoke_inst(sm, smr, a);
            Il2CppObject* back = gm ? invoke_inst(gm, smr, nullptr) : nullptr;
            LOGI("[UNITY] step7 readback sharedMesh want=%p got=%p -> %s",
                 (void*)mesh, (void*)back, back == mesh ? "OK" : "MISMATCH");
        } else {
            LOGE("[IL2CPP] step7 GATE FAILED: set_sharedMesh not in metadata");
        }
    }

    // ---- 步骤 8：set_bones（闸门 2）----
    {
        Il2CppArray* bones = g_api.array_new(TrCls, 2);
        if (bones) {
            *(Il2CppObject**)array_addr(bones, sizeof(void*), 0) = boneA;
            *(Il2CppObject**)array_addr(bones, sizeof(void*), 1) = boneB;
        }
        const MethodInfo* sb = find_method_any(smrReal ? smrReal : SMRCls, "set_bones");
        const MethodInfo* gb = find_method_any(smrReal ? smrReal : SMRCls, "get_bones");
        LOGI("[IL2CPP] step8 set_bones=%p get_bones=%p arr=%p", (void*)sb, (void*)gb, (void*)bones);
        if (sb && bones) {
            void* a[1] = { bones };
            invoke_inst(sb, smr, a);
            Il2CppArray* back = gb ? (Il2CppArray*)invoke_inst(gb, smr, nullptr) : nullptr;
            LOGI("[UNITY] step8 readback bones len=%ld (want 2) -> %s",
                 back ? (long)back->max_length : -1L,
                 (back && back->max_length == 2) ? "OK" : "MISMATCH");
        } else {
            LOGE("[IL2CPP] step8 GATE FAILED: set_bones not in metadata");
        }
    }

    // ---- 步骤 9：set_rootBone ----
    {
        const MethodInfo* srb = find_method_any(smrReal ? smrReal : SMRCls, "set_rootBone");
        LOGI("[IL2CPP] step9 set_rootBone=%p", (void*)srb);
        if (srb) { void* a[1] = { boneA }; invoke_inst(srb, smr, a); }
    }

    // ---- 步骤 10：材质 ----
    {
        const MethodInfo* find = ShCls ? find_method(ShCls, "Find", 1) : nullptr;
        const MethodInfo* setMat = find_method_any(MRCls, "set_sharedMaterial");
        if (find && setMat && MatCls) {
            const char* names[] = {"Unlit/Texture", "Mobile/Diffuse", "Sprites/Default"};
            for (int q = 0; q < 3; q++) {
                void* a[1] = { g_api.string_new(names[q]) };
                Il2CppObject* sh = invoke_static(find, a);
                if (!sh) continue;
                Il2CppObject* mat = g_api.object_new(MatCls);
                const MethodInfo* c1 = find_method(MatCls, ".ctor", 1);
                if (c1) { void* b[1] = { sh }; invoke_inst(c1, mat, b); }
                if (mat) {
                    void* b[1] = { mat };
                    invoke_inst(setMat, smr, b);
                    const MethodInfo* sc = find_method(MatCls, "set_color", 1);
                    if (sc) { Color4 w = {1.0f, 0.6f, 0.2f, 1.0f}; void* c[1] = { &w }; invoke_inst(sc, mat, c); }
                    LOGI("[UNITY] step10 material from shader %s", names[q]);
                    break;
                }
            }
        }
    }

    // ---- 步骤 11：放到玩家角色旁边，免得看不见 ----
    if (g_player_root && goTr) {
        const MethodInfo* getPos = find_method_any(TrCls, "get_position");
        const MethodInfo* setPos = find_method_any(TrCls, "set_position");
        if (getPos && setPos) {
            Il2CppObject* v = invoke_inst(getPos, g_player_root, nullptr);
            if (v) {
                const float* f = (const float*)((char*)v + 16);
                Vec3 np = { f[0] + 0.9f, f[1] + 1.0f, f[2] };
                void* a[1] = { &np };
                invoke_inst(setPos, goTr, a);
                LOGI("[UNITY] step11 placed at (%.2f,%.2f,%.2f)", np.x, np.y, np.z);
            }
        }
    }

    // ---- 步骤 12：转骨骼 B，看网格弯不弯 ----
    {
        const MethodInfo* setLR = find_method_any(TrCls, "set_localRotation");
        if (setLR) {
            Quat q = {0.0f, 0.0f, 0.3826834f, 0.9238795f};   // 绕 Z 轴 45°
            void* a[1] = { &q };
            invoke_inst(setLR, boneB, a);
            LOGI("[SKIN] step12 boneB rotated 45deg around Z - watch the pole bend");
        }
    }

    LOGI("========== [SKIN] NATIVE SKIN PROBE END ==========");
}

static Il2CppObject* g_rigidRoot = nullptr;

static int spawn_rigid_parts(Il2CppObject* origSmr) {
    LOGI("rigid: begin");
    {
        static bool everDumped = false;
        if (!everDumped) { everDumped = true; LOGI("rigid: STEP1 dump_everything"); dump_everything(); LOGI("rigid: STEP1 done"); }
    }
    destroy_our_smrs();
    LOGI("rigid: STEP2 cleared old");          // 先清掉上一轮建的块，否则每选一次就叠一层
    Il2CppClass* SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    if (!SMRCls) { LOGE("rigid: no SMR class"); return 0; }
    const MethodInfo* gb = find_method_any(SMRCls, "get_bones");
    if (!gb) { LOGE("rigid: no get_bones"); return 0; }
    Il2CppArray* ba = (Il2CppArray*)invoke_inst(gb, origSmr, nullptr);
    if (!ba || ba->max_length == 0) { LOGE("rigid: no bones"); return 0; }
    int N = (int)ba->max_length;

    LOGI("rigid: STEP3 collect union bones");
    std::vector<Il2CppObject*> bones;
    std::vector<std::string> gnorm;
    collect_role_bones(origSmr, bones, gnorm);
    LOGI("rigid: STEP3 done, union=%d", (int)bones.size());
    if ((int)bones.size() < 22) {
        // 并集都没凑够，退回当前 SMR
        bones.assign((size_t)N, nullptr);
        gnorm.assign((size_t)N, std::string());
        for (int i = 0; i < N; i++) {
            Il2CppObject* b = *(Il2CppObject**)array_addr(ba, 8, (size_t)i);
            bones[i] = b;
            gnorm[i] = b ? norm_bone(obj_name(b), false) : std::string();
        }
        LOGI("rigid: union too small, using single SMR bones=%d", N);
    } else {
        N = (int)bones.size();
        LOGI("rigid: using UNION bones N=%d", N);
    }

    // 找那个目录（两个候选）
    const char* cand[2] = {
        "/data/user/0/com.hermes.h1game/files/",
        "/data/data/com.hermes.h1game/files/",
    };
    char dir[256] = {0};
    for (int i = 0; i < 2 && !dir[0]; i++) {
        char probe[320];
        snprintf(probe, sizeof(probe), "%srg_0_05.mmesh", cand[i]);
        FILE* f = fopen(probe, "rb");
        if (f) { fclose(f); snprintf(dir, sizeof(dir), "%s", cand[i]); }
    }
    if (!dir[0]) { LOGE("rigid: rg_*.mmesh not found"); return 0; }

    // 我们的 22 个骨骼名
    char p0[320];
    snprintf(p0, sizeof(p0), "%scustom_0.mmesh", dir);
    std::vector<std::string> ourNames;
    if (!read_mm2_names(p0, ourNames)) { LOGE("rigid: cannot read custom_0 names"); return 0; }
    std::vector<int> gidx;
    int found = 0;

    // ---- Humanoid 路径已禁用 ----
    // Animator.GetBoneTransform 在非 Humanoid rig 上会崩（SIGSEGV addr 0x78，
    // 现场寄存器里是 "Avatar" 字符串），native 崩溃没法 try/catch，只能不走。
    if (false) {
        std::vector<Il2CppObject*> hb;
        int hbGot = try_humanoid_bones(origSmr, hb);
        if (hbGot >= 15) {
            LOGI("rigid: *** using HUMANOID bone API (%d/22) ***", hbGot);
            bones.assign(hb.begin(), hb.end());
            N = (int)bones.size();           // 22
            gnorm.assign((size_t)N, std::string());
            for (int i = 0; i < N; i++)
                gnorm[i] = bones[i] ? norm_bone(obj_name(bones[i]), false) : std::string();
            gidx.assign((size_t)N, -1);
            for (int i = 0; i < N; i++) if (bones[i]) { gidx[i] = i; found++; }
            LOGI("rigid: humanoid mapped %d/22", found);
            goto mapped_done;
        }
        if (hbGot > 0) LOGI("rigid: humanoid only %d/22, fallback to name matching", hbGot);
    }

    LOGI("rigid: STEP4 name match");
    found = map_bones(ourNames, gnorm, &gidx);
    LOGI("rigid: name-mapped %d/%d bones, target rig has %d", found, (int)ourNames.size(), N);

    // 先按 bonedb 分析出的硬映射补
    apply_hard_map(gnorm, N, gidx);

    // 层级猜测已禁用。
    // 上一版 hier[20] 把 R Forearm 配到了 Bone095，而 Bone095 的 wpos=(0.09,1.15,-7.08)
    // 和 Bip001 R UpperArm 几乎完全重合 —— 那是肩膀上的装饰骨，不是前臂。
    // 猜错一块，那块就整体飞到错误位置，表现为"碎一地"。
    // 宁可少一块，不要错一块。
    {
        found = 0;
        for (int i = 0; i < 22; i++) if (gidx[i] >= 0) found++;
        LOGI("rigid: final mapped %d/22 (hierarchy guessing disabled)", found);
    }
    LOGI("rigid: STEP4 done");
    mapped_done:;
    for (int i = 0; i < (int)ourNames.size(); i++) {
        LOGI("rigid: namemap[%2d] %-20s -> %s", i, ourNames[i].c_str(),
             gidx[i] >= 0 ? gnorm[gidx[i]].c_str() : "NONE");
    }
    // 位置匹配只在名字匹配几乎没命中时才用。
    // 它是"猜"——目标骨骼布局和 PMX 差得远时，猜出来的配对会让整块跑到错误位置，
    // 表现为模型碎成一地。名字匹配虽然少，但每一根都是确定的。
    if (found < 8) {
        LOGI("rigid: name match only %d, trying position fallback (strict)", found);
        int m2 = match_bones_by_position(dir, gnorm, bones, N, gidx);
        if (m2 > found) found = m2;
        LOGI("rigid: after position fallback -> %d/%d", found, (int)ourNames.size());
    } else {
        LOGI("rigid: skipping position matching (name match %d is reliable enough)", found);
    }
    if (found < 5) { LOGE("rigid: too few bones mapped -> abort"); return 0; }

    Il2CppClass* MeshCls = find_class_any("UnityEngine", "Mesh");
    Il2CppClass* V3Cls   = find_class_any("UnityEngine", "Vector3");
    Il2CppClass* V2Cls   = find_class_any("UnityEngine", "Vector2");
    Il2CppClass* I32Cls  = find_class_any("System", "Int32");
    Il2CppClass* ByteCls = find_class_any("System", "Byte");
    Il2CppClass* GOCls   = find_class_any("UnityEngine", "GameObject");
    Il2CppClass* MFCls   = find_class_any("UnityEngine", "MeshFilter");
    Il2CppClass* MRCls   = find_class_any("UnityEngine", "MeshRenderer");
    Il2CppClass* CompCls = find_class_any("UnityEngine", "Component");
    Il2CppClass* TransCls= find_class_any("UnityEngine", "Transform");
    Il2CppClass* MatCls  = find_class_any("UnityEngine", "Material");
    Il2CppClass* ShCls   = find_class_any("UnityEngine", "Shader");
    Il2CppClass* TexCls  = find_class_any("UnityEngine", "Texture2D");
    Il2CppClass* RCls    = find_class_any("UnityEngine", "Renderer");
    if (!MeshCls || !V3Cls || !V2Cls || !I32Cls || !ByteCls || !GOCls || !MFCls ||
        !MRCls || !CompCls || !TransCls || !MatCls || !RCls) {
        LOGE("rigid: class missing"); return 0;
    }

    // 原 SMR 的 shader（自带蒙皮变体，MeshRenderer 也能用）
    Il2CppObject* srcShader = nullptr;
    {
        const MethodInfo* gm = find_method_any(RCls, "get_sharedMaterial");
        const MethodInfo* gs = find_method_any(MatCls, "get_shader");
        if (gm && gs) {
            Il2CppObject* mt = invoke_inst(gm, origSmr, nullptr);
            if (mt) srcShader = invoke_inst(gs, mt, nullptr);
        }
    }

    const MethodInfo* l2w = TransCls ? find_method_any(TransCls, "get_localToWorldMatrix") : nullptr;
    const MethodInfo* setParent = find_method_any(TransCls, "SetParent");
    const MethodInfo* setLPos = find_method_any(TransCls, "set_localPosition");
    const MethodInfo* setLRot = find_method_any(TransCls, "set_localRotation");
    const MethodInfo* setLScl = find_method_any(TransCls, "set_localScale");
    const MethodInfo* goCtor = find_method_any(GOCls, ".ctor");
    const MethodInfo* getGOTr = find_method_any(GOCls, "get_transform");
    const MethodInfo* addComp = find_method_any(GOCls, "AddComponent");
    const MethodInfo* setMesh = find_method_any(MFCls, "set_mesh");
    const MethodInfo* setMat  = find_method_any(RCls, "set_sharedMaterial");
    const MethodInfo* getLayer = find_method_any(GOCls, "get_layer");
    const MethodInfo* setLayer = find_method_any(GOCls, "set_layer");
    if (!setParent || !goCtor || !getGOTr || !addComp || !setMesh || !setMat) {
        LOGE("rigid: method missing"); return 0;
    }

    // 原 SMR 所在 GameObject 的 layer，照抄
    int layer = 0;
    {
        const MethodInfo* getGO = find_method_any(CompCls, "get_gameObject");
        if (getGO) {
            Il2CppObject* sgo = invoke_inst(getGO, origSmr, nullptr);
            if (sgo && getLayer) {
                Il2CppObject* lr = invoke_inst(getLayer, sgo, nullptr);
                if (lr) layer = *(int*)((char*)lr + 16);
            }
        }
    }
    if (layer == 0) layer = 10;
    LOGI("rigid: layer=%d", layer);

    LOGI("rigid: STEP4b bind pose refs");
    std::map<void*, BindRef> bindref;
    build_bindref_map(SMRCls, bindref);

    LOGI("rigid: STEP5 spawning pieces");

    // ---- 自适应缩放 ----
    // 之前写死 kBase = 1.7/20.214 = 0.084，把每块缩小了 10 倍，
    // 结果 27 块各自缩成小碎片挂在各自骨骼位置上 —— 看起来就是"碎一地"。
    // 正确比例要从游戏骨骼自身量：Pelvis→Head 的实际跨度 / PMX 里的跨度。
    float kBase = 1.7f / 20.214f;
    {
        Il2CppClass* TrCls2 = find_class_any("UnityEngine", "Transform");
        const MethodInfo* l2w2 = TrCls2 ? find_method_any(TrCls2, "get_localToWorldMatrix") : nullptr;
        int bpi = -1, bhi = -1;
        for (int i = 0; i < 22; i++) {
            int g = gidx[i];
            if (g < 0 || g >= N) continue;
            if (gnorm[g].find("pelvis") != std::string::npos) bpi = g;
            if (gnorm[g].find("head")   != std::string::npos) bhi = g;
        }
        if (bpi >= 0 && bhi >= 0 && l2w2 && bones[bpi] && bones[bhi]) {
            float py = 0, hy = 0;
            Il2CppObject* m1 = invoke_inst(l2w2, bones[bpi], nullptr);
            if (m1) py = *(float*)((char*)m1 + 16 + 13*4);
            Il2CppObject* m2 = invoke_inst(l2w2, bones[bhi], nullptr);
            if (m2) hy = *(float*)((char*)m2 + 16 + 13*4);
            float gSpan = fabsf(hy - py);
            float pSpan = 16.0363f - 8.0000f;      // PMX Pelvis→Head
            LOGI("rigid: span game=%.3f pmx=%.3f", gSpan, pSpan);
            if (gSpan > 0.05f && gSpan < 500.0f && pSpan > 0.01f) {
                kBase = gSpan / pSpan;
                LOGI("rigid: kBase auto %.5f -> %.5f", 1.7f / 20.214f, kBase);
            } else {
                LOGE("rigid: span out of range, keep fallback %.5f", kBase);
            }
        } else {
            LOGE("rigid: no pelvis/head for k, keep fallback %.5f", kBase);
        }
    }

    int made = 0, skipped = 0;

    for (int part = 0; part < 4; part++) {
        for (int j = 0; j < (int)ourNames.size(); j++) {
            int tgt = gidx[j];
            if (tgt < 0 || tgt >= N || !bones[tgt]) { skipped++; continue; }

            char path[400];
            snprintf(path, sizeof(path), "%srg_%d_%02d.mmesh", dir, part, j);
            FILE* f = fopen(path, "rb");
            if (!f) continue;

            char magic[4];
            uint32_t nv = 0, ni = 0, flags = 0;
            if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "MMSH", 4) != 0) { fclose(f); continue; }
            fread(&nv, 4, 1, f); fread(&ni, 4, 1, f); fread(&flags, 4, 1, f);
            if (nv == 0 || ni == 0 || nv > 200000) { fclose(f); continue; }
            std::vector<float> pos((size_t)nv*3), nrm((size_t)nv*3), uv((size_t)nv*2);
            std::vector<uint32_t> idx(ni);
            if (fread(pos.data(),4,(size_t)nv*3,f) != (size_t)nv*3 ||
                fread(nrm.data(),4,(size_t)nv*3,f) != (size_t)nv*3 ||
                fread(uv.data(), 4,(size_t)nv*2,f) != (size_t)nv*2 ||
                fread(idx.data(),4,ni,f) != ni) { fclose(f); continue; }
            fclose(f);

            // 建 Mesh
            Il2CppObject* mesh = g_api.object_new(MeshCls);
            const MethodInfo* mc = find_method_any(MeshCls, ".ctor");
            if (mc) invoke_inst(mc, mesh, nullptr);

            Il2CppArray* va = g_api.array_new(V3Cls, nv); memcpy(array_addr(va,12,0), pos.data(), (size_t)nv*12);
            Il2CppArray* na = g_api.array_new(V3Cls, nv); memcpy(array_addr(na,12,0), nrm.data(), (size_t)nv*12);
            Il2CppArray* ua = g_api.array_new(V2Cls, nv); memcpy(array_addr(ua, 8,0), uv.data(),  (size_t)nv*8);
            Il2CppArray* ia = g_api.array_new(I32Cls, ni); memcpy(array_addr(ia, 4,0), idx.data(), (size_t)ni*4);
            const MethodInfo* sv = find_method_any(MeshCls, "set_vertices");
            const MethodInfo* sn = find_method_any(MeshCls, "set_normals");
            const MethodInfo* su = find_method_any(MeshCls, "set_uv");
            const MethodInfo* st = find_method_any(MeshCls, "set_triangles");
            if (sv) { void* a[1]={va}; invoke_inst(sv, mesh, a); }
            if (sn) { void* a[1]={na}; invoke_inst(sn, mesh, a); }
            if (su) { void* a[1]={ua}; invoke_inst(su, mesh, a); }
            if (st) { void* a[1]={ia}; invoke_inst(st, mesh, a); }

            // GameObject + 组件
            void* an[1] = { g_api.string_new("mm_rigid") };
            Il2CppObject* go = g_api.object_new(GOCls);
            invoke_inst(goCtor, go, an);
            if (setLayer && go) { void* a[1]={&layer}; invoke_inst(setLayer, go, a); }

            void* t1[1] = { g_api.type_get_object(g_api.class_get_type(MFCls)) };
            Il2CppObject* mf = invoke_inst(addComp, go, t1);
            if (mf) { void* a[1]={mesh}; invoke_inst(setMesh, mf, a); }
            void* t2[1] = { g_api.type_get_object(g_api.class_get_type(MRCls)) };
            Il2CppObject* mr = invoke_inst(addComp, go, t2);

            // 材质：优先用 Unlit/Texture（标准 _MainTex），
            // 游戏自己的 OP/Char/Models_V2_Proxy 可能读 _BaseMap 之类别的属性，
            // 把贴图塞进 _MainTex 它根本不采样 -> 贴图混乱/纯色
            if (mr) {
                Il2CppObject* mat = nullptr;
                const char* usedShader = "none";
                if (ShCls) {
                    const MethodInfo* sf = find_method(ShCls, "Find", 1);
                    if (sf) {
                        const char* sn[] = {"Mobile/Diffuse", "Unlit/Texture", "Sprites/Default"};
                        for (int q = 0; q < 3 && !mat; q++) {
                            void* a[1] = { g_api.string_new(sn[q]) };
                            Il2CppObject* sh = invoke_static(sf, a);
                            if (!sh) continue;
                            mat = g_api.object_new(MatCls);
                            const MethodInfo* c1 = find_method(MatCls, ".ctor", 1);
                            if (c1) { void* b[1]={sh}; invoke_inst(c1, mat, b); }
                            if (mat) { usedShader = sn[q]; LOGI("rigid: shader pick[%d]=%s ok", q, sn[q]); }
                            else LOGI("rigid: shader pick[%d]=%s null", q, sn[q]);
                        }
                    }
                }
                if (!mat && srcShader) {
                    mat = g_api.object_new(MatCls);
                    const MethodInfo* c1 = find_method(MatCls, ".ctor", 1);
                    if (c1) { void* b[1]={srcShader}; invoke_inst(c1, mat, b); }
                    if (mat) usedShader = "orig";
                }
                if (mat) LOGI("rigid: mat shader=%s part=%d", usedShader, part);
                if (mat) {
                    void* a[1]={mat}; invoke_inst(setMat, mr, a);
                    const MethodInfo* sc = find_method(MatCls, "set_color", 1);
                    if (sc) { Color4 w={1,1,1,1}; void* b[1]={&w}; invoke_inst(sc, mat, b); }
                    // 贴图
                    char tp[400];
                    snprintf(tp, sizeof(tp), "%scustom_%d.mmtex", dir, part);
                    FILE* tf = fopen(tp, "rb");
                    if (tf) {
                        char tm[4]; uint32_t tw=0, th=0;
                        if (fread(tm,1,4,tf)==4 && memcmp(tm,"MMTX",4)==0 &&
                            fread(&tw,4,1,tf)==1 && fread(&th,4,1,tf)==1 &&
                            tw>0 && th>0 && tw<=4096 && th<=4096) {
                            size_t nbytes=(size_t)tw*th*4;
                            std::vector<unsigned char> px(nbytes);
                            if (fread(px.data(),1,nbytes,tf)==nbytes && TexCls && ByteCls) {
                                const MethodInfo* tc = find_method(TexCls, ".ctor", 4);
                                Il2CppObject* tex = g_api.object_new(TexCls);
                                if (tc) { int wi=(int)tw, hi=(int)th, fmt=4; bool mip=false;
                                          void* ta[4]={&wi,&hi,&fmt,&mip}; invoke_inst(tc, tex, ta); }
                                const MethodInfo* lr = find_method(TexCls, "LoadRawTextureData", 1);
                                const MethodInfo* ap = find_method(TexCls, "Apply", 1);
                                if (tex && lr) {
                                    Il2CppArray* bar = g_api.array_new(ByteCls, nbytes);
                                    memcpy(array_addr(bar,1,0), px.data(), nbytes);
                                    void* la[1]={bar}; invoke_inst(lr, tex, la);
                                    if (ap) { bool u=false; void* aa[1]={&u}; invoke_inst(ap, tex, aa); }
                                    const MethodInfo* sm2 = find_method(MatCls, "set_mainTexture", 1);
                                    if (sm2) {
                                        void* sa[1]={tex}; invoke_inst(sm2, mat, sa);
                                        const MethodInfo* gm2 = find_method_any(MatCls, "get_mainTexture");
                                        Il2CppObject* back = gm2 ? invoke_inst(gm2, mat, nullptr) : nullptr;
                                        LOGI("rigid: tex part=%d %ux%u readback=%s", part, tw, th,
                                             back == tex ? "OK" : "MISMATCH");
                                    }
                                }
                            }
                        }
                        fclose(tf);
                    }
                }
            }

            // 挂到骨骼下
            Il2CppObject* gt = getGOTr ? invoke_inst(getGOTr, go, nullptr) : nullptr;
            if (!gt) continue;
            void* pa[1] = { bones[tgt] };
            invoke_inst(setParent, gt, pa);

            // ---------------------------------------------------------------
            // v141：三元组一次设死，全部拿 bindposes 推出来的 bind 姿态
            //
            // 顶点早已减过骨骼 bind 位置（v - p），所以块原点放在骨骼原点：
            //   世界 = M_bone * T(0) * R_b * S_b * (v - p)
            // bind 时 M_bone = T(p) * R_bind * S(s)，要它退化成 p + (v-p)*kBase 就得：
            //   R_b = R_bind^T   （bindpose 3x3 列归一化后的矩阵）
            //   S_b = kBase / s  （bindpose 列长 l = 1/s → kBase * l）
            // 一次设死之后，骨骼动画的增量旋转照常带着块转，这才是对的。
            // v139 用实时 worldRotation 是错的：角色不在 bind pose 时每块就带着
            // bind→现在的增量旋转，表现就是“位置对、方向歪”。
            // ---------------------------------------------------------------
            BindRef br;
            br.ok = false; br.sc = 1.0f;
            br.rot.x = 0; br.rot.y = 0; br.rot.z = 0; br.rot.w = 1;
            {
                std::map<void*, BindRef>::iterator it = bindref.find((void*)bones[tgt]);
                if (it != bindref.end() && it->second.ok) br = it->second;
            }
            float ls;
            if (br.ok) {
                ls = kBase * br.sc;
            } else {
                // bindposes 里没这根骨骼（union 里来自别的 SMR），退回实时世界变换
                float ws = 1.0f;
                if (l2w) {
                    Il2CppObject* m2 = invoke_inst(l2w, bones[tgt], nullptr);
                    if (m2) {
                        const float* mm = (const float*)((char*)m2 + 16);
                        float sx = sqrtf(mm[0]*mm[0] + mm[1]*mm[1] + mm[2]*mm[2]);
                        if (sx > 0.0001f) ws = sx;
                    }
                }
                ls = kBase / ws;
                const MethodInfo* getRot = find_method_any(TransCls, "get_rotation");
                if (getRot) {
                    Il2CppObject* rq = invoke_inst(getRot, bones[tgt], nullptr);
                    if (rq) {
                        br.rot.x = -*(float*)((char*)rq + 16);
                        br.rot.y = -*(float*)((char*)rq + 20);
                        br.rot.z = -*(float*)((char*)rq + 24);
                        br.rot.w =  *(float*)((char*)rq + 28);
                    }
                }
            }
            if (made < 30) {
                LOGI("rigid: piece p%d/s%d -> bone[%d](%s) bind=%s ls=%.5f",
                     part, j, tgt, gnorm[tgt].c_str(), br.ok ? "YES" : "no", ls);
            }
            if (setLScl) { Vec3 v = {ls, ls, ls}; void* a[1]={&v}; invoke_inst(setLScl, gt, a); }
            if (setLRot) { void* a[1] = { &br.rot }; invoke_inst(setLRot, gt, a); }
            if (setLPos) { Vec3 v = {0,0,0}; void* a[1]={&v}; invoke_inst(setLPos, gt, a); }

            g_ourSmrs.insert(go);
            made++;
        }
    }
    LOGI("rigid: spawned %d pieces (skipped %d unmapped bones)", made, skipped);
    if (made > 0) g_gpu_skin = 1;
    return made;
}

static int spawn_gpu_parts(Il2CppObject* origSmr) {
    if (g_gpu_tried) return g_gpu_skin ? 1 : 0;
    g_gpu_tried = 1;

    const char* b1[] = {
        "/data/user/0/com.hermes.h1game/files/custom_%d.mmesh",
        "/data/data/com.hermes.h1game/files/custom_%d.mmesh",
    };
    const char* b2[] = {
        "/data/user/0/com.hermes.h1game/files/custom_%d.mmtex",
        "/data/data/com.hermes.h1game/files/custom_%d.mmtex",
    };
    char p0[256] = {0}, t0[256] = {0};
    for (int b = 0; b < 2; b++) {
        snprintf(p0, sizeof(p0), b1[b], 0);
        FILE* tf = fopen(p0, "rb");
        if (!tf) continue;
        fclose(tf);
        snprintf(t0, sizeof(t0), b2[b], 0);
        break;
    }
    if (!p0[0]) { LOGE("gpu: custom_0.mmesh not found"); return 0; }

    std::vector<std::string> names;
    if (!read_mm2_names(p0, names)) { LOGE("gpu: cannot read bone names"); return 0; }
    LOGI("gpu: our part has %d bone names", (int)names.size());

    Il2CppClass* SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    if (!SMRCls) { LOGE("gpu: no SMR class"); return 0; }

    Il2CppObject* roleRoot = find_role_root(origSmr);
    LOGI("gpu: player roleRoot=%p", roleRoot);

    std::vector<Il2CppObject*> bones;
    std::vector<std::string> gameNorm;
    Il2CppObject* target = nullptr;

    // 用户手动选过就用它
    if (g_tgt_sel >= 0) {
        Il2CppObject* forced = nullptr;
        pthread_mutex_lock(&g_tgt_mtx);
        if (g_tgt_sel < (int)g_tgt_smrs.size()) forced = g_tgt_smrs[g_tgt_sel];
        pthread_mutex_unlock(&g_tgt_mtx);
        if (forced && *(void**)((char*)forced + 16)) {
            const MethodInfo* gb2 = find_method_any(SMRCls, "get_bones");
            Il2CppArray* a2 = gb2 ? (Il2CppArray*)invoke_inst(gb2, forced, nullptr) : nullptr;
            if (a2 && a2->max_length > 0) {
                for (size_t j = 0; j < a2->max_length; j++) {
                    Il2CppObject* t = *(Il2CppObject**)array_addr(a2, 8, j);
                    bones.push_back(t);
                    gameNorm.push_back(t ? norm_bone(obj_name(t), false) : "");
                }
                target = forced;
                LOGI("gpu: USER-PICKED target idx=%d bones=%d", (int)g_tgt_sel, (int)a2->max_length);
            }
        }
        if (!target) LOGE("gpu: picked target %d is gone, auto-picking", (int)g_tgt_sel);
    }
    if (!target) target = pick_best_smr(SMRCls, names, roleRoot, &bones, &gameNorm);
    if (!target) { LOGE("gpu: no SMR matches our bones"); return 0; }
    int N = (int)bones.size();
    LOGI("gpu: target smr=%p bones=%d", target, N);

    float k = 1.7f / 20.214f;
    int ok = replace_original_smr(nullptr, target, p0, t0, N, gameNorm, k);
    if (ok) { g_gpu_skin = 1; LOGI("gpu: GPU skinning ACTIVE"); }
    else     { LOGE("gpu: falling back to static path"); }
    return ok;
}

static int spawn_parts_loop(Il2CppObject* smr) {
    g_skinParts.clear();   // 重建时先清掉旧的蒙皮数据
    Il2CppObject* roleRoot = find_role_root(smr);
    if (!roleRoot) { LOGE("parts: no roleRoot"); return 0; }

    // 总开关关掉就直接不建
    if (!g_custom_on) {
        destroy_our_smrs();
        LOGI("parts: custom model OFF, nothing spawned");
        return 0;
    }

    LOGI("parts: render_mode=%d", (int)g_render_mode);

    // 刚性分段只在模式 1 用
    if (g_render_mode == 1 && !g_gpu_skin) {
        int r = spawn_rigid_parts(smr);
        LOGI("parts: rigid=%d -> %s", r, r > 0 ? "RIGID MODE" : "fallback static");
        if (r > 0) return r;
    } else if (g_gpu_skin) {
        LOGI("parts: already in rigid/gpu mode, skip");
        return 1;
    }

    // 静态 / CPU 蒙皮 都要 bind 逆矩阵
    if (!g_bindInv_built) build_bindinv_map();

    // 用 1.7 米 / 20.214 单位（南宫羽 bbox 高度）
    float scaleK = 1.7f / (20.214f * 1.0f);

    static Il2CppObject* parts[8] = {nullptr};
    int made = 0;
    const char* bases[] = {
        "/data/user/0/com.hermes.h1game/files/custom_%d.mmesh",
        "/data/data/com.hermes.h1game/files/custom_%d.mmesh",
    };
    const char* texbases[] = {
        "/data/user/0/com.hermes.h1game/files/custom_%d.mmtex",
        "/data/data/com.hermes.h1game/files/custom_%d.mmtex",
    };
    for (int i = 0; i < 4; i++) {
        char mp[256], tp[256];
        bool ok = false;
        for (int b = 0; b < 2 && !ok; b++) {
            snprintf(mp, sizeof(mp), bases[b], i);
            FILE* tf = fopen(mp, "rb");
            if (!tf) continue;
            fclose(tf);
            snprintf(tp, sizeof(tp), texbases[b], i);
            ok = true;
        }
        if (!ok) { LOGI("parts: custom_%d.mmesh missing", i); continue; }
        Il2CppObject* g = spawn_one_part(smr, roleRoot, mp, tp, scaleK);
        if (g) { parts[made++] = g; }
    }
    LOGI("parts: spawned %d", made);
    return made;
}

// 实测 0x1d8 就是 native SkinnedMeshRenderer 里的 mesh 指针，直接改它换模型
#define NATIVE_MESH_OFF 0x1d8

static void test_native_swap() {
    Il2CppClass* SMRCls = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    if (!SMRCls) return;
    const MethodInfo* getMesh = find_method(SMRCls, "get_sharedMesh", 0);
    if (!getMesh) return;

    std::vector<Fingerprint> list;
    collect_fingerprints(SMRCls, list);

    Il2CppObject* smrA = nullptr; Il2CppObject* meshA = nullptr;
    Il2CppObject* smrB = nullptr; Il2CppObject* meshB = nullptr;
    for (auto& f : list) {
        if (f.chain.find("Body") == std::string::npos) continue;
        Il2CppObject* m = invoke_inst(getMesh, f.smr, nullptr);
        if (!m) continue;
        if (!smrA) { smrA = f.smr; meshA = m; continue; }
        if (m == meshA) continue;
        smrB = f.smr; meshB = m; break;
    }
    if (!smrA || !smrB) { LOGI("swap: need 2 distinct meshes (A=%p B=%p)", smrA, smrB); return; }

    void* natA = *(void**)((char*)smrA + 16);
    void* nMeshB = *(void**)((char*)meshB + 16);
    if (!natA || !nMeshB) { LOGE("swap: null nat ptr"); return; }

    void* old = *(void**)((char*)natA + NATIVE_MESH_OFF);
    *(void**)((char*)natA + NATIVE_MESH_OFF) = nMeshB;
    LOGI("swap: smrA=%p nat=%p off=0x%x old=%p new=%p", smrA, natA, NATIVE_MESH_OFF, old, nMeshB);
    LOGI("swap: meshA=%p meshB=%p", meshA, meshB);

    void* back = *(void**)((char*)natA + NATIVE_MESH_OFF);
    LOGI("swap: readback=%p %s", back, back == nMeshB ? "OK" : "MISMATCH");
    Il2CppObject* after = invoke_inst(getMesh, smrA, nullptr);
    LOGI("swap: get_sharedMesh after=%p (want %p)", after, meshB);
}

// 从 SMR 的 transform 往上爬，找 _U_/_H_/_C_ 这种角色实例节点
static Il2CppObject* find_role_root(Il2CppObject* smr) {
    if (!smr) return nullptr;
    Il2CppClass* CompC = find_class_any("UnityEngine", "Component");
    Il2CppClass* TransC = find_class_any("UnityEngine", "Transform");
    if (!CompC || !TransC) return nullptr;
    const MethodInfo* getTransC = find_method(CompC, "get_transform", 0);
    const MethodInfo* getParent = find_method(TransC, "get_parent", 0);
    if (!getTransC || !getParent) return nullptr;
    Il2CppObject* cur = invoke_inst(getTransC, smr, nullptr);
    for (int i = 0; i < 8 && cur; i++) {
        std::string cn = obj_name(cur);
        if (cn.find("_U_") != std::string::npos ||
            cn.find("_H_") != std::string::npos ||
            cn.find("_C_") != std::string::npos) {
            return cur;
        }
        cur = invoke_inst(getParent, cur, nullptr);
    }
    return nullptr;
}

// 这个 SMR 是否在玩家主控角色下面
static bool under_player_root(Il2CppObject* smr) {
    if (!g_only_main) return true;
    if (!g_player_root) return true;     // 还没锁定主控，先不管
    Il2CppClass* CompC = find_class_any("UnityEngine", "Component");
    Il2CppClass* TransC = find_class_any("UnityEngine", "Transform");
    if (!CompC || !TransC) return true;
    const MethodInfo* getTransC = find_method(CompC, "get_transform", 0);
    const MethodInfo* getParent = find_method(TransC, "get_parent", 0);
    if (!getTransC || !getParent) return true;
    Il2CppObject* cur = invoke_inst(getTransC, smr, nullptr);
    for (int i = 0; i < 10 && cur; i++) {
        if (cur == g_player_root) return true;
        cur = invoke_inst(getParent, cur, nullptr);
    }
    return false;
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
    find_updates();

    // DOTween.Update：主线程每帧必调，拿到它就有主线程上下文
    {
        Il2CppClass* TC = find_class_any("DG.Tweening.Core", "DOTweenComponent");
        if (TC) {
            const MethodInfo* up = find_method(TC, "Update", 0);
            LOGI("DOTween.Update=%p", (void*)up);
            if (up) {
                void* sh = load_so_by_name("libshadowhook.so");
                if (sh) {
                    typedef int (*fn_init)(int, bool);
                    fn_init init = (fn_init)dlsym(sh, "shadowhook_init");
                    if (init) init(0, false);
                    void* o = try_hook(sh, *(void**)up, (void*)hk_tween_update, "DOTween.Update");
                    if (o) { g_orig_tween = o; LOGI("tween hook ok"); }
                }
            }
        } else {
            LOGE("no DOTweenComponent");
        }
    }

    find_module_ranges();

    // libc clock_gettime：实测 UnityMain 每秒 5000+ 次，这是真正的主线程 tick
    // 不走 shadowhook（它没写入代码），用自己的 patch
    {
        void* cgt = dlsym(RTLD_DEFAULT, "clock_gettime");
        LOGI("clock_gettime addr=%p", cgt);
        if (cgt) {
            if (self_hook_code(cgt, (void*)hk_cgt)) LOGI("cgt self-hook ok");
            else LOGE("cgt self-hook FAILED");
        }
    }

    // 其他 MonoBehaviour.Update 候选：越多入口越好，总有一个高频
    {
        void* sh3 = load_so_by_name("libshadowhook.so");
        struct Cand { const char* ns; const char* cls; void* fn; void** orig; const char* tag; };
        Cand cands[] = {
            { "UnityEngine.EventSystems", "EventSystem",       (void*)hk_event_update,  &g_orig_event,   "EventSystem.Update" },
            { "UnityEngine.UI",           "CanvasScaler",      (void*)hk_cscaler_update,&g_orig_cscaler, "CanvasScaler.Update" },
            { "UnityEngine.UI",           "AspectRatioFitter", (void*)hk_aspect_update, &g_orig_aspect,  "AspectRatioFitter.Update" },
            { "UnityEngine.Experimental.Rendering", "Volume",  (void*)hk_volume_update, &g_orig_volume,  "Volume.Update" },
        };
        if (!sh3) LOGE("cand: no shadowhook");
        for (auto& c : cands) {
            Il2CppClass* K = find_class_any(c.ns, c.cls);
            if (!K) { LOGE("cand: no class %s", c.cls); continue; }
            const MethodInfo* up = find_method(K, "Update", 0);
            if (!up) { LOGE("cand: no Update in %s", c.cls); continue; }
            if (!sh3) break;
            void* o = try_hook(sh3, *(void**)up, c.fn, c.tag);
            if (o) *c.orig = o;
        }
    }

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

    // 看门狗：盯住所有 hook 点，被改回去就重装
    {
        pthread_t wt;
        if (pthread_create(&wt, nullptr, hook_watchdog, nullptr) == 0) {
            pthread_detach(wt);
            LOGI("watchdog started");
        }
    }

    LOGI("worker loop start (hide + instantiate-hook)");

    // 一次性 dump：字段偏移 + 方法名
    dump_class_members("UnityEngine", "SkinnedMeshRenderer");
    dump_class_members("UnityEngine", "Renderer");
    dump_class_members("UnityEngine", "Mesh");

    int round = 0;
    while (true) {
        round++;
        std::vector<Fingerprint> list;
        collect_fingerprints(SMRCls, list);
        LOGI("scan #%d: %zu smrs", round, list.size());
        {
            static int probed = 0;
            if (!probed && list.size() >= 10) { probed = 1; probe_native_mesh(); }
        }
        // swap 实测会让游戏崩（0x1d8 不是安全的 mesh 入口），先关掉
        // {
        //     static int swapped = 0;
        //     if (!swapped && round >= 12 && list.size() >= 10) { swapped = 1; test_native_swap(); }
        // }

        int done = 0;
        for (auto& f : list) {
            // 全路径采样，用于分析特效/披风的真实命名
            if (g_path_dump.size() < 600 && !g_path_dump.count(f.chain)) {
                g_path_dump.insert(f.chain);
                LOGI("PATH %s", f.chain.c_str());
            }

            if (done >= 4) break;
            if (g_patched_meshes.count(f.smr)) continue;

            for (int r = 0; r < RULE_COUNT; r++) {
                if (!rule_matches(g_rules[r], f)) continue;

                // 只动玩家主控角色，别把对面也隐藏了
                if (!under_player_root(f.smr)) continue;

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
        sleep(5);
    }
    return nullptr;
}

// ================= 入口 =================
// Java 层把游戏的 files 目录传进来，保证日志一定写得出去
extern "C" JNIEXPORT void JNICALL
Java_com_moss_modelmod_HookEntry_setReplaceMode(JNIEnv*, jclass, jint on) {
    g_replace_mode = (int)on;
    g_cube_done = 0;
    g_force_tick = 1;
    g_hide_main_pending = 1;   // 不等 tick，立即把主控角色开/关
    LOGI("replace mode = %d", (int)on);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_moss_modelmod_HookEntry_getLayerInfo(JNIEnv* env, jclass) {
    update_layer_info();
    return env->NewStringUTF(g_layer_info);
}

extern "C" JNIEXPORT void JNICALL
Java_com_moss_modelmod_HookEntry_reloadModel(JNIEnv*, jclass) {
    g_cube_done = 0;
    g_force_tick = 1;          // 不跳 3 秒节流，下一 tick 立刻重建
    destroy_our_smrs();        // v114: 干掉旧的 GPU 部件
    g_gpu_tried = 0; g_gpu_skin = 0;
    LOGI("reload requested: force rebuild");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_moss_modelmod_HookEntry_getTargets(JNIEnv* env, jclass) {
    pthread_mutex_lock(&g_tgt_mtx);
    std::string out = g_tgt_list;
    pthread_mutex_unlock(&g_tgt_mtx);
    return env->NewStringUTF(out.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_moss_modelmod_HookEntry_selectTarget(JNIEnv*, jclass, jint idx) {
    destroy_our_smrs();          // 换目标前把旧的清干净
    g_tgt_sel = (int)idx;
    g_gpu_tried = 0;
    g_cube_done = 0;
    g_force_tick = 1;
    LOGI("target selected = %d, rebuild queued", (int)idx);
}

extern "C" JNIEXPORT void JNICALL
Java_com_moss_modelmod_HookEntry_setCustomEnabled(JNIEnv*, jclass, jint on) {
    g_custom_on = (int)on;
    destroy_our_smrs();
    g_gpu_tried = 0;
    g_cube_done = 0;
    g_force_tick = 1;
    LOGI("custom model = %s", on ? "ON" : "OFF");
}

extern "C" JNIEXPORT jint JNICALL
Java_com_moss_modelmod_HookEntry_getCustomEnabled(JNIEnv*, jclass) {
    return (jint)g_custom_on;
}

// v144: 渲染模式 0=静态 1=刚性 2=CPU蒙皮
extern "C" JNIEXPORT void JNICALL
Java_com_moss_modelmod_HookEntry_setRenderMode(JNIEnv*, jclass, jint m) {
    g_render_mode = (int)m;
    g_bindInv_built = 0;
    g_skinParts.clear();
    destroy_our_smrs();
    g_gpu_tried = 0; g_gpu_skin = 0;
    g_cube_done = 0;
    g_force_tick = 1;
    LOGI("render mode = %d", (int)m);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_moss_modelmod_HookEntry_getRenderMode(JNIEnv*, jclass) {
    return (jint)g_render_mode;
}

// v141: 原生蒙皮验证开关
// 1 = 原 mesh 顶点 x0.5   2 = 全部顶点绑 0 号骨   3 = 还原
extern "C" JNIEXPORT void JNICALL
Java_com_moss_modelmod_HookEntry_testMesh(JNIEnv*, jclass, jint cmd) {
    g_test_cmd = (int)cmd;
    g_force_tick = 1;
    LOGI("test cmd queued: %d", (int)cmd);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_moss_modelmod_HookEntry_selectedTarget(JNIEnv*, jclass) {
    return (jint)g_tgt_sel;
}

extern "C" JNIEXPORT void JNICALL
Java_com_moss_modelmod_HookEntry_setHideMask(JNIEnv*, jclass, jint mask) {
    g_hide_mask = (int)mask;
    LOGI("hide mask = 0x%x", (unsigned)mask);
}

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
