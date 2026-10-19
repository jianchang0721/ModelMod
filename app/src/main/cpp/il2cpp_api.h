#pragma once
#include <cstdint>
#include <cstddef>

typedef struct Il2CppClass          Il2CppClass;
typedef struct Il2CppObject         Il2CppObject;
typedef struct Il2CppImage          Il2CppImage;
typedef struct Il2CppAssembly       Il2CppAssembly;
typedef struct Il2CppDomain         Il2CppDomain;
typedef struct Il2CppException      Il2CppException;
typedef struct Il2CppString         Il2CppString;
typedef struct Il2CppType           Il2CppType;
typedef struct Il2CppReflectionType Il2CppReflectionType;
typedef struct MethodInfo           MethodInfo;
typedef struct FieldInfo            FieldInfo;

struct Il2CppObject {
    void* klass;
    void* monitor;
};

// 64 位：klass(8) + monitor(8) + bounds(8) + max_length(4) + pad(4) = 32
struct Il2CppArray {
    void*  klass;
    void*  monitor;
    void*  bounds;
    size_t max_length;
};

static inline void* array_addr(Il2CppArray* a, size_t elemSize, size_t idx) {
    return (char*)a + 32 + elemSize * idx;
}

typedef Il2CppDomain*         (*fn_domain_get)();
typedef const Il2CppAssembly**(*fn_domain_get_assemblies)(const Il2CppDomain*, size_t*);
typedef Il2CppDomain*         (*fn_thread_attach)(Il2CppDomain*);
typedef Il2CppImage*          (*fn_assembly_get_image)(const Il2CppAssembly*);
typedef const char*           (*fn_image_get_name)(Il2CppImage*);
typedef size_t                (*fn_image_get_class_count)(const Il2CppImage*);
typedef Il2CppClass*          (*fn_image_get_class)(const Il2CppImage*, size_t);
typedef Il2CppClass*          (*fn_class_from_name)(const Il2CppImage*, const char*, const char*);
typedef const char*           (*fn_class_get_name)(Il2CppClass*);
typedef const char*           (*fn_class_get_namespace)(Il2CppClass*);
typedef Il2CppClass*          (*fn_class_get_parent)(Il2CppClass*);
typedef bool                  (*fn_class_is_assignable_from)(Il2CppClass*, Il2CppClass*);
typedef const Il2CppType*     (*fn_class_get_type)(Il2CppClass*);
typedef const MethodInfo*     (*fn_class_get_methods)(Il2CppClass*, void**);
typedef const char*           (*fn_method_get_name)(const MethodInfo*);
typedef uint32_t              (*fn_method_get_param_count)(const MethodInfo*);
typedef const Il2CppType*     (*fn_method_get_param)(const MethodInfo*, uint32_t);
typedef Il2CppClass*          (*fn_class_from_type)(const Il2CppType*);
typedef Il2CppObject*         (*fn_runtime_invoke)(const MethodInfo*, void*, void**, Il2CppException**);
typedef Il2CppString*         (*fn_string_new)(const char*);
typedef Il2CppObject*         (*fn_object_new)(Il2CppClass*);
typedef Il2CppClass*          (*fn_object_get_class)(Il2CppObject*);
typedef Il2CppArray*          (*fn_array_new)(Il2CppClass*, size_t);
typedef Il2CppReflectionType* (*fn_type_get_object)(const Il2CppType*);
typedef FieldInfo*            (*fn_class_get_fields)(Il2CppClass*, void**);
typedef const char*           (*fn_field_get_name)(FieldInfo*);
typedef size_t                (*fn_field_get_offset)(FieldInfo*);
typedef const Il2CppType*     (*fn_field_get_type)(FieldInfo*);
typedef char*                 (*fn_type_get_name)(const Il2CppType*);
typedef int32_t               (*fn_class_array_element_size)(const Il2CppClass*);

struct Il2CppApi {
    fn_domain_get                domain_get;
    fn_domain_get_assemblies     domain_get_assemblies;
    fn_thread_attach             thread_attach;
    fn_assembly_get_image        assembly_get_image;
    fn_image_get_name            image_get_name;
    fn_image_get_class_count     image_get_class_count;
    fn_image_get_class           image_get_class;
    fn_class_from_name           class_from_name;
    fn_class_get_name            class_get_name;
    fn_class_get_namespace       class_get_namespace;
    fn_class_get_parent          class_get_parent;
    fn_class_is_assignable_from  class_is_assignable_from;
    fn_class_get_type            class_get_type;
    fn_class_get_methods         class_get_methods;
    fn_method_get_name           method_get_name;
    fn_method_get_param_count    method_get_param_count;
    fn_method_get_param          method_get_param;
    fn_class_from_type           class_from_type;
    fn_runtime_invoke            runtime_invoke;
    fn_string_new                string_new;
    fn_object_new                object_new;
    fn_object_get_class          object_get_class;
    fn_array_new                 array_new;
    fn_type_get_object           type_get_object;
    fn_class_get_fields          class_get_fields;
    fn_field_get_name            field_get_name;
    fn_field_get_offset          field_get_offset;
    fn_field_get_type            field_get_type;
    fn_type_get_name             type_get_name;
    fn_class_array_element_size  class_array_element_size;
};

extern Il2CppApi g_api;
