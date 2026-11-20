// Korvid core: components, and the field tables that describe them.
//
// A component is one piece of data an entity has: a position, a velocity, a health. In
// code it is a plain struct (no constructors that matter, no pointers to own, no virtual
// functions). Entities live in ktables (ktable.h), each a fixed set of components stored
// as one column per component.
//
// The engine can't see inside a struct on its own, so each component comes with a field
// table: one row per member, made by a macro from the member names. That table is the
// reflection everything else reads. The engine lays columns out from it, hashes and
// compares rows field by field (padding never counts), saves and restores them, prints
// any row in the CLI and the logs, and knows which fields are handles, so it can drop
// events about entities that died.
//
//   struct Velocity { vec2 v; f32 spin; };   // pixels per second, radians per second
//   struct Health   { i32 hp; };
//
//   constinit const FieldDesc k_velocity_fields[] = {KV_FIELDS(Velocity, v, spin)};
//   constinit const FieldDesc k_health_fields[]   = {KV_FIELDS(Health, hp)};
//   constinit const ComponentDesc k_components[] = {
//       KV_COMPONENT(Velocity, k_velocity_fields),
//       KV_COMPONENT(Health, k_health_fields, CompPrev),  // keep last tick's copy: a trigger watches it
//   };
//   // ModuleDesc{.components = k_components, ...}; a KTableDesc then lists "Velocity, Health"
//
// List every member. A member left out of the table is stored and copied with the row,
// but not hashed, compared, saved or shown.
//
// A field's scalar type and count come from FieldTraits<M>: core covers the integers,
// floats, bool, Handle, enums and fixed arrays (char[N] is a string); math/fields.h covers
// vec2, point2, rect2 and color. A member of any other type fails to compile, with
// FieldTraits named; KV_FIELD_TRAITS teaches it a plain aggregate of your own.
#pragma once
#include "korvid/core/handle.h"

namespace kv {

// Char is a byte of UTF-8 text: a char[N] field is a NUL-terminated string of at most N - 1
// bytes, hashed and compared as bytes, printed and patched as a string.
enum class FieldType : u8 { Bool, U8, U16, U32, U64, I8, I16, I32, I64, F32, F64, Handle, Char, Count };

u32         field_type_size(FieldType t); // bytes of one scalar: 4 for F32, 8 for Handle
const char* field_type_name(FieldType t); // "f32", "handle", ...

// One field: `count` scalars of `type`, contiguous at `offset`. vec2 is two F32. Made by
// KV_FIELD and KV_FIELDS, never by hand.
struct FieldDesc {
    const char* name;   // the member's name, as the CLI and saves spell it
    FieldType   type;
    u16         count;  // scalars: 1 for an f32, 2 for a vec2, N for a char[N]
    u16         offset; // bytes from the start of the struct
};

// What the engine does with a component's columns; the third argument of KV_COMPONENT,
// ORed together. A component is sim state unless it says otherwise.
enum ComponentFlags : u8 {
    CompSim      = 0, // the default: simulation state, hashed, saved, visible to triggers
    CompCosmetic = 1, // presentation only (a flash timer, a glyph's look): never hashed;
                      // a sim system reading it fails engine_init: it can't steer the game
    CompPrev     = 2, // keep last tick's copy, taken at the start of PreSim: for triggers, for
                      // KTableRows::prev<T>(), and for interpolation (Transform2 has it)
};

// One component as the engine knows it: a row of a module's component table, made by
// KV_COMPONENT. Its name is the type's, and every module refers to it by that name.
struct ComponentDesc {
    const char*           name;  // "Velocity": global, not prefixed by the module
    u32                   size;
    u32                   align;
    Span<const FieldDesc> fields;
    u8                    flags; // ComponentFlags
    u32*                  id;    // &ComponentType<T>::id
    const void*           type; // TypeKey<T>, so a typed access can be checked against the schema

    bool is_sim()   const { return (flags & CompCosmetic) == 0; }
    bool has_prev() const { return (flags & CompPrev) != 0; }
};

// The id engine_init gives component T, for code that would rather not look it up by
// name: ctx.write<T>, views and spawns use it. 0 before init and after shutdown. T must be
// plain data with a standard layout.
template <class T>
struct ComponentType {
    static_assert(__is_trivially_copyable(T), "components are plain data");
    static_assert(__is_standard_layout(T), "components are standard layout, so offsets are well defined");
    static inline u32 id = 0;
};

// Scalar type and count of a member type. For a plain aggregate of your own that is a run
// of one scalar type, say so with KV_FIELD_TRAITS (at namespace kv scope), and it can be a
// field:
//   struct Rgb8 { u8 r, g, b; };
//   namespace kv { KV_FIELD_TRAITS(Rgb8, U8, 3); }
template <class M> struct FieldTraits;
#define KV_FIELD_TRAITS(M, T, N) \
    template <> struct FieldTraits<M> { static constexpr FieldType type = FieldType::T; static constexpr u16 count = N; }
KV_FIELD_TRAITS(bool, Bool, 1);
KV_FIELD_TRAITS(char, Char, 1); // a char[N] field is a UTF-8 string in N bytes
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
// A field table describes any plain struct: components and events share these. Tools,
// saves and the CLI use them to work on rows they know nothing else about.

// Null if the table is consistent for a struct of `size` bytes; otherwise a static string
// naming what is wrong, with the offending field's name in `field` if there is one.
const char*      schema_check(u32 size, Span<const FieldDesc> fields, const char** field);
// The field with that name, or null.
const FieldDesc* schema_field(Span<const FieldDesc> fields, const char* name);
// Whether any field is a Handle.
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
// One scalar field as a double (integers widened, floats converted); false for handles
// and strings.
bool schema_field_as_f64(const FieldDesc& f, const void* row, f64* out);
// Lane `lane` of a field (0 for a scalar, 0..count-1 for a vector) as a double, and the
// reverse: a double stored into it with the field's type (rounded for integers, clamped
// to a bool). False for a handle, a string, or a lane past the count. The CLI's print and patch.
bool schema_field_lane(const FieldDesc& f, const void* row, u32 lane, f64* out);
bool schema_field_set(const FieldDesc& f, void* row, u32 lane, f64 value);
// A Char field as a quoted string: UTF-8 passed through, `"` and `\` escaped, control
// bytes as \xNN, up to the first NUL or the field's end. snprintf's return convention.
int  schema_field_string(const FieldDesc& f, const void* row, char* out, usize cap);
// Stores a UTF-8 string into a Char field, truncated at a code point boundary to fit
// with its NUL, the rest zeroed. False for a field that is not Char.
bool schema_field_set_string(const FieldDesc& f, void* row, const char* str);

// The schema operations on a ComponentDesc: component_check adds the checks engine_init
// makes on a component (a name, a size, a power-of-two alignment, made by KV_COMPONENT).
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

// One field of struct T, from the member's name. KV_FIELDS is the usual way to write a
// table; KV_FIELD is for listing members one by one.
// (Entries are braced lists, not FieldDesc{...}: cl will not constant-initialize a
// constinit object from a typed prvalue, though the standard makes the two the same.
// KV_COMPONENT and KV_EVENT are written the same way.)
#define KV_FIELD(T, m) \
    {#m, ::kv::FieldTraits<decltype(T::m)>::type, ::kv::FieldTraits<decltype(T::m)>::count, (::kv::u16)offsetof(T, m)}

// The whole table from the member names, up to sixteen (KV_FIELD for more):
//   constinit const FieldDesc k_velocity_fields[] = {KV_FIELDS(Velocity, v, spin)};
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

// One row of a module's component table: the type, its field table, and optional
// ComponentFlags.
//   KV_COMPONENT(Velocity, k_velocity_fields)
//   KV_COMPONENT(Transform2, k_fields, CompPrev)
#define KV_COMPONENT(T, fields, ...)                                                               \
    {#T, (::kv::u32)sizeof(T), (::kv::u32)alignof(T), fields,                   \
                        (::kv::u8)(0 __VA_OPT__(| (__VA_ARGS__))), &::kv::ComponentType<T>::id, \
                        &::kv::TypeKey<T>::key}
