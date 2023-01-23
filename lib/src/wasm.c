#include "tree_sitter/api.h"
#include "tree_sitter/parser.h"
#include "./language.h"

#ifdef TREE_SITTER_FEATURE_WASM

#include <wasmtime.h>
#include <wasm.h>
#include <wctype.h>
#include <string.h>
#include "./alloc.h"
#include "./language.h"
#include "./array.h"
#include "./atomic.h"
#include "./lexer.h"
#include "./wasm.h"
#include "./lexer.h"
#include "./wasm/wasm-stdlib.h"

#define STDLIB_SYMBOL_COUNT 30
const char *STDLIB_SYMBOLS[STDLIB_SYMBOL_COUNT] = {
  "malloc",
  "free",
  "calloc",
  "realloc",
  "memchr",
  "memcmp",
  "memcpy",
  "memmove",
  "memset",
  "strlen",
  "towupper",
  "towlower",
  "iswdigit",
  "iswalpha",
  "iswalnum",
  "iswspace",
  "iswupper",
  "iswlower",
  "_Znwm",
  "_ZdlPv",
  "_ZNKSt3__212basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4copyEPcmm",
  "_ZNSt3__212basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcm",
  "_ZNSt3__212basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7reserveEm",
  "_ZNSt3__212basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9__grow_byEmmmmmm",
  "_ZNSt3__212basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9push_backEc",
  "_ZNSt3__212basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED2Ev",
  "_ZNSt3__212basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE9push_backEw",
  "_ZNSt3__212basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEED2Ev",

  "_ZNSt3__212basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE25__init_copy_ctor_externalEPKcm",
  "_ZNSt3__212basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE17__assign_externalEPKcm",
};

typedef struct {
  wasmtime_module_t *module;
  uint32_t language_id;
  const char *name;
  char *symbol_name_buffer;
  char *field_name_buffer;
} LanguageWasmModule;

typedef struct {
  uint32_t language_id;
  wasmtime_instance_t instance;
  int32_t external_states_address;
  int32_t lex_main_fn_index;
  int32_t lex_keyword_fn_index;
  int32_t scanner_create_fn_index;
  int32_t scanner_destroy_fn_index;
  int32_t scanner_serialize_fn_index;
  int32_t scanner_deserialize_fn_index;
  int32_t scanner_scan_fn_index;
} LanguageWasmInstance;

struct TSWasmStore {
  wasm_engine_t *engine;
  wasmtime_store_t *store;
  wasmtime_table_t function_table;
  wasmtime_memory_t memory;
  TSLexer *current_lexer;
  LanguageWasmInstance *current_instance;
  Array(LanguageWasmInstance) language_instances;
  uint32_t current_memory_offset;
  uint32_t current_function_table_offset;
  uint16_t fn_indices[STDLIB_SYMBOL_COUNT];
};

typedef Array(char) StringData;

typedef struct {
  uint32_t version;
  uint32_t symbol_count;
  uint32_t alias_count;
  uint32_t token_count;
  uint32_t external_token_count;
  uint32_t state_count;
  uint32_t large_state_count;
  uint32_t production_id_count;
  uint32_t field_count;
  uint16_t max_alias_sequence_length;
  int32_t parse_table;
  int32_t small_parse_table;
  int32_t small_parse_table_map;
  int32_t parse_actions;
  int32_t symbol_names;
  int32_t field_names;
  int32_t field_map_slices;
  int32_t field_map_entries;
  int32_t symbol_metadata;
  int32_t public_symbol_map;
  int32_t alias_map;
  int32_t alias_sequences;
  int32_t lex_modes;
  int32_t lex_fn;
  int32_t keyword_lex_fn;
  TSSymbol keyword_capture_token;
  struct {
    int32_t states;
    int32_t symbol_map;
    int32_t create;
    int32_t destroy;
    int32_t scan;
    int32_t serialize;
    int32_t deserialize;
  } external_scanner;
  int32_t primary_state_ids;
} LanguageInWasmMemory;

typedef struct {
  int32_t lookahead;
  TSSymbol result_symbol;
  int32_t advance;
  int32_t mark_end;
  int32_t get_column;
  int32_t is_at_included_range_start;
  int32_t eof;
} LexerInWasmMemory;

typedef struct {
  uint32_t memory_size;
  uint32_t memory_align;
  uint32_t table_size;
  uint32_t table_align;
} WasmDylinkMemoryInfo;

static volatile uint32_t NEXT_LANGUAGE_ID;

static const uint32_t STACK_SIZE = 64 * 1024;
static const uint32_t HEAP_SIZE = 1024 * 1024;
static const uint32_t SERIALIZATION_BUFFER_ADDRESS = STACK_SIZE - TREE_SITTER_SERIALIZATION_BUFFER_SIZE;
static const uint32_t LEXER_ADDRESS = SERIALIZATION_BUFFER_ADDRESS - sizeof(LexerInWasmMemory);
static const uint32_t INITIAL_STACK_POINTER_ADDRESS = LEXER_ADDRESS;
static const uint32_t HEAP_START_ADDRESS = STACK_SIZE;
static const uint32_t DATA_START_ADDRESS = STACK_SIZE + HEAP_SIZE;

enum FunctionIx {
  NULL_IX = 0,
  PROC_EXIT_IX,
  ABORT_IX,
  ASSERT_FAIL_IX,
  AT_EXIT_IX,
  LEXER_ADVANCE_IX,
  LEXER_MARK_END_IX,
  LEXER_GET_COLUMN_IX,
  LEXER_IS_AT_INCLUDED_RANGE_START_IX,
  LEXER_EOF_IX,
};

static uint8_t read_u8(const uint8_t **p, const uint8_t *end) {
  return *(*p)++;
}

static inline uint64_t read_uleb128(const uint8_t **p, const uint8_t *end) {
  uint64_t value = 0;
  unsigned shift = 0;
  do {
    if (*p == end)  return UINT64_MAX;
    value += (uint64_t)(**p & 0x7f) << shift;
    shift += 7;
  } while (*((*p)++) >= 128);
  return value;
}

static bool parse_wasm_dylink_memory_info(
  const uint8_t *bytes,
  size_t length,
  WasmDylinkMemoryInfo *info
) {
  const uint8_t WASM_MAGIC_NUMBER[4] = {0, 'a', 's', 'm'};
  const uint8_t WASM_VERSION[4] = {1, 0, 0, 0};
  const uint8_t WASM_CUSTOM_SECTION = 0x0;
  const uint8_t WASM_DYLINK_MEM_INFO = 0x1;

  const uint8_t *p = bytes;
  const uint8_t *end = bytes + length;

  if (length < 8) return false;
  if (memcmp(p, WASM_MAGIC_NUMBER, 4) != 0) return false;
  p += 4;
  if (memcmp(p, WASM_VERSION, 4) != 0) return false;
  p += 4;

  while (p < end) {
    uint8_t section_id = read_u8(&p, end);
    uint32_t section_length = read_uleb128(&p, end);
    const uint8_t *section_end = p + section_length;
    if (section_end > end) return false;

    if (section_id == WASM_CUSTOM_SECTION) {
      uint32_t name_length = read_uleb128(&p, section_end);
      const uint8_t *name_end = p + name_length;
      if (name_end > section_end) return false;

      if (name_length == 8 && memcmp(p, "dylink.0", 8) == 0) {
        p = name_end;
        while (p < section_end) {
          uint8_t subsection_type = read_u8(&p, section_end);
          uint32_t subsection_size = read_uleb128(&p, section_end);
          const uint8_t *subsection_end = p + subsection_size;
          if (subsection_end > section_end) return false;
          if (subsection_type == WASM_DYLINK_MEM_INFO) {
            info->memory_size = read_uleb128(&p, subsection_end);
            info->memory_align = read_uleb128(&p, subsection_end);
            info->table_size = read_uleb128(&p, subsection_end);
            info->table_align = read_uleb128(&p, subsection_end);
            return true;
          }
          p = subsection_end;
        }
      }
    }
    p = section_end;
  }
  return false;
}

static wasm_trap_t *callback__exit(
  void *env,
  wasmtime_caller_t* caller,
  wasmtime_val_raw_t *args_and_results,
  size_t args_and_results_len
) {
  printf("exit called");
  abort();
}

static wasm_trap_t *callback__at_exit(
  void *env,
  wasmtime_caller_t* caller,
  wasmtime_val_raw_t *args_and_results,
  size_t args_and_results_len
) {
  printf("atexit called");
  abort();
}

static wasm_trap_t *callback__assert_fail(
  void *env,
  wasmtime_caller_t* caller,
  wasmtime_val_raw_t *args_and_results,
  size_t args_and_results_len
) {
  printf("assert failed called");
  abort();
}

static wasm_trap_t *callback__lexer_advance(
  void *env,
  wasmtime_caller_t* caller,
  wasmtime_val_raw_t *args_and_results,
  size_t args_and_results_len
) {
  wasmtime_context_t *context = wasmtime_caller_context(caller);
  assert(args_and_results_len == 2);

  TSWasmStore *store = env;
  TSLexer *lexer = store->current_lexer;
  bool skip = args_and_results[1].i32;
  lexer->advance(lexer, skip);

  uint8_t *memory = wasmtime_memory_data(context, &store->memory);
  memcpy(&memory[LEXER_ADDRESS], &lexer->lookahead, sizeof(lexer->lookahead));
  return NULL;
}

static wasm_trap_t *callback__lexer_mark_end(
  void *env,
  wasmtime_caller_t* caller,
  wasmtime_val_raw_t *args_and_results,
  size_t args_and_results_len
) {
  TSWasmStore *store = env;
  TSLexer *lexer = store->current_lexer;
  lexer->mark_end(lexer);
  return NULL;
}

static wasm_trap_t *callback__lexer_get_column(
  void *env,
  wasmtime_caller_t* caller,
  wasmtime_val_raw_t *args_and_results,
  size_t args_and_results_len
) {
  TSWasmStore *store = env;
  TSLexer *lexer = store->current_lexer;
  uint32_t result = lexer->get_column(lexer);
  args_and_results[0].i32 = result;
  return NULL;
}

static wasm_trap_t *callback__lexer_is_at_included_range_start(
  void *env,
  wasmtime_caller_t* caller,
  wasmtime_val_raw_t *args_and_results,
  size_t args_and_results_len
) {
  TSWasmStore *store = env;
  TSLexer *lexer = store->current_lexer;
  bool result = lexer->is_at_included_range_start(lexer);
  args_and_results[0].i32 = result;
  return NULL;
}

static wasm_trap_t *callback__lexer_eof(
  void *env,
  wasmtime_caller_t* caller,
  wasmtime_val_raw_t *args_and_results,
  size_t args_and_results_len
) {
  TSWasmStore *store = env;
  TSLexer *lexer = store->current_lexer;
  bool result = lexer->eof(lexer);
  args_and_results[0].i32 = result;
  return NULL;
}

typedef struct {
  wasmtime_func_unchecked_callback_t callback;
  wasm_functype_t *type;
} FunctionDefinition;

#define array_len(a) (sizeof(a) / sizeof(a[0]))

static void *copy(const void *data, size_t size) {
  void *result = ts_malloc(size);
  memcpy(result, data, size);
  return result;
}

static void *copy_strings(
  const uint8_t *data,
  int32_t array_address,
  size_t count,
  StringData *string_data
) {
  const char **result = ts_malloc(count * sizeof(char *));
  for (unsigned i = 0; i < count; i++) {
    int32_t address;
    memcpy(&address, &data[array_address + i * sizeof(address)], sizeof(address));
    if (address == 0) {
      result[i] = (const char *)-1;
    } else {
      const uint8_t *string = &data[address];
      uint32_t len = strlen((const char *)string);
      result[i] = (const char *)(uintptr_t)string_data->size;
      array_extend(string_data, len + 1, string);
    }
  }
  for (unsigned i = 0; i < count; i++) {
    if (result[i] == (const char *)-1) {
      result[i] = NULL;
    } else {
      result[i] = string_data->contents + (uintptr_t)result[i];
    }
  }
  return result;
}

static bool name_eq(const wasm_name_t *name, const char *string) {
  return strncmp(string, name->data, name->size) == 0;
}

static inline wasm_functype_t* wasm_functype_new_4_0(
  wasm_valtype_t* p1,
  wasm_valtype_t* p2,
  wasm_valtype_t* p3,
  wasm_valtype_t* p4
) {
  wasm_valtype_t* ps[4] = {p1, p2, p3, p4};
  wasm_valtype_vec_t params, results;
  wasm_valtype_vec_new(&params, 4, ps);
  wasm_valtype_vec_new_empty(&results);
  return wasm_functype_new(&params, &results);
}

static wasmtime_extern_t get_builtin_func_extern(
  wasmtime_context_t *context,
  wasmtime_table_t *table,
  unsigned index
) {
  wasmtime_val_t val;
  bool exists = wasmtime_table_get(context, table, index, &val);
  assert(exists);
  return (wasmtime_extern_t) {.kind = WASMTIME_EXTERN_FUNC, .of.func = val.of.funcref};
}

TSWasmStore *ts_wasm_store_new(TSWasmEngine *engine) {
  TSWasmStore *self = ts_malloc(sizeof(TSWasmStore));
  wasmtime_store_t *store = wasmtime_store_new(engine, self, NULL);
  wasmtime_context_t *context = wasmtime_store_context(store);
  wasmtime_error_t *error = NULL;
  wasm_trap_t *trap = NULL;

  // Initialize store's memory
  wasm_limits_t memory_limits = {.min = 256, .max = 256};
  wasm_memorytype_t *memory_type = wasm_memorytype_new(&memory_limits);
  wasmtime_memory_t memory;
  error = wasmtime_memory_new(context, memory_type, &memory);
  assert(!error);
  wasm_memorytype_delete(memory_type);

  // Initialize lexer struct with function pointers in wasm memory.
  uint8_t *memory_data = wasmtime_memory_data(context, &memory);
  LexerInWasmMemory lexer = {
    .lookahead = 0,
    .result_symbol = 0,
    .advance = LEXER_ADVANCE_IX,
    .mark_end = LEXER_MARK_END_IX,
    .get_column = LEXER_GET_COLUMN_IX,
    .is_at_included_range_start = LEXER_IS_AT_INCLUDED_RANGE_START_IX,
    .eof = LEXER_EOF_IX,
  };
  memcpy(&memory_data[LEXER_ADDRESS], &lexer, sizeof(lexer));

  // Define builtin functions.
  FunctionDefinition definitions[] = {
    [NULL_IX] = {NULL, NULL},
    [PROC_EXIT_IX] = {callback__exit, wasm_functype_new_1_0(wasm_valtype_new_i32())},
    [ABORT_IX] = {callback__exit, wasm_functype_new_0_0()},
    [AT_EXIT_IX] = {callback__at_exit, wasm_functype_new_3_1(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32())},
    [ASSERT_FAIL_IX] = {callback__assert_fail, wasm_functype_new_4_0(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32())},
    [LEXER_ADVANCE_IX] = {callback__lexer_advance, wasm_functype_new_2_0(wasm_valtype_new_i32(), wasm_valtype_new_i32())},
    [LEXER_MARK_END_IX] = {callback__lexer_mark_end, wasm_functype_new_1_0(wasm_valtype_new_i32())},
    [LEXER_GET_COLUMN_IX] = {callback__lexer_get_column, wasm_functype_new_1_1(wasm_valtype_new_i32(), wasm_valtype_new_i32())},
    [LEXER_IS_AT_INCLUDED_RANGE_START_IX] = {callback__lexer_is_at_included_range_start, wasm_functype_new_1_1(wasm_valtype_new_i32(), wasm_valtype_new_i32())},
    [LEXER_EOF_IX] = {callback__lexer_eof, wasm_functype_new_1_1(wasm_valtype_new_i32(), wasm_valtype_new_i32())},
  };
  unsigned definitions_len = array_len(definitions);

  // Add builtin functions to the store's function table.
  wasmtime_table_t function_table;
  wasm_limits_t table_limits = {.min = definitions_len, .max = wasm_limits_max_default};
  wasm_tabletype_t *table_type = wasm_tabletype_new(wasm_valtype_new(WASM_FUNCREF), &table_limits);
  wasmtime_val_t initializer = {.kind = WASMTIME_FUNCREF};
  error = wasmtime_table_new(context, table_type, &initializer, &function_table);
  assert(!error);
  wasm_tabletype_delete(table_type);

  uint32_t prev_size;
  error = wasmtime_table_grow(context, &function_table, definitions_len, &initializer, &prev_size);
  assert(!error);
  for (unsigned i = 1; i < definitions_len; i++) {
    FunctionDefinition *definition = &definitions[i];
    wasmtime_func_t func;
    wasmtime_func_new_unchecked(context, definition->type, definition->callback, self, NULL, &func);
    wasmtime_val_t func_val = {.kind = WASMTIME_FUNCREF, .of.funcref = func};
    error = wasmtime_table_set(context, &function_table, i, &func_val);
    assert(!error);
    wasm_functype_delete(definition->type);
  }

  WasmDylinkMemoryInfo stdlib_info;
  if (!parse_wasm_dylink_memory_info(STDLIB_WASM, STDLIB_WASM_LEN, &stdlib_info)) {
    printf("failed to parse wasm dylink info\n");
    abort();
  }
  printf("memory info: %u, %u\n", stdlib_info.memory_size, stdlib_info.table_size);

  wasmtime_module_t *stdlib_module;
  error = wasmtime_module_new(engine, STDLIB_WASM, STDLIB_WASM_LEN, &stdlib_module);
  assert(!error);

  wasmtime_val_t table_base_val = WASM_I32_VAL(definitions_len);
  wasmtime_val_t memory_base_val = WASM_I32_VAL(DATA_START_ADDRESS);
  wasmtime_val_t stack_pointer_val = WASM_I32_VAL(INITIAL_STACK_POINTER_ADDRESS);
  wasmtime_val_t heap_base_val = WASM_I32_VAL(HEAP_START_ADDRESS);
  wasmtime_global_t table_base_global;
  wasmtime_global_t memory_base_global;
  wasmtime_global_t heap_base_global;
  wasmtime_global_t stack_pointer_global;
  wasm_globaltype_t *const_i32_type = wasm_globaltype_new(wasm_valtype_new_i32(), WASM_CONST);
  wasm_globaltype_t *var_i32_type = wasm_globaltype_new(wasm_valtype_new_i32(), WASM_VAR);
  error = wasmtime_global_new(context, const_i32_type, &table_base_val, &table_base_global);
  assert(!error);
  error = wasmtime_global_new(context, const_i32_type, &memory_base_val, &memory_base_global);
  assert(!error);
  error = wasmtime_global_new(context, var_i32_type, &heap_base_val, &heap_base_global);
  assert(!error);
  error = wasmtime_global_new(context, var_i32_type, &stack_pointer_val, &stack_pointer_global);
  assert(!error);
  wasm_globaltype_delete(const_i32_type);
  wasm_globaltype_delete(var_i32_type);

  wasmtime_instance_t instance;
  wasm_importtype_vec_t import_types = WASM_EMPTY_VEC;
  wasmtime_module_imports(stdlib_module, &import_types);
  wasmtime_extern_t imports[import_types.size];
  for (unsigned i = 0; i < import_types.size; i++) {
    wasm_importtype_t *type = import_types.data[i];
    const wasm_name_t *import_name = wasm_importtype_name(type);
    printf("stdlib import name: %.*s\n", (int)import_name->size, import_name->data);
    if (name_eq(import_name, "__memory_base")) {
      imports[i] = (wasmtime_extern_t) {.kind = WASMTIME_EXTERN_GLOBAL, .of.global = memory_base_global};
    } else if (name_eq(import_name, "__heap_base")) {
      imports[i] = (wasmtime_extern_t) {.kind = WASMTIME_EXTERN_GLOBAL, .of.global = heap_base_global};
    } else if (name_eq(import_name, "__table_base")) {
      imports[i] = (wasmtime_extern_t) {.kind = WASMTIME_EXTERN_GLOBAL, .of.global = table_base_global};
    } else if (name_eq(import_name, "__stack_pointer")) {
      imports[i] = (wasmtime_extern_t) {.kind = WASMTIME_EXTERN_GLOBAL, .of.global = stack_pointer_global};
    } else if (name_eq(import_name, "__indirect_function_table")) {
      imports[i] = (wasmtime_extern_t) {.kind = WASMTIME_EXTERN_TABLE, .of.table = function_table};
    } else if (name_eq(import_name, "memory")) {
      imports[i] = (wasmtime_extern_t) {.kind = WASMTIME_EXTERN_MEMORY, .of.memory = memory};
    } else if (name_eq(import_name, "proc_exit")) {
      imports[i] = get_builtin_func_extern(context, &function_table, PROC_EXIT_IX);
    } else {
      printf("unexpected import");
      abort();
    }
  }

  error = wasmtime_instance_new(context, stdlib_module, imports, import_types.size, &instance, &trap);
  if (error) {
    wasm_message_t message;
    wasmtime_error_message(error, &message);
    printf("error compiling standard library: %.*s\n", (int)message.size, message.data);
    abort();
  }
  assert(!error);
  wasm_importtype_vec_delete(&import_types);

  *self = (TSWasmStore) {
    .store = store,
    .engine = engine,
    .memory = memory,
    .language_instances = array_new(),
    .function_table = function_table,
    .current_memory_offset = DATA_START_ADDRESS + stdlib_info.memory_size,
    .current_function_table_offset = definitions_len + stdlib_info.table_size,
  };

  for (unsigned i = 0; i < STDLIB_SYMBOL_COUNT; i++) {
    self->fn_indices[i] = UINT16_MAX;
  }

  // Process the stdlib module's exports.
  wasm_exporttype_vec_t export_types = WASM_EMPTY_VEC;
  wasmtime_module_exports(stdlib_module, &export_types);
  for (unsigned i = 0; i < export_types.size; i++) {
    wasm_exporttype_t *export_type = export_types.data[i];
    const wasm_name_t *name = wasm_exporttype_name(export_type);

    char *export_name;
    size_t name_len;
    wasmtime_extern_t export = {.kind = WASM_EXTERN_GLOBAL};
    bool exists = wasmtime_instance_export_nth(context, &instance, i, &export_name, &name_len, &export);
    assert(exists);

    bool store_index = false;
    if (export.kind == WASMTIME_EXTERN_FUNC) {
      for (unsigned j = 0; j < array_len(STDLIB_SYMBOLS); j++) {
        if (name_eq(name, STDLIB_SYMBOLS[j])) {
          self->fn_indices[j] = export.of.func.index;
          store_index = true;
          break;
        }
      }
      if (!store_index) {
        printf("  other stdlib name: %.*s\n", (int)name->size, name->data);
      }
    } 
  }

  for (unsigned i = 0; i < STDLIB_SYMBOL_COUNT; i++) {
    if (self->fn_indices[i] == UINT16_MAX) {
      printf("undefined stdlib import: %s\n", STDLIB_SYMBOLS[i]);
      abort();
    }
  }

  wasm_exporttype_vec_delete(&export_types);
  return self;
}

void ts_wasm_store_delete(TSWasmStore *self) {
  if (!self) return;
  wasmtime_store_delete(self->store);
  wasm_engine_delete(self->engine);
  array_delete(&self->language_instances);
  ts_free(self);
}

static bool ts_wasm_store__instantiate(
  TSWasmStore *self,
  wasmtime_module_t *module,
  const char *language_name,
  wasmtime_instance_t *result,
  int32_t *language_address
) {
  wasmtime_context_t *context = wasmtime_store_context(self->store);
  wasmtime_error_t *error = NULL;
  wasm_trap_t *trap = NULL;

  // Construct the language function name as string.
  unsigned prefix_len = strlen("tree_sitter_");
  unsigned name_len = strlen(language_name);
  char language_function_name[prefix_len + name_len + 1];
  memcpy(&language_function_name[0], "tree_sitter_", prefix_len);
  memcpy(&language_function_name[prefix_len], language_name, name_len);
  language_function_name[prefix_len + name_len] = '\0';
  
  // Construct globals representing the offset in memory and in the function
  // table where the module should be added.
  wasmtime_val_t table_base_val = WASM_I32_VAL(self->current_function_table_offset);
  wasmtime_val_t memory_base_val = WASM_I32_VAL(self->current_memory_offset);
  wasmtime_val_t stack_pointer_val = WASM_I32_VAL(INITIAL_STACK_POINTER_ADDRESS);
  wasmtime_global_t memory_base_global;
  wasmtime_global_t table_base_global;
  wasmtime_global_t stack_pointer_global;
  wasm_globaltype_t *const_i32_type = wasm_globaltype_new(wasm_valtype_new_i32(), WASM_CONST);
  wasm_globaltype_t *var_i32_type = wasm_globaltype_new(wasm_valtype_new_i32(), WASM_VAR);
  error = wasmtime_global_new(context, const_i32_type, &memory_base_val, &memory_base_global);
  assert(!error);
  error = wasmtime_global_new(context, const_i32_type, &table_base_val, &table_base_global);
  assert(!error);
  error = wasmtime_global_new(context, var_i32_type, &stack_pointer_val, &stack_pointer_global);
  assert(!error);
  wasm_globaltype_delete(const_i32_type);

  const uint64_t store_id = self->function_table.store_id;

  // Build the imports list for the module.
  wasm_importtype_vec_t import_types = WASM_EMPTY_VEC;
  wasmtime_module_imports(module, &import_types);
  wasmtime_extern_t imports[import_types.size];
  
  printf("import count: %lu\n", import_types.size);
  for (unsigned i = 0; i < import_types.size; i++) {
    const wasm_importtype_t *import_type = import_types.data[i];
    const wasm_name_t *import_name = wasm_importtype_name(import_type);
    if (import_name->size == 0) {
      return false;
    }

    // Initialization parameters
    if (name_eq(import_name, "__memory_base")) {
      imports[i] = (wasmtime_extern_t) {.kind = WASMTIME_EXTERN_GLOBAL, .of.global = memory_base_global};
    } else if (name_eq(import_name, "__table_base")) {
      imports[i] = (wasmtime_extern_t) {.kind = WASMTIME_EXTERN_GLOBAL, .of.global = table_base_global};
    } else if (name_eq(import_name, "__stack_pointer")) {
      imports[i] = (wasmtime_extern_t) {.kind = WASMTIME_EXTERN_GLOBAL, .of.global = stack_pointer_global};
    } else if (name_eq(import_name, "__indirect_function_table")) {
      imports[i] = (wasmtime_extern_t) {.kind = WASMTIME_EXTERN_TABLE, .of.table = self->function_table};
    } else if (name_eq(import_name, "memory")) {
      imports[i] = (wasmtime_extern_t) {.kind = WASMTIME_EXTERN_MEMORY, .of.memory = self->memory};
    }

    // Builtin functions
    else if (name_eq(import_name, "__assert_fail")) {
      imports[i] = get_builtin_func_extern(context, &self->function_table, ASSERT_FAIL_IX);
    } else if (name_eq(import_name, "__cxa_atexit")) {
      imports[i] = get_builtin_func_extern(context, &self->function_table, AT_EXIT_IX);
    } else if (name_eq(import_name, "abort")) {
      imports[i] = get_builtin_func_extern(context, &self->function_table, ABORT_IX);
    }

    else {
      bool defined_in_stdlib = false;
      for (unsigned j = 0; j < array_len(STDLIB_SYMBOLS); j++) {
        if (name_eq(import_name, STDLIB_SYMBOLS[j])) {
          uint16_t address = self->fn_indices[j];
          imports[i] = (wasmtime_extern_t) {.kind = WASMTIME_EXTERN_FUNC, .of.func = {store_id, address}};
          defined_in_stdlib = true;
          break;
        }
      }
  
      if (!defined_in_stdlib) {
        printf("unexpected import '%.*s'\n", (int)import_name->size, import_name->data);
        return false;
      }
    }
  }

  wasm_importtype_vec_delete(&import_types);

  wasmtime_instance_t instance;
  error = wasmtime_instance_new(context, module, imports, array_len(imports), &instance, &trap);
  if (error) {
    wasm_message_t message;
    wasmtime_error_message(error, &message);
    printf("error instantiating wasm module: %s\n", message.data);
    return false;
  }
  assert(!error);
  if (trap) {
    wasm_message_t message;
    wasm_trap_message(trap, &message);
    printf("error instantiating wasm module: %s\n", message.data);
    return false;
  }

  // Process the module's exports.
  wasmtime_extern_t language_extern;
  wasm_exporttype_vec_t export_types = WASM_EMPTY_VEC;
  wasmtime_module_exports(module, &export_types);
  for (unsigned i = 0; i < export_types.size; i++) {
    wasm_exporttype_t *export_type = export_types.data[i];
    const wasm_name_t *name = wasm_exporttype_name(export_type);

    char *export_name;
    size_t name_len;
    wasmtime_extern_t export = {.kind = WASM_EXTERN_GLOBAL};
    bool exists = wasmtime_instance_export_nth(context, &instance, i, &export_name, &name_len, &export);
    assert(exists);

    // Update pointers to reflect memory and function table offsets.
    if (name_eq(name, "__wasm_apply_data_relocs")) {
      wasmtime_func_t apply_relocation_func = export.of.func;
      error = wasmtime_func_call(context, &apply_relocation_func, NULL, 0, NULL, 0, &trap);
      assert(!error);
      if (trap) {
        wasm_message_t message;
        wasm_trap_message(trap, &message);
        printf("error calling relocation function: %s\n", message.data);
        abort();
      }
    }

    // Find the main language function for the module.
    else if (name_eq(name, language_function_name)) {
      language_extern = export;
    }
  }
  wasm_exporttype_vec_delete(&export_types);

  if (language_extern.kind != WASMTIME_EXTERN_FUNC) {
    printf("failed to find function %s\n", language_function_name);
    return false;
  }

  // Invoke the language function to get the static address of the language object.
  wasmtime_func_t language_func = language_extern.of.func;
  wasmtime_val_t language_address_val;
  error = wasmtime_func_call(context, &language_func, NULL, 0, &language_address_val, 1, &trap);
  assert(!error);
  if (trap) {
    wasm_message_t message;
    wasm_trap_message(trap, &message);
    printf("error calling language function: %s\n", message.data);
    return false;
  }

  assert(language_address_val.kind == WASMTIME_I32);
  *result = instance;
  *language_address = language_address_val.of.i32;
  return true;
}

static bool ts_wasm_store__sentinel_lex_fn(TSLexer *_lexer, TSStateId state) {
  return false;
}

const TSLanguage *ts_wasm_store_load_language(
  TSWasmStore *self,
  const char *language_name,
  const char *wasm,
  uint32_t wasm_len
) {
  // Compile the wasm code.
  wasmtime_module_t *module;
  wasmtime_error_t *error = wasmtime_module_new(self->engine, (const uint8_t *)wasm, wasm_len, &module);
  if (error) {
    wasm_message_t message;
    wasmtime_error_message(error, &message);
    printf("failed to load wasm language: %s", message.data);
    return NULL;
  }

  // Instantiate the module in this store.
  wasmtime_instance_t instance;
  int32_t language_address;
  if (!ts_wasm_store__instantiate(
    self,
    module,
    language_name,
    &instance,
    &language_address
  )) return NULL;

  // Copy all of the static data out of the language object in wasm memory,
  // constructing a native language object.
  LanguageInWasmMemory wasm_language;
  wasmtime_context_t *context = wasmtime_store_context(self->store);
  const uint8_t *memory = wasmtime_memory_data(context, &self->memory);
  memcpy(&wasm_language, &memory[language_address], sizeof(LanguageInWasmMemory));

  TSLanguage *language = ts_malloc(sizeof(TSLanguage));
  StringData symbol_name_buffer = array_new();
  StringData field_name_buffer = array_new();

  *language = (TSLanguage) {
    .version = wasm_language.version,
    .symbol_count = wasm_language.symbol_count,
    .alias_count = wasm_language.alias_count,
    .token_count = wasm_language.token_count,
    .external_token_count = wasm_language.external_token_count,
    .state_count = wasm_language.state_count,
    .large_state_count = wasm_language.large_state_count,
    .production_id_count = wasm_language.production_id_count,
    .field_count = wasm_language.field_count,
    .max_alias_sequence_length = wasm_language.max_alias_sequence_length,
    .keyword_capture_token = wasm_language.keyword_capture_token,
    .parse_table = copy(
      &memory[wasm_language.parse_table],
      wasm_language.large_state_count * wasm_language.symbol_count * sizeof(uint16_t)
    ),
    .parse_actions = copy(
      &memory[wasm_language.parse_actions],
      5655 * sizeof(TSParseActionEntry) // TODO - determine number of parse actions
    ),
    .symbol_names = copy_strings(
      memory,
      wasm_language.symbol_names,
      wasm_language.symbol_count + wasm_language.alias_count,
      &symbol_name_buffer
    ),
    .symbol_metadata = copy(
      &memory[wasm_language.symbol_metadata],
      (wasm_language.symbol_count + wasm_language.alias_count) * sizeof(TSSymbolMetadata)
    ),
    .public_symbol_map = copy(
      &memory[wasm_language.public_symbol_map],
      (wasm_language.symbol_count + wasm_language.alias_count) * sizeof(TSSymbol)
    ),
    .lex_modes = copy(
      &memory[wasm_language.lex_modes],
      wasm_language.state_count * sizeof(TSLexMode)
    ),
  };

  if (language->field_count > 0 && language->production_id_count > 0) {
    language->field_map_slices = copy(
      &memory[wasm_language.field_map_slices],
      wasm_language.production_id_count * sizeof(TSFieldMapSlice)
    );
    const TSFieldMapSlice last_field_map_slice = language->field_map_slices[language->production_id_count - 1];
    language->field_map_entries = copy(
      &memory[wasm_language.field_map_entries],
      (last_field_map_slice.index + last_field_map_slice.length) * sizeof(TSFieldMapEntry)
    );
    language->field_names = copy_strings(
      memory,
      wasm_language.field_names,
      wasm_language.field_count + 1,
      &field_name_buffer
    );
  }

  if (language->alias_count > 0 && language->production_id_count > 0) {
    // The alias map contains symbols, alias counts, and aliases, terminated by a null symbol.
    int32_t alias_map_size = 0;
    for (;;) {
      TSSymbol symbol;
      memcpy(&symbol, &memory[wasm_language.alias_map + alias_map_size], sizeof(symbol));
      alias_map_size += sizeof(TSSymbol);
      if (symbol == 0) break;
      uint16_t value_count;
      memcpy(&value_count, &memory[wasm_language.alias_map + alias_map_size], sizeof(value_count));
      alias_map_size += value_count * sizeof(TSSymbol);
    }
    language->alias_map = copy(
      &memory[wasm_language.alias_map],
      alias_map_size * sizeof(TSSymbol)
    );
    language->alias_sequences = copy(
      &memory[wasm_language.alias_sequences],
      wasm_language.production_id_count * wasm_language.max_alias_sequence_length * sizeof(TSSymbol)
    );
  }

  if (language->state_count > language->large_state_count) {
    uint32_t small_state_count = wasm_language.state_count - wasm_language.large_state_count;
    language->small_parse_table_map = copy(
      &memory[wasm_language.small_parse_table_map],
      small_state_count * sizeof(uint32_t)
    );
    uint32_t index = language->small_parse_table_map[small_state_count - 1];
    language->small_parse_table = copy(
      &memory[wasm_language.small_parse_table],
      (index + 64) * sizeof(uint16_t) // TODO - determine actual size
    );
  }

  if (language->version >= 14) {
    language->primary_state_ids = copy(
      &memory[wasm_language.primary_state_ids],
      wasm_language.state_count * sizeof(TSStateId)
    );
  }

  if (language->external_token_count > 0) {
    language->external_scanner.symbol_map = copy(
      &memory[wasm_language.external_scanner.symbol_map],
      wasm_language.external_token_count * sizeof(TSSymbol)
    );
    language->external_scanner.states = (void *)(uintptr_t)wasm_language.external_scanner.states;
  }

  unsigned name_len = strlen(language_name);
  char *name = ts_malloc(name_len + 1);
  memcpy(name, language_name, name_len);
  name[name_len] = '\0';

  LanguageWasmModule *language_module = ts_malloc(sizeof(LanguageWasmModule));
  *language_module = (LanguageWasmModule) {
    .language_id = atomic_inc(&NEXT_LANGUAGE_ID),
    .module = module,
    .name = name,
    .symbol_name_buffer = symbol_name_buffer.contents,
    .field_name_buffer = field_name_buffer.contents,
  };

  // The lex functions are not used for wasm languages. Use those two fields
  // to mark this language as WASM-based and to store the language's
  // WASM-specific data.
  language->lex_fn = ts_wasm_store__sentinel_lex_fn;
  language->keyword_lex_fn = (void *)language_module;

  // Store some information about this store's specific instance of this
  // language module, keyed by the language's id.
  array_push(&self->language_instances, ((LanguageWasmInstance) {
    .language_id = language_module->language_id,
    .instance = instance,
    .external_states_address = wasm_language.external_scanner.states,
    .lex_main_fn_index = wasm_language.lex_fn,
    .lex_keyword_fn_index = wasm_language.keyword_lex_fn,
    .scanner_create_fn_index = wasm_language.external_scanner.create,
    .scanner_destroy_fn_index = wasm_language.external_scanner.destroy,
    .scanner_serialize_fn_index = wasm_language.external_scanner.serialize,
    .scanner_deserialize_fn_index = wasm_language.external_scanner.deserialize,
    .scanner_scan_fn_index = wasm_language.external_scanner.scan,
  }));

  return language;
}

bool ts_wasm_store_add_language(
  TSWasmStore *self,
  const TSLanguage *language,
  uint32_t *index
) {
  wasmtime_context_t *context = wasmtime_store_context(self->store);
  const LanguageWasmModule *language_module = (void *)language->keyword_lex_fn;

  // Search for the information about this store's instance of the language module.
  bool exists = false;
  array_search_sorted_by(
    &self->language_instances,
    .language_id,
    language_module->language_id,
    index,
    &exists
  );

  // If the language module has not been instantiated in this store, then add
  // it to this store.
  if (!exists) {
    wasmtime_instance_t instance;
    int32_t language_address;
    if (!ts_wasm_store__instantiate(
      self,
      language_module->module,
      language_module->name,
      &instance,
      &language_address
    )) {
      return false;
    }

    LanguageInWasmMemory wasm_language;
    const uint8_t *memory = wasmtime_memory_data(context, &self->memory);
    memcpy(&wasm_language, &memory[language_address], sizeof(LanguageInWasmMemory));
    array_insert(&self->language_instances, *index, ((LanguageWasmInstance) {
      .language_id = language_module->language_id,
      .instance = instance,
      .external_states_address = wasm_language.external_scanner.states,
      .lex_main_fn_index = wasm_language.lex_fn,
      .lex_keyword_fn_index = wasm_language.keyword_lex_fn,
      .scanner_create_fn_index = wasm_language.external_scanner.create,
      .scanner_destroy_fn_index = wasm_language.external_scanner.destroy,
      .scanner_serialize_fn_index = wasm_language.external_scanner.serialize,
      .scanner_deserialize_fn_index = wasm_language.external_scanner.deserialize,
      .scanner_scan_fn_index = wasm_language.external_scanner.scan,
    }));
  }

  return true;
}

bool ts_wasm_store_start(TSWasmStore *self, TSLexer *lexer, const TSLanguage *language) {
  uint32_t instance_index;
  if (!ts_wasm_store_add_language(self, language, &instance_index)) return false;
  self->current_lexer = lexer;
  self->current_instance = &self->language_instances.contents[instance_index];
  return true;
}

void ts_wasm_store_stop(TSWasmStore *self) {
  self->current_lexer = NULL;
  self->current_instance = NULL;
}

static void ts_wasm_store__call(TSWasmStore *self, int32_t function_index, wasmtime_val_raw_t *args_and_results) {
  wasmtime_context_t *context = wasmtime_store_context(self->store);
  wasmtime_val_t value;
  bool succeeded = wasmtime_table_get(context, &self->function_table, function_index, &value);
  assert(succeeded);
  assert(value.kind == WASMTIME_FUNCREF);
  wasmtime_func_t func = value.of.funcref;

  wasm_trap_t *trap = wasmtime_func_call_unchecked(context, &func, args_and_results);
  if (trap) {
    wasm_message_t message;
    wasm_trap_message(trap, &message);
    printf("error calling function index %u: %s\n", function_index, message.data);
    abort();
  }
}

static bool ts_wasm_store__call_lex_function(TSWasmStore *self, unsigned function_index, TSStateId state) {
  wasmtime_context_t *context = wasmtime_store_context(self->store);
  uint8_t *memory_data = wasmtime_memory_data(context, &self->memory);
  memcpy(
    &memory_data[LEXER_ADDRESS],
    &self->current_lexer->lookahead,
    sizeof(self->current_lexer->lookahead)
  );

  wasmtime_val_raw_t args[2] = {
    {.i32 = LEXER_ADDRESS},
    {.i32 = state},
  };
  ts_wasm_store__call(self, function_index, args);
  bool result = args[0].i32;

  memcpy(
    &self->current_lexer->lookahead,
    &memory_data[LEXER_ADDRESS],
    sizeof(self->current_lexer->lookahead) + sizeof(self->current_lexer->result_symbol)
  );
  return result;
}

bool ts_wasm_store_call_lex_main(TSWasmStore *self, TSStateId state) {
  return ts_wasm_store__call_lex_function(
    self,
    self->current_instance->lex_main_fn_index,
    state
  );
}

bool ts_wasm_store_call_lex_keyword(TSWasmStore *self, TSStateId state) {
  return ts_wasm_store__call_lex_function(
    self,
    self->current_instance->lex_keyword_fn_index,
    state
  );
}

uint32_t ts_wasm_store_call_scanner_create(TSWasmStore *self) {
  wasmtime_val_raw_t args[1] = {{.i32 = 0}};
  ts_wasm_store__call(self, self->current_instance->scanner_create_fn_index, args);
  return args[0].i32;
}

void ts_wasm_store_call_scanner_destroy(TSWasmStore *self, uint32_t scanner_address) {
  wasmtime_val_raw_t args[1] = {{.i32 = scanner_address}};
  ts_wasm_store__call(self, self->current_instance->scanner_destroy_fn_index, args);
}

bool ts_wasm_store_call_scanner_scan(
  TSWasmStore *self,
  uint32_t scanner_address,
  uint32_t valid_tokens_ix
) {
  wasmtime_context_t *context = wasmtime_store_context(self->store);
  uint8_t *memory_data = wasmtime_memory_data(context, &self->memory);

  memcpy(
    &memory_data[LEXER_ADDRESS],
    &self->current_lexer->lookahead,
    sizeof(self->current_lexer->lookahead)
  );

  uint32_t valid_tokens_address =
    self->current_instance->external_states_address +
    (valid_tokens_ix * sizeof(bool));
  wasmtime_val_raw_t args[3] = {
    {.i32 = scanner_address},
    {.i32 = LEXER_ADDRESS},
    {.i32 = valid_tokens_address}
  };
  ts_wasm_store__call(self, self->current_instance->scanner_scan_fn_index, args);

  memcpy(
    &self->current_lexer->lookahead,
    &memory_data[LEXER_ADDRESS],
    sizeof(self->current_lexer->lookahead) + sizeof(self->current_lexer->result_symbol)
  );
  return args[0].i32;
}

uint32_t ts_wasm_store_call_scanner_serialize(
  TSWasmStore *self,
  uint32_t scanner_address,
  char *buffer
) {
  wasmtime_context_t *context = wasmtime_store_context(self->store);
  uint8_t *memory_data = wasmtime_memory_data(context, &self->memory);

  wasmtime_val_raw_t args[2] = {
    {.i32 = scanner_address},
    {.i32 = SERIALIZATION_BUFFER_ADDRESS},
  };
  ts_wasm_store__call(self, self->current_instance->scanner_serialize_fn_index, args);
  uint32_t length = args[0].i32;

  if (length > 0) {
    memcpy(
      ((Lexer *)self->current_lexer)->debug_buffer,
      &memory_data[SERIALIZATION_BUFFER_ADDRESS],
      length
    );
  }
  return length;
}

void ts_wasm_store_call_scanner_deserialize(
  TSWasmStore *self,
  uint32_t scanner_address,
  const char *buffer,
  unsigned length
) {
  wasmtime_context_t *context = wasmtime_store_context(self->store);
  uint8_t *memory_data = wasmtime_memory_data(context, &self->memory);

  if (length > 0) {
    memcpy(
      &memory_data[SERIALIZATION_BUFFER_ADDRESS],
      buffer,
      length
    );
  }

  wasmtime_val_raw_t args[3] = {
    {.i32 = scanner_address},
    {.i32 = SERIALIZATION_BUFFER_ADDRESS},
    {.i32 = length},
  };
  ts_wasm_store__call(self, self->current_instance->scanner_deserialize_fn_index, args);
}

bool ts_language_is_wasm(const TSLanguage *self) {
  return self->lex_fn == ts_wasm_store__sentinel_lex_fn;
}

#else

void ts_wasm_store_delete(TSWasmStore *self) {
  (void)self;
}

bool ts_wasm_store_start(
  TSWasmStore *self,
  TSLexer *lexer,
  const TSLanguage *language
) {
  (void)self;
  (void)lexer;
  (void)language;
  return false;
}

void ts_wasm_store_stop(TSWasmStore *self) {
  (void)self;
}


bool ts_wasm_store_call_lex_main(TSWasmStore *self, TSStateId state) {
  (void)self;
  (void)state;
  return false;
}

bool ts_wasm_store_call_lex_keyword(TSWasmStore *self, TSStateId state) {
  (void)self;
  (void)state;
  return false;
}

uint32_t ts_wasm_store_call_scanner_create(TSWasmStore *self) {
  (void)self;
  return 0;
}

void ts_wasm_store_call_scanner_destroy(
  TSWasmStore *self,
  uint32_t scanner_address
) {
  (void)self;
  (void)scanner_address;
}

bool ts_wasm_store_call_scanner_scan(
  TSWasmStore *self,
  uint32_t scanner_address,
  uint32_t valid_tokens_ix
) {
  (void)self;
  (void)scanner_address;
  (void)valid_tokens_ix;
  return false;
}

uint32_t ts_wasm_store_call_scanner_serialize(
  TSWasmStore *self,
  uint32_t scanner_address,
  char *buffer
) {
  (void)self;
  (void)scanner_address;
  (void)buffer;
  return 0;
}

void ts_wasm_store_call_scanner_deserialize(
  TSWasmStore *self,
  uint32_t scanner_address,
  const char *buffer,
  unsigned length
) {
  (void)self;
  (void)scanner_address;
  (void)buffer;
  (void)length;
}

bool ts_language_is_wasm(const TSLanguage *self) {
  (void)self;
  return false;
}

#endif