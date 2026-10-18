#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <vector>
#include <map>
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
#define MOD_MODE 0
#endif

// ================= 角色映射表 =================
// 填法：日志里看到 [SMR 0] path=HeroRoot/hero_11001/body bone0=Bip001 mat0=11001_body
//      key 填 "11001"，bundle 填对应文件路径，asset 填 bundle 内的资源名
struct CharRule {
    const char* key;
    const char* bundle;
    const char* asset;
};

static CharRule g_rules[] = {
    // { "11001", "/data/local/tmp/modelmod/11001.ab", "hero_11001" },
};
static const int RULE_COUNT = (int)(sizeof(g_rules) / sizeof(g_rules[0]));

Il2CppApi g_api{};
static JavaVM* g_vm = nullptr;
static Il2CppDomain* g_domain = nullptr;
static std::string g_log_path;
static FILE* g_log_fp = nullptr;
static std::map<std::string, Il2CppObject*> g_mesh_cache;

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

static const MethodInfo* find_method(Il2CppClass* klass, const char* name, int argc) {
    if (!klass) return nullptr;
    void* iter = nullptr;
    const MethodInfo* m;
    while ((m = g_api.class_get_methods(klass, &iter))) {
        if (strcmp(g_api.method_get_name(m), name) != 0) continue;
        if ((int)g_api.method_get_param_count(m) != argc) continue;
        return m;
    }
    return nullptr;
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
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, soname)) continue;
        uintptr_t start = strtoull(line, nullptr, 16);
        if (!base || start < base) base = start;
    }
    fclose(f);
    return base;
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
    B(class_from_name,           "il2cpp_class_from_name")
    B(class_get_name,            "il2cpp_class_get_name")
    B(class_get_parent,          "il2cpp_class_get_parent")
    B(class_is_assignable_from,  "il2cpp_class_is_assignable_from")
    B(class_get_type,            "il2cpp_class_get_type")
    B(class_get_methods,         "il2cpp_class_get_methods")
    B(method_get_name,           "il2cpp_method_get_name")
    B(method_get_param_count,    "il2cpp_method_get_param_count")
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
    auto it = g_mesh_cache.find(r.key);
    if (it != g_mesh_cache.end()) return it->second;

    Il2CppObject* m = load_mesh_from_bundle(r.bundle, r.asset);
    if (m) {
        g_mesh_cache[r.key] = m;
        LOGI("rule ready: %s -> %p", r.key, m);
    }
    return m;
}

static int patch_scene_targeted(Il2CppClass* SMRCls, const MethodInfo* setMesh, int budget) {
    std::vector<Fingerprint> list;
    collect_fingerprints(SMRCls, list);

    int done = 0;
    for (size_t i = 0; i < list.size() && done < budget; i++) {
        Fingerprint& f = list[i];
        for (int r = 0; r < RULE_COUNT; r++) {
            CharRule& rule = g_rules[r];
            if (!contains(f.chain, rule.key) &&
                !contains(f.bone0, rule.key) &&
                !contains(f.mat0,  rule.key)) continue;

            Il2CppObject* want = mesh_for_rule(rule);
            if (!want) break;

            const MethodInfo* getMesh = find_method(SMRCls, "get_sharedMesh", 0);
            Il2CppObject* cur = invoke_inst(getMesh, f.smr, nullptr);
            if (cur == want) break;

            void* a[1] = { want };
            invoke_inst(setMesh, f.smr, a);
            LOGI("swapped %s -> key=%s", f.chain.c_str(), rule.key);
            done++;
            break;
        }
    }
    return done;
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

    if (MOD_MODE == 0) {
        LOGI("mode 0: fingerprint dump, every 10s");
        while (true) {
            dump_fingerprints(SMRCls);
            sleep(10);
        }
    }

    LOGI("mode 1: targeted swap");
    const MethodInfo* setMesh = find_method(SMRCls, "set_sharedMesh", 1);
    if (!setMesh) { LOGE("set_sharedMesh not found"); return nullptr; }

    while (true) {
        int n = patch_scene_targeted(SMRCls, setMesh, 8);
        if (n > 0) LOGI("round swapped %d", n);
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
