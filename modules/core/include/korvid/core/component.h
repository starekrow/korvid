// Korvid core: component schemas. A component is a plain struct a module declares as
// data: name, size, alignment, a field table and flags. The schema is the reflection
// everything else reads: the engine lays out columns from it, hashes and compares rows
// by field (padding never counts), prints any row for a tool, and knows which fields are
// handles. Declaring is one table per type and one row per field:
//
//   constinit const FieldDesc k_velocity_fields[] = {KV_FIELDS(Velocity, linear, angular)};
//   constinit const ComponentDesc k_components[]  = {KV_COMPONENT(Velocity, k_velocity_fields)};
//
// A field's scalar type and count come from FieldTraits<M>: core covers the scalars,
// Handle and fixed arrays; math/fields.h covers vec2, point2, rect2 and color. A struct
// with an unknown field type fails to compile, never to describe.
#pragma once
#include "korvid/core/handle.h"

namespace kv {

enum class FieldType : u8 { Bool, U8, U16, U32, U64, I8, I16, I32, I64, F32, F64, Handle, Count };

u32         field_type_size(FieldType t);
const char* field_type_name(FieldType t);

// One field: `count` scalars of `type`, contiguous at `offset`. vec2 is two F32.
struct FieldDesc {
    const char* name;
    FieldType   type;
    u16         count;
    u16         offset;
};

// Flags OR together. A component is sim state unless it says otherwise.
enum ComponentFlags : u8 {
    CompSim      = 0, // authoritative: hashed, snapshotted, visible to triggers
    CompCosmetic = 1, // presentation only: never hashed, never read by a sim system
    CompPrev     = 2, // the engine keeps a previous-tick copy
};

// The id slot is ComponentType<T>::id, filled at init like events.
struct ComponentDesc {
    const char*           name;
    u32                   size;
    u32                   align;
    Span<const FieldDesc> fields;
    u8                    flags;
    u32*                  id;
    const void*           type; // TypeKey<T>, so a typed access can be checked against the schema

    bool is_sim()   const { return (flags & CompCosmetic) == 0; }
    bool has_prev() const { return (flags & CompPrev) != 0; }
};

template <class T>
struct ComponentType {
    static_assert(__is_trivially_copyable(T), "components are plain data");
    static_assert(__is_standard_layout(T), "components are standard layout, so offsets are well defined");
    static inline u32 id = 0;
};

// Scalar type and count of a member type. Specialize for your own plain aggregates.
template <class M> struct FieldTraits;
#define KV_FIELD_TRAITS(M, T, N) \
    template <> struct FieldTraits<M> { static constexpr FieldType type = FieldType::T; static constexpr u16 count = N; }
KV_FIELD_TRAITS(bool, Bool, 1);
KV_FIELD_TRAITS(u8,  U8,  1);
KV_FIELD_TRAITS(u16, U16, 1);
KV_FIELD_TRAITS(u32, U32, 1);
KV_FIELD_TRAITS(u64, U64, 1);
KV_FIELD_TRAITS(i8,  I8,  1);
KV_FIELD_TRAITS(i16, I16, 1);
KV_FIELD_TRAITS(i32, I32, 1);
KV_FIELD_TRAITS(i64, I64, 1);
KV_FIELD_TRAITS(f32, F32, 1);
KV_FIELD_TRAITS(f64, F64, 1);
KV_FIELD_TRAITS(Handle, Handle, 1);
template <class M, usize N>
struct FieldTraits<M[N]> {
    static constexpr FieldType type  = FieldTraits<M>::type;
    static constexpr u16       count = (u16)(FieldTraits<M>::count * N);
};
// An enum field is its underlying integer.
template <class E> requires __is_enum(E)
struct FieldTraits<E> : FieldTraits<__underlying_type(E)> {};

// ---- schema operations: every one reads only the field table ----------------------
// A field table describes any plain struct: components and events share these.

// Null if the table is consistent for a struct of `size` bytes; otherwise a static string
// naming what is wrong, with the offending field's name in `field` if there is one.
const char*      schema_check(u32 size, Span<const FieldDesc> fields, const char** field);
const FieldDesc* schema_field(Span<const FieldDesc> fields, const char* name);
bool             schema_has_handles(Span<const FieldDesc> fields);
// Field by field, so padding never matters. The hash is the fixed engine hash over the
// scalars' bit patterns in declaration order: identical across runs and platforms.
u64  schema_hash(Span<const FieldDesc> fields, const void* row);
bool schema_equal(Span<const FieldDesc> fields, const void* a, const void* b);
// "Name{field=value, vec=(a, b), h=kind#index}". Returns what snprintf would.
int  schema_describe(const char* name, Span<const FieldDesc> fields, const void* row, char* out, usize cap);
// Calls fn(handle&) for every handle scalar in the row; stops early if fn returns false.
// Delivery uses it to drop events that refer to the dead.
bool schema_each_handle(Span<const FieldDesc> fields, void* row, bool (*fn)(Handle&, void*), void* user);
// One scalar field as a double (integers widened, floats converted); false for handles.
bool schema_field_as_f64(const FieldDesc& f, const void* row, f64* out);

inline const char*      component_check(const ComponentDesc& c, const char** field) {
    *field = nullptr;
    if (!c.name || !c.name[0]) return "has no name";
    if (c.size == 0) return "has zero size";
    if (c.align == 0 || (c.align & (c.align - 1))) return "alignment is not a power of two";
    if (!c.id || !c.type) return "was not declared with KV_COMPONENT";
    return schema_check(c.size, c.fields, field);
}
inline const FieldDesc* component_field(const ComponentDesc& c, const char* name) { return schema_field(c.fields, name); }
inline u64  component_hash(const ComponentDesc& c, const void* row) { return schema_hash(c.fields, row); }
inline bool component_equal(const ComponentDesc& c, const void* a, const void* b) { return schema_equal(c.fields, a, b); }
inline int  component_describe(const ComponentDesc& c, const void* row, char* out, usize cap) { return schema_describe(c.name, c.fields, row, out, cap); }
inline bool component_each_handle(const ComponentDesc& c, void* row, bool (*fn)(Handle&, void*), void* user) { return schema_each_handle(c.fields, row, fn, user); }

} // namespace kv

#define KV_FIELD(T, m) \
    ::kv::FieldDesc{#m, ::kv::FieldTraits<decltype(T::m)>::type, ::kv::FieldTraits<decltype(T::m)>::count, (::kv::u16)offsetof(T, m)}

// The whole table from the member names, up to sixteen:
//   constinit const FieldDesc k_velocity_fields[] = {KV_FIELDS(Velocity, linear, angular)};
#define KV_FIELDS(T, ...) KV_CAT_(KV_FIELDS_, KV_NARGS_(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1))(T, __VA_ARGS__)
#define KV_NARGS_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
#define KV_CAT_(a, b) KV_CAT__(a, b)
#define KV_CAT__(a, b) a##b
#define KV_FIELDS_1(T, a)       KV_FIELD(T, a)
#define KV_FIELDS_2(T, a, ...)  KV_FIELD(T, a), KV_FIELDS_1(T, __VA_ARGS__)
#define KV_FIELDS_3(T, a, ...)  KV_FIELD(T, a), KV_FIELDS_2(T, __VA_ARGS__)
#define KV_FIELDS_4(T, a, ...)  KV_FIELD(T, a), KV_FIELDS_3(T, __VA_ARGS__)
#define KV_FIELDS_5(T, a, ...)  KV_FIELD(T, a), KV_FIELDS_4(T, __VA_ARGS__)
#define KV_FIELDS_6(T, a, ...)  KV_FIELD(T, a), KV_FIELDS_5(T, __VA_ARGS__)
#define KV_FIELDS_7(T, a, ...)  KV_FIELD(T, a), KV_FIELDS_6(T, __VA_ARGS__)
#define KV_FIELDS_8(T, a, ...)  KV_FIELD(T, a), KV_FIELDS_7(T, __VA_ARGS__)
#define KV_FIELDS_9(T, a, ...)  KV_FIELD(T, a), KV_FIELDS_8(T, __VA_ARGS__)
#define KV_FIELDS_10(T, a, ...) KV_FIELD(T, a), KV_FIELDS_9(T, __VA_ARGS__)
#define KV_FIELDS_11(T, a, ...) KV_FIELD(T, a), KV_FIELDS_10(T, __VA_ARGS__)
#define KV_FIELDS_12(T, a, ...) KV_FIELD(T, a), KV_FIELDS_11(T, __VA_ARGS__)
#define KV_FIELDS_13(T, a, ...) KV_FIELD(T, a), KV_FIELDS_12(T, __VA_ARGS__)
#define KV_FIELDS_14(T, a, ...) KV_FIELD(T, a), KV_FIELDS_13(T, __VA_ARGS__)
#define KV_FIELDS_15(T, a, ...) KV_FIELD(T, a), KV_FIELDS_14(T, __VA_ARGS__)
#define KV_FIELDS_16(T, a, ...) KV_FIELD(T, a), KV_FIELDS_15(T, __VA_ARGS__)

// KV_COMPONENT(Velocity, k_velocity_fields) or KV_COMPONENT(Transform2, k_fields, CompPrev).
#define KV_COMPONENT(T, fields, ...)                                                               \
    ::kv::ComponentDesc{#T, (::kv::u32)sizeof(T), (::kv::u32)alignof(T), fields,                   \
                        (::kv::u8)(0 __VA_OPT__(| (__VA_ARGS__))), &::kv::ComponentType<T>::id, \
                        &::kv::TypeKey<T>::key}
