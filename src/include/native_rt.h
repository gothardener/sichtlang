#ifndef NATIVE_RT_H
#define NATIVE_RT_H

#include "value.h"

void rt_make_int(Value* out, int v);
void rt_make_float(Value* out, double v);
void rt_make_bool(Value* out, int v);
void rt_make_string(Value* out, const char* s);

void rt_add(Value* out, const Value* a, const Value* b);
void rt_sub(Value* out, const Value* a, const Value* b);
void rt_mul(Value* out, const Value* a, const Value* b);
void rt_div(Value* out, const Value* a, const Value* b);

void rt_eq(Value* out, const Value* a, const Value* b);
void rt_ne(Value* out, const Value* a, const Value* b);
void rt_gt(Value* out, const Value* a, const Value* b);
void rt_lt(Value* out, const Value* a, const Value* b);
void rt_gte(Value* out, const Value* a, const Value* b);
void rt_lte(Value* out, const Value* a, const Value* b);

void rt_and(Value* out, const Value* a, const Value* b);
void rt_or(Value* out, const Value* a, const Value* b);
void rt_not(Value* out, const Value* a);
void rt_neg(Value* out, const Value* a);

void rt_cast_int(Value* out, const Value* v);
void rt_cast_float(Value* out, const Value* v);
void rt_cast_bool(Value* out, const Value* v);
void rt_cast_string(Value* out, const Value* v);

int rt_get_int(const Value* v);
int rt_truthy(const Value* v);

void rt_print_inline(const Value* v);
void rt_print_int(int v);
void rt_print_float(double v);
void rt_print_text(const char* s);
void rt_print_text_repeat(const char* s, int count);
void rt_print_text_repeat_checked(const char* s, int count);
void rt_print_newline(void);

void rt_list_new(Value* out, int count);
void rt_list_set(Value* list_val, const Value* index_val, const Value* value);
void rt_list_set_i32(Value* list_val, int index, const Value* value);
void rt_list_get(Value* out, const Value* list_val, const Value* index_val);
void rt_list_get_i32(Value* out, const Value* list_val, int index);
void rt_list_add(Value* list_val, const Value* value);
void rt_gen_yield(Value* list_val, const Value* value);
void rt_list_remove(Value* list_val, const Value* value);
void rt_list_remove_element(Value* list_val, const Value* index_val);
void rt_list_remove_element_i32(Value* list_val, int index);
void rt_list_clear(Value* list_val);
void rt_list_index_of(Value* out, const Value* list_val, const Value* value);

void rt_dict_new(Value* out, int count);
void rt_dict_set(Value* dict_val, const Value* key, const Value* value);
void rt_dict_get(Value* out, const Value* dict_val, const Value* key);
void rt_dict_remove(Value* dict_val, const Value* key);
void rt_dict_clear(Value* dict_val);
void rt_dict_contains_item(Value* dict_val, const Value* key);

void rt_contains(Value* out, const Value* left, const Value* right);

void rt_input(Value* out, const char* prompt);
void rt_builtin(Value* out, int builtin_type, const Value* arg);
void rt_char_at(Value* out, const Value* str_val, const Value* index_val);
void rt_file_read(Value* out, const Value* path_val);
void rt_file_write(const Value* path_val, const Value* content_val, int append_mode);

void rt_cli_set_args(int argc, const char** argv);
void rt_cli_set_args_from_argv(int argc, const char** argv);
void rt_native_cli_args(Value* out);

void rt_native_io_directory_exists(Value* out, const Value* path_val);
void rt_native_io_create_directory(Value* out, const Value* path_val);
void rt_native_io_list_directories(Value* out, const Value* path_val);
void rt_native_io_list_files(Value* out, const Value* path_val);
void rt_native_io_remove_directory(Value* out, const Value* path_val);
void rt_native_io_delete_file(Value* out, const Value* path_val);
void rt_native_io_copy_file(Value* out, const Value* src_val, const Value* dst_val);
void rt_native_io_copy_directory(Value* out, const Value* src_val, const Value* dst_val);

void rt_native_env_get(Value* out, const Value* name_val);
void rt_native_zip_extract(Value* out, const Value* zip_path, const Value* dest_dir);
void rt_native_process_run(Value* out, const Value* command_val);
void rt_native_process_id(Value* out);

void rt_native_http_get(Value* out, const Value* url, const Value* headers);
  void rt_native_http_post(Value* out, const Value* url, const Value* body, const Value* headers);
  void rt_native_http_request(Value* out, const Value* method, const Value* url, const Value* body, const Value* headers);
  void rt_native_http_download(Value* out, const Value* url, const Value* path, const Value* headers);
  void rt_native_time_ms(Value* out);
  
  void rt_make_generator(Value* out, void* fn_ptr, const Value* args, int arg_count);
int rt_generator_next(Value* out, const Value* gen_val);
int rt_collection_count(const Value* source);
void rt_collection_item(Value* out, const Value* source, int index);

void rt_gc_register_globals(Value* globals, int count);
void rt_gc_push_root(Value* value);
void rt_gc_pop_roots(int count);
void rt_gc_maybe_collect(void);
void rt_step_tick(void);

void rt_llvl_set_gc_paused(int paused);
void rt_llvl_require(void);
void rt_llvl_set_bounds_check(int enabled);
void rt_llvl_set_pointer_checks(int enabled);
void rt_llvl_alloc(Value* out, const Value* size_val);
void rt_llvl_free(Value* buffer_val);
void rt_llvl_resize(Value* buffer_val, const Value* size_val, int is_grow);
void rt_llvl_resize_any(Value* buffer_val, const Value* size_val);
void rt_llvl_copy(Value* out, const Value* src_val);
void rt_llvl_copy_bytes(const Value* src_val, Value* dst_val, const Value* size_val);
void rt_llvl_move(Value* dst_val, Value* src_val);
void rt_llvl_get_value(Value* out, const Value* src_val);
void rt_llvl_set_value(Value* buffer_val, const Value* value_val);
void rt_llvl_get_byte(Value* out, const Value* buffer_val, const Value* index_val);
void rt_llvl_set_byte(Value* buffer_val, const Value* index_val, const Value* value_val);
void rt_llvl_get_bit(Value* out, const Value* buffer_val, const Value* index_val);
void rt_llvl_bit_op(Value* buffer_val, const Value* index_val, int op);
void rt_llvl_place_of(Value* out, const Value* buffer_val);
void rt_llvl_offset(Value* out, const Value* base_val, const Value* offset_val);
void rt_llvl_get_at(Value* out, const Value* addr_val);
void rt_llvl_set_at(const Value* addr_val, const Value* value_val);
void rt_llvl_get_at_typed(Value* out, const Value* addr_val, int type_kind);
void rt_llvl_set_at_typed(const Value* addr_val, const Value* value_val, int type_kind);
void rt_llvl_set_buffer_meta(Value* buffer_val, size_t elem_size, int elem_kind, const char* elem_type_name);
void rt_llvl_register_type(const char* name, int is_union, int field_count, size_t size);
void rt_llvl_register_field(const char* type_name, int field_index, const char* field_name, int kind, int array_len, size_t offset, size_t size);
void rt_llvl_field_get(Value* out, const Value* buffer_val, const char* field_name);
void rt_llvl_field_set(const Value* buffer_val, const char* field_name, const Value* value_val);
void rt_llvl_wait_ms(const Value* ms_val);
void rt_llvl_pin_write(const Value* value_val, const Value* pin_val);
void rt_llvl_pin_read(Value* out, const Value* pin_val);
void rt_llvl_port_write(const Value* port_val, const Value* value_val);
void rt_llvl_port_read(Value* out, const Value* port_val);
void rt_llvl_register_interrupt(const Value* id_val, const char* handler_name);

#endif

