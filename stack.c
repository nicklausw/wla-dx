
#include "flags.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "defines.h"

#include "hashmap.h"
#include "parse.h"
#include "phase_1.h"
#include "stack.h"
#include "include.h"
#include "printf.h"
#include "mersenne.h"


extern int g_input_number_error_msg, g_bankheader_status, g_input_float_mode, g_global_label_hint, g_input_parse_if;
extern int g_source_index, g_source_file_size, g_parsed_int, g_macro_active, g_string_size, g_section_status, g_parse_floats;
extern char g_xyz[512], *g_buffer, *g_tmp, g_expanded_macro_string[256], g_label[MAX_NAME_LENGTH + 1];
extern struct map_t *g_defines_map;
extern struct active_file_info *g_active_file_info_first, *g_active_file_info_last, *g_active_file_info_tmp;
extern struct macro_runtime *g_macro_runtime_current;
extern struct section_def *g_sec_tmp;
extern struct export_def *g_export_first;
extern double g_parsed_double;
extern unsigned char g_asciitable[256];
extern int g_operand_hint, g_operand_hint_type, g_can_calculate_a_minus_b, g_expect_calculations, g_asciitable_defined;

int g_latest_stack = 0, g_last_stack_id = 0, g_resolve_stack_calculations = YES, g_stack_calculations_max = 0;
int g_parsing_function_body = NO, g_fail_quetly_on_non_found_functions = NO;
struct stack **g_stack_calculations = NULL;

static int g_delta_counter = 0, g_delta_section = -1, g_delta_address = -1;
static struct stack_item *g_delta_old_pointer = NULL;

static int s_dsp_file_name_id = 0, s_dsp_line_number = 0;

static int _resolve_string(struct stack_item *s, int *cannot_resolve);

PROFILE_GLOBALS_EXTERN();


struct stack *allocate_struct_stack(int items) {

  struct stack *stack = calloc(sizeof(struct stack), 1);
  if (stack == NULL) {
    print_error(ERROR_STC, "Out of memory error while allocating room for a new calculation stack.\n");
    return NULL;
  }

  init_stack_struct(stack);

  stack->stacksize = items;
  stack->stack_items = calloc(sizeof(struct stack_item) * items, 1);
  if (stack->stack_items == NULL) {
    free(stack);
    print_error(ERROR_STC, "Out of memory error while allocating room for a new calculation stack.\n");
    return NULL;
  }

  return stack;
}


void init_stack_struct(struct stack *s) {

  s->stack_items = NULL;
  s->id = -123456;
  s->compressed_id = -123456;
  s->position = STACK_POSITION_DEFINITION;
  s->filename_id = -123456;
  s->stacksize = 0;
  s->linenumber = -123456;
  s->type = STACK_TYPE_UNKNOWN;
  s->bank = -123456;
  s->slot = -123456;
  s->relative_references = 0;
  s->base = -123456;
  /* NOTE! section_status is not really set anywhere, but phase_4.c uses it -> investigate */
  s->section_status = 0;
  s->section_id = -123456;
  s->address = -123456;
  s->special_id = 0;
  s->bits_position = 0;
  s->bits_to_define = 0;
  s->is_function_body = NO;
  s->is_bankheader_section = NO;
  s->is_single_instance = NO;
  s->has_been_calculated = NO;
  s->value = 0.0;
}


int calculation_stack_insert(struct stack *s) {

  if (g_bankheader_status == OFF) {
    /* outside bankheader sections */
    s->is_bankheader_section = NO;    
  }
  else {
    /* inside a bankheader section */
    s->is_bankheader_section = YES;
  }

  s->id = g_last_stack_id;
  s->section_status = g_section_status;
  if (g_section_status == ON)
    s->section_id = g_sec_tmp->id;
  else
    s->section_id = 0;

  g_latest_stack = g_last_stack_id;
  g_last_stack_id++;

  if (g_latest_stack >= g_stack_calculations_max) {
    /* enlarge the pointer array! */
    g_stack_calculations_max += 4096;

    g_stack_calculations = realloc(g_stack_calculations, sizeof(struct stack *) * g_stack_calculations_max);
    if (g_stack_calculations == NULL) {
      print_error(ERROR_NUM, "Out of memory error while trying to enlarge stack calculations pointer array!\n");
      return FAILED;
    }
  }

  g_stack_calculations[g_latest_stack] = s;

  return SUCCEEDED;
}


void free_stack_calculations(void) {

  int i;

  for (i = 0; i < g_last_stack_id; i++) {
    if (g_stack_calculations[i] != NULL)
      delete_stack_calculation_struct(g_stack_calculations[i]);
  }

  free(g_stack_calculations);

  g_stack_calculations = NULL;
  g_latest_stack = 0;
  g_last_stack_id = 0;
  g_stack_calculations_max = 0;
}


void delete_stack_calculation_struct(struct stack *s) {

  if (s == NULL)
    print_error(ERROR_WRN, "Deleting a non-existing computation stack! Please submit a bug report!\n");
  else {
    g_stack_calculations[s->id] = NULL;

    free(s->stack_items);
    free(s);
  }
}


struct stack *find_stack_calculation(int id, int print_error_message) {

  struct stack *s;
  
  if (id < 0 || id >= g_last_stack_id) {
    if (print_error_message == YES)
      print_error(ERROR_NUM, "Stack calculation %d is out of bounds [0, %d]! Please submit a bug report!\n", id, g_last_stack_id);
    return NULL;
  }

  s = g_stack_calculations[id];

  if (s == NULL) {
    if (print_error_message == YES)
      print_error(ERROR_NUM, "Stack calculation %d has gone missing! Please submit a bug report!\n", id);
  }

  return s;
}


int compress_stack_calculation_ids(void) {

  struct stack **stack_calculations = NULL;
  struct export_def *export_tmp;
  int i, compressed_id = 0;
  
  /* 1. give stack calculations new IDs */

  /* pending calculations we'll export come first */
  for (i = 0; i < g_last_stack_id; i++) {
    struct stack *s = g_stack_calculations[i];

    if (s == NULL)
      continue;
    if (s->is_function_body == YES)
      continue;

    s->compressed_id = compressed_id++;
  }

  /* next come the pending calculations we will not export */
  for (i = 0; i < g_last_stack_id; i++) {
    struct stack *s = g_stack_calculations[i];

    if (s == NULL)
      continue;
    if (s->is_function_body == NO)
      continue;

    s->compressed_id = compressed_id++;
  }

  /* 2. reorder the stack calculations into a new pointer array */

  if (g_stack_calculations_max > 0) {
    stack_calculations = calloc(sizeof(struct stack *) * g_stack_calculations_max, 1);
    if (stack_calculations == NULL) {
      print_error(ERROR_NUM, "Out of memory error while trying to reorder stack calculations pointer array! g_stack_calculations_max = %d!\n", g_stack_calculations_max);
      return FAILED;
    }

    for (i = 0; i < g_last_stack_id; i++) {
      struct stack *s = g_stack_calculations[i];

      if (s == NULL)
        continue;

      stack_calculations[s->compressed_id] = s;
    }
  }
  
  /* 3. update all stack calculation IDs in the stack calculations */

  for (i = 0; i < g_last_stack_id; i++) {
    struct stack *s = g_stack_calculations[i];
    int j;

    if (s == NULL)
      continue;

    for (j = 0; j < s->stacksize; j++) {
      if (s->stack_items[j].type == STACK_ITEM_TYPE_STACK) {
        struct stack *s2 = g_stack_calculations[(int)s->stack_items[j].value];

        s->stack_items[j].value = (double)s2->compressed_id;
      }
    }
  }

  /* 4. update all definitions that contain stack calculation IDs */

  export_tmp = g_export_first;
  while (export_tmp != NULL) {
    struct definition *tmp_def;
    
    hashmap_get(g_defines_map, export_tmp->name, (void*)&tmp_def);
    if (tmp_def != NULL) {
      if (tmp_def->type == DEFINITION_TYPE_STACK) {
        struct stack *s = g_stack_calculations[(int)tmp_def->value];

        if (s != NULL)
          tmp_def->value = (double)s->compressed_id;
      }
    }

    export_tmp = export_tmp->next;
  }
  
  /* 5. delete the old array, replace it with the new array */

  free(g_stack_calculations);
  g_stack_calculations = stack_calculations;

  /* 6. finalize the ID changes */

  for (i = 0; i < g_last_stack_id; i++) {
    struct stack *s = g_stack_calculations[i];

    if (s != NULL)
      s->id = s->compressed_id;
  }
  
  return SUCCEEDED;
}


static int _break_before_value_or_string(int i, struct stack_item *si) {

  /* we use this function to test if the previous item in the stack
     is something that cannot be followed by a value or a string.
     in such a case we'll stop adding items to this stack computation */
  
  if (i <= 0)
    return FAILED;

  si = &si[i-1];
  if (si->type == STACK_ITEM_TYPE_VALUE)
    return SUCCEEDED;
  if (si->type == STACK_ITEM_TYPE_STRING)
    return SUCCEEDED;
  if (si->type == STACK_ITEM_TYPE_LABEL)
    return SUCCEEDED;
  if (si->type == STACK_ITEM_TYPE_OPERATOR && si->value == SI_OP_RIGHT)
    return SUCCEEDED;

  return FAILED;
}

#ifdef WLA_DEBUG
void debug_print_stack(int line_number, int stack_id, struct stack_item *ta, int count, int id, struct stack *stack) {

  int k;
  
  printf("LINE %5d: ID = %d (STACK) CALCULATION ID = %d (c%d) ", line_number, id, stack_id, stack_id);
  if (stack == NULL)
    printf("FB?: ");
  else if (stack->is_function_body == YES)
    printf("FBy: ");
  else
    printf("FBn: ");
  
  for (k = 0; k < count; k++) {
    char ar[] = "+-*()|&/^01%~<>!:<>";
    int add_sign = YES;

    if (ta[k].type == STACK_ITEM_TYPE_DELETED)
      continue;

    if (ta[k].type == STACK_ITEM_TYPE_OPERATOR) {
      int value = (int)ta[k].value;

      if (!(value == SI_OP_ROUND ||
            value == SI_OP_CEIL ||
            value == SI_OP_FLOOR ||
            value == SI_OP_MIN ||
            value == SI_OP_MAX ||
            value == SI_OP_SQRT ||
            value == SI_OP_COS ||
            value == SI_OP_SIN ||
            value == SI_OP_TAN ||
            value == SI_OP_ACOS ||
            value == SI_OP_ASIN ||
            value == SI_OP_ATAN ||
            value == SI_OP_ATAN2 ||
            value == SI_OP_COSH ||
            value == SI_OP_SINH ||
            value == SI_OP_TANH ||
            value == SI_OP_LOG ||
            value == SI_OP_LOG10 ||
            value == SI_OP_POW ||
            value == SI_OP_LOW_BYTE ||
            value == SI_OP_HIGH_BYTE ||
            value == SI_OP_LOW_WORD ||
            value == SI_OP_HIGH_WORD ||
            value == SI_OP_BANK_BYTE ||
            value == SI_OP_BANK ||
            value == SI_OP_CLAMP ||
            value == SI_OP_SIGN ||
            value == SI_OP_ABS))
        add_sign = NO;
    }
    
    if (add_sign == YES) {
      if (ta[k].sign == SI_SIGN_POSITIVE)
        printf("+");
      else
        printf("-");
    }
    
    if (ta[k].can_calculate_deltas == YES)
      printf("@");
    
    if (ta[k].type == STACK_ITEM_TYPE_OPERATOR) {
      int value = (int)ta[k].value;

      if (value == SI_OP_SHIFT_LEFT)
        printf("<<");
      else if (value == SI_OP_SHIFT_RIGHT)
        printf(">>");
      else if (value == SI_OP_COMPARE_EQ)
        printf("==");
      else if (value == SI_OP_COMPARE_NEQ)
        printf("!=");
      else if (value == SI_OP_COMPARE_LTE)
        printf("<=");
      else if (value == SI_OP_COMPARE_GTE)
        printf(">=");
      else if (value == SI_OP_LOGICAL_OR)
        printf("||");
      else if (value == SI_OP_LOGICAL_AND)
        printf("&&");
      else if (value == SI_OP_LOW_WORD)
        printf("loword(a)");
      else if (value == SI_OP_HIGH_WORD)
        printf("hiword(a)");
      else if (value == SI_OP_BANK_BYTE)
        printf("bankbyte(a)");
      else if (value == SI_OP_ROUND)
        printf("round(a)");
      else if (value == SI_OP_CEIL)
        printf("ceil(a)");
      else if (value == SI_OP_FLOOR)
        printf("floor(a)");
      else if (value == SI_OP_MIN)
        printf("min(a,b)");
      else if (value == SI_OP_MAX)
        printf("max(a,b)");
      else if (value == SI_OP_SQRT)
        printf("sqrt(a)");
      else if (value == SI_OP_ABS)
        printf("abs(a)");
      else if (value == SI_OP_COS)
        printf("cos(a)");
      else if (value == SI_OP_SIN)
        printf("sin(a)");
      else if (value == SI_OP_TAN)
        printf("tan(a)");
      else if (value == SI_OP_ACOS)
        printf("acos(a)");
      else if (value == SI_OP_ASIN)
        printf("asin(a)");
      else if (value == SI_OP_ATAN)
        printf("atan(a)");
      else if (value == SI_OP_ATAN2)
        printf("atan2(a,b)");
      else if (value == SI_OP_NEGATE)
        printf("negate(a)");
      else if (value == SI_OP_COSH)
        printf("cosh(a)");
      else if (value == SI_OP_SINH)
        printf("sinh(a)");
      else if (value == SI_OP_TANH)
        printf("tanh(a)");
      else if (value == SI_OP_LOG)
        printf("log(a)");
      else if (value == SI_OP_LOG10)
        printf("log10(a)");
      else if (value == SI_OP_POW)
        printf("pow(a,b)");
      else if (value == SI_OP_CLAMP)
        printf("pow(v,min,max)");
      else if (value == SI_OP_SIGN)
        printf("sign(a)");
      else {
        if (value >= (int)strlen(ar)) {
          printf("ERROR!\n");
          printf("debug_print_stack(): ERROR: Unhandled SI_OP_* (%d)! Please submit a bug report!\n", value);
          exit(1);
        }
        printf("%c", ar[value]);
      }
    }
    else if (ta[k].type == STACK_ITEM_TYPE_VALUE)
      printf("V(%f)", ta[k].value);
    else if (ta[k].type == STACK_ITEM_TYPE_STACK)
      printf("C(%d)", (int)ta[k].value);
    else if (ta[k].type == STACK_ITEM_TYPE_STRING || ta[k].type == STACK_ITEM_TYPE_LABEL)
      printf("S(%s)", ta[k].string);
    else
      printf("?");

    if (k < count-1)
      printf(", ");
  }
  printf("\n");
}
#endif


int get_label_length(char *l) {

  struct definition *tmp_def;
  int length;
  
  hashmap_get(g_defines_map, l, (void*)&tmp_def);

  if (tmp_def != NULL) {
    if (tmp_def->type == DEFINITION_TYPE_STRING)
      return (int)strlen(tmp_def->string);
    else {
      print_error(ERROR_NUM, "Definition \"%s\" is not a string definition. .length returns 0 for that...\n", l);
      return 0;
    }
  }

  length = (int)strlen(l);

  if (l[0] == '"' && l[length-1] == '"')
    length -= 2;

  return length;
}


static struct stack_item_priority_item g_stack_item_priority_items[] = {
  { SI_OP_LOGICAL_OR, 10 },
  { SI_OP_LOGICAL_AND, 20 },
  { SI_OP_OR, 30 },
  { SI_OP_XOR, 40 },
  { SI_OP_AND, 50 },
  { SI_OP_COMPARE_EQ, 60 },
  { SI_OP_COMPARE_NEQ, 60 },
  { SI_OP_COMPARE_LT, 70 },
  { SI_OP_COMPARE_GT, 70 },
  { SI_OP_COMPARE_LTE, 70 },
  { SI_OP_COMPARE_GTE, 70 },
  { SI_OP_SHIFT_LEFT, 80 },
  { SI_OP_SHIFT_RIGHT, 80 },
  { SI_OP_ADD, 90 },
  { SI_OP_SUB, 90 },
  { SI_OP_NEGATE, 100 },
  { SI_OP_MULTIPLY, 100 },
  { SI_OP_DIVIDE, 100 },
  { SI_OP_MODULO, 100 },
  { SI_OP_POWER, 100 },
  { SI_OP_LOW_BYTE, 110 },
  { SI_OP_HIGH_BYTE, 110 },
  { SI_OP_LOW_WORD, 110 },
  { SI_OP_HIGH_WORD, 110 },
  { SI_OP_BANK, 110 },
  { SI_OP_BANK_BYTE, 110 },
  { SI_OP_ROUND, 110 },
  { SI_OP_CEIL, 110 },
  { SI_OP_FLOOR, 110 },
  { SI_OP_MIN, 110 },
  { SI_OP_MAX, 110 },
  { SI_OP_SQRT, 110 },
  { SI_OP_ABS, 110 },
  { SI_OP_COS, 110 },
  { SI_OP_SIN, 110 },
  { SI_OP_TAN, 110 },
  { SI_OP_COSH, 110 },
  { SI_OP_SINH, 110 },
  { SI_OP_TANH, 110 },
  { SI_OP_ACOS, 110 },
  { SI_OP_ASIN, 110 },
  { SI_OP_ATAN, 110 },
  { SI_OP_ATAN2, 110 },
  { SI_OP_LOG, 110 },
  { SI_OP_LOG10, 110 },
  { SI_OP_POW, 110 },
  { SI_OP_CLAMP, 110 },
  { SI_OP_SIGN, 110 },
  { SI_OP_NOT, 120 },
  { 999, 999 }
};


static int _get_op_priority(int op) {

  int i = 0;
  
  while (g_stack_item_priority_items[i].op < 999) {
    if (g_stack_item_priority_items[i].op == op)
      return g_stack_item_priority_items[i].priority;
    i++;
  }

  fprintf(stderr, "_get_op_priority(): No priority for OP %d! Please submit a bug report\n", op);

  return 0;
}


static int _parse_function_asc(char *in, int *result, int *parsed_chars) {

  int res, old_expect = g_expect_calculations, source_index_original = g_source_index, source_index_backup;
  
  /* NOTE! we assume that 'in' is actually '&g_buffer[xyz]', so
     let's update g_source_index for input_number() */

  g_source_index = (int)(in - g_buffer);
  source_index_backup = g_source_index;

  g_expect_calculations = NO;
  res = input_number();
  g_expect_calculations = old_expect;

  if (res != SUCCEEDED || g_parsed_int < 0 || g_parsed_int > 255) {
    print_error(ERROR_NUM, "asc() requires an immediate value between 0 and 255.\n");
    return FAILED;
  }
  
  if (g_buffer[g_source_index] != ')') {
    print_error(ERROR_NUM, "Malformed \"asc(?)\" detected!\n");
    return FAILED;
  }

  /* skip ')' */
  g_source_index++;

  /* count the parsed chars */
  *parsed_chars = (int)(g_source_index - source_index_backup);

  /* return g_source_index */
  g_source_index = source_index_original;
  
  if (g_asciitable_defined == 0) {
    print_error(ERROR_WRN, "No .ASCIITABLE defined. Using the default n->n -mapping.\n");
    *result = g_parsed_int;
  }
  else
    *result = (int)g_asciitable[g_parsed_int];

  return SUCCEEDED;
}


static int _parse_function_random(char *in, int *result, int *parsed_chars) {

  int res, old_expect = g_expect_calculations, source_index_original = g_source_index, source_index_backup;
  int min, max;
  
  /* NOTE! we assume that 'in' is actually '&g_buffer[xyz]', so
     let's update g_source_index for input_number() */

  g_source_index = (int)(in - g_buffer);
  source_index_backup = g_source_index;

  g_expect_calculations = YES;
  res = input_number();

  if (res != SUCCEEDED) {
    print_error(ERROR_NUM, "random() requires an immediate value for min.\n");
    return FAILED;
  }

  min = g_parsed_int;

  g_expect_calculations = YES;
  res = input_number();
  g_expect_calculations = old_expect;

  if (res != SUCCEEDED) {
    print_error(ERROR_NUM, "random() requires an immediate value for max.\n");
    return FAILED;
  }

  max = g_parsed_int;

  if (min >= max) {
    print_error(ERROR_DIR, "random() needs that min < max.\n");
    return FAILED;
  }
  
  if (g_buffer[g_source_index] != ')') {
    print_error(ERROR_NUM, "Malformed \"random(?,?)\" detected!\n");
    return FAILED;
  }

  /* skip ')' */
  g_source_index++;

  /* count the parsed chars */
  *parsed_chars = (int)(g_source_index - source_index_backup);

  /* return g_source_index */
  g_source_index = source_index_original;

  /* output the random number */
  *result = (genrand_int32() % (max-min+1)) + min;

  return SUCCEEDED;
}


static int _parse_function_defined(char *in, int *result, int *parsed_chars) {

  int res, old_expect = g_expect_calculations, source_index_original = g_source_index, source_index_backup;
  struct definition *d;

  /* NOTE! we assume that 'in' is actually '&g_buffer[xyz]', so
     let's update g_source_index for input_number() */

  g_source_index = (int)(in - g_buffer);
  source_index_backup = g_source_index;

  g_expect_calculations = NO;
  res = get_next_plain_string();
  g_expect_calculations = old_expect;

  if (res != SUCCEEDED) {
    print_error(ERROR_NUM, "defined() requires a definition name string.\n");
    return FAILED;
  }
  
  if (g_buffer[g_source_index] != ')') {
    print_error(ERROR_NUM, "Malformed \"defined(?)\" detected!\n");
    return FAILED;
  }

  /* skip ')' */
  g_source_index++;

  /* count the parsed chars */
  *parsed_chars = (int)(g_source_index - source_index_backup);

  /* return g_source_index */
  g_source_index = source_index_original;

  /* try to find the definition */
  hashmap_get(g_defines_map, g_label, (void*)&d);

  if (d != NULL)
    *result = 1;
  else
    *result = 0;
  
  return SUCCEEDED;
}


static int _parse_function_exists(char *in, int *result, int *parsed_chars) {

  int res, old_expect = g_expect_calculations, source_index_original = g_source_index, source_index_backup;
  FILE *f;
  
  /* NOTE! we assume that 'in' is actually '&g_buffer[xyz]', so
     let's update g_source_index for input_number() */

  g_source_index = (int)(in - g_buffer);
  source_index_backup = g_source_index;

  g_expect_calculations = NO;
  res = input_number();
  g_expect_calculations = old_expect;

  if (res != INPUT_NUMBER_ADDRESS_LABEL && res != INPUT_NUMBER_STRING) {
    print_error(ERROR_NUM, "exists() requires a file name string.\n");
    return FAILED;
  }
  
  if (g_buffer[g_source_index] != ')') {
    print_error(ERROR_NUM, "Malformed \"exists(?)\" detected!\n");
    return FAILED;
  }

  /* skip ')' */
  g_source_index++;

  /* count the parsed chars */
  *parsed_chars = (int)(g_source_index - source_index_backup);

  /* return g_source_index */
  g_source_index = source_index_original;
  
  f = fopen(g_label, "rb");
  if (f == NULL)
    *result = 0;
  else {
    *result = 1;

    fclose(f);
  }
  
  return SUCCEEDED;
}


static int _parse_function_math1(char *in, int *type, double *value, char *string, int *parsed_chars, char *name) {

  int res, source_index_original = g_source_index, source_index_backup, input_float_mode = g_input_float_mode;
  
  /* NOTE! we assume that 'in' is actually '&g_buffer[xyz]', so
     let's update g_source_index for input_number() */

  g_source_index = (int)(in - g_buffer);
  source_index_backup = g_source_index;

  if (g_buffer[g_source_index] == ')') {
    print_error(ERROR_STC, "%s is missing argument 1!\n", name);
    return FAILED;
  }
  
  g_input_float_mode = ON;
  res = input_number();
  while (res == INPUT_NUMBER_EOL) {
    next_line();
    res = input_number();
  }
  g_input_float_mode = input_float_mode;

  *type = res;
  if (res == SUCCEEDED)
    *value = g_parsed_int;
  else if (res == INPUT_NUMBER_FLOAT)
    *value = g_parsed_double;
  else if (res == INPUT_NUMBER_ADDRESS_LABEL)
    strcpy(string, g_label);
  else if (res == INPUT_NUMBER_STACK)
    *value = g_latest_stack;
  else if (res == FAILED)
    return FAILED;
  else {
    print_error(ERROR_STC, "Unhandled result type %d of argument 1 in %s!\n", res, name);
    return FAILED;
  }

  if (g_buffer[g_source_index] != ')') {
    print_error(ERROR_NUM, "Malformed \"%s\" detected!\n", name);
    return FAILED;
  }

  /* skip ')' */
  g_source_index++;

  /* count the parsed chars */
  *parsed_chars = (int)(g_source_index - source_index_backup);

  /* return g_source_index */
  g_source_index = source_index_original;
  
  return SUCCEEDED;
}


static int _parse_function_math2(char *in, int *type_a, int *type_b, double *value_a, double *value_b, char *string_a, char *string_b, int *parsed_chars, char *name) {

  int res, source_index_original = g_source_index, source_index_backup, input_float_mode = g_input_float_mode;
  
  /* NOTE! we assume that 'in' is actually '&g_buffer[xyz]', so
     let's update g_source_index for input_number() */

  g_source_index = (int)(in - g_buffer);
  source_index_backup = g_source_index;

  /* a */
  g_input_float_mode = ON;
  res = input_number();
  while (res == INPUT_NUMBER_EOL) {
    next_line();
    res = input_number();
  }
  
  *type_a = res;
  if (res == SUCCEEDED)
    *value_a = g_parsed_int;
  else if (res == INPUT_NUMBER_FLOAT)
    *value_a = g_parsed_double;
  else if (res == INPUT_NUMBER_ADDRESS_LABEL)
    strcpy(string_a, g_label);
  else if (res == INPUT_NUMBER_STACK)
    *value_a = g_latest_stack;
  else if (res == FAILED)
    return FAILED;
  else {
    print_error(ERROR_STC, "Unhandled result type %d of argument 1 in %s!\n", res, name);
    return FAILED;
  }

  if (g_buffer[g_source_index] == ')') {
    print_error(ERROR_STC, "%s is missing argument 2!\n", name);
    return FAILED;
  }
  
  /* b */
  res = input_number();
  while (res == INPUT_NUMBER_EOL) {
    next_line();
    res = input_number();
  }
  g_input_float_mode = input_float_mode;

  *type_b = res;
  if (res == SUCCEEDED)
    *value_b = g_parsed_int;
  else if (res == INPUT_NUMBER_FLOAT)
    *value_b = g_parsed_double;
  else if (res == INPUT_NUMBER_ADDRESS_LABEL)
    strcpy(string_b, g_label);
  else if (res == INPUT_NUMBER_STACK)
    *value_b = g_latest_stack;
  else if (res == FAILED)
    return FAILED;
  else {
    print_error(ERROR_STC, "Unhandled result type %d of argument 2 in %s!\n", res, name);
    return FAILED;
  }
  
  if (g_buffer[g_source_index] != ')') {
    print_error(ERROR_NUM, "Malformed \"%s\" detected!\n", name);
    return FAILED;
  }

  /* skip ')' */
  g_source_index++;

  /* count the parsed chars */
  *parsed_chars = (int)(g_source_index - source_index_backup);

  /* return g_source_index */
  g_source_index = source_index_original;
  
  return SUCCEEDED;
}


static int _parse_function_math3(char *in, int *type_a, int *type_b, int *type_c, double *value_a, double *value_b, double *value_c, char *string_a, char *string_b, char *string_c, int *parsed_chars, char *name) {

  int res, source_index_original = g_source_index, source_index_backup, input_float_mode = g_input_float_mode;
  
  /* NOTE! we assume that 'in' is actually '&g_buffer[xyz]', so
     let's update g_source_index for input_number() */

  g_source_index = (int)(in - g_buffer);
  source_index_backup = g_source_index;

  /* a */
  g_input_float_mode = ON;
  res = input_number();
  while (res == INPUT_NUMBER_EOL) {
    next_line();
    res = input_number();
  }
  
  *type_a = res;
  if (res == SUCCEEDED)
    *value_a = g_parsed_int;
  else if (res == INPUT_NUMBER_FLOAT)
    *value_a = g_parsed_double;
  else if (res == INPUT_NUMBER_ADDRESS_LABEL)
    strcpy(string_a, g_label);
  else if (res == INPUT_NUMBER_STACK)
    *value_a = g_latest_stack;
  else if (res == FAILED)
    return FAILED;
  else {
    print_error(ERROR_STC, "Unhandled result type %d of argument 1 in %s!\n", res, name);
    return FAILED;
  }

  if (g_buffer[g_source_index] == ')') {
    print_error(ERROR_STC, "%s is missing argument 2!\n", name);
    return FAILED;
  }
  
  /* b */
  res = input_number();
  while (res == INPUT_NUMBER_EOL) {
    next_line();
    res = input_number();
  }
  g_input_float_mode = input_float_mode;

  *type_b = res;
  if (res == SUCCEEDED)
    *value_b = g_parsed_int;
  else if (res == INPUT_NUMBER_FLOAT)
    *value_b = g_parsed_double;
  else if (res == INPUT_NUMBER_ADDRESS_LABEL)
    strcpy(string_b, g_label);
  else if (res == INPUT_NUMBER_STACK)
    *value_b = g_latest_stack;
  else if (res == FAILED)
    return FAILED;
  else {
    print_error(ERROR_STC, "Unhandled result type %d of argument 2 in %s!\n", res, name);
    return FAILED;
  }

  if (g_buffer[g_source_index] == ')') {
    print_error(ERROR_STC, "%s is missing argument 3!\n", name);
    return FAILED;
  }
  
  /* c */
  res = input_number();
  while (res == INPUT_NUMBER_EOL) {
    next_line();
    res = input_number();
  }
  g_input_float_mode = input_float_mode;

  *type_c = res;
  if (res == SUCCEEDED)
    *value_c = g_parsed_int;
  else if (res == INPUT_NUMBER_FLOAT)
    *value_c = g_parsed_double;
  else if (res == INPUT_NUMBER_ADDRESS_LABEL)
    strcpy(string_c, g_label);
  else if (res == INPUT_NUMBER_STACK)
    *value_c = g_latest_stack;
  else if (res == FAILED)
    return FAILED;
  else {
    print_error(ERROR_STC, "Unhandled result type %d of argument 3 in %s!\n", res, name);
    return FAILED;
  }
  
  if (g_buffer[g_source_index] != ')') {
    print_error(ERROR_NUM, "Malformed \"%s\" detected!\n", name);
    return FAILED;
  }

  /* skip ')' */
  g_source_index++;

  /* count the parsed chars */
  *parsed_chars = (int)(g_source_index - source_index_backup);

  /* return g_source_index */
  g_source_index = source_index_original;
  
  return SUCCEEDED;
}


static int _parse_function_math1_base(char **in, struct stack_item *si, int *q, char *name, int operator) {

  int parsed_chars = 0, type = -1;
  char string[MAX_NAME_LENGTH + 1];
  double value = 0.0;

  if (_parse_function_math1(*in, &type, &value, string, &parsed_chars, name) == FAILED)
    return FAILED;
  *in += parsed_chars;

  si[*q].type = STACK_ITEM_TYPE_OPERATOR;
  si[*q].value = operator;
  si[*q].sign = SI_SIGN_POSITIVE;

  (*q)++;

  if ((*q)+1 >= MAX_STACK_CALCULATOR_ITEMS-1) {
    print_error(ERROR_STC, "Out of stack space. Adjust MAX_STACK_CALCULATOR_ITEMS in defines.h and recompile WLA!\n");
    return FAILED;
  }

  si[*q].sign = SI_SIGN_POSITIVE;
  if (type == SUCCEEDED || type == INPUT_NUMBER_FLOAT) {
    si[*q].type = STACK_ITEM_TYPE_VALUE;
    si[*q].value = value;
  }
  else if (type == INPUT_NUMBER_ADDRESS_LABEL) {
    si[*q].type = STACK_ITEM_TYPE_LABEL;
    strcpy(si[*q].string, string);
  }
  else if (type == INPUT_NUMBER_STACK) {
    si[*q].type = STACK_ITEM_TYPE_STACK;
    si[*q].value = value;
  }

  return SUCCEEDED;
}


static int _parse_function_math2_base(char **in, struct stack_item *si, int *q, char *name, int operator) {

  int parsed_chars = 0, type_a = -1, type_b = -1;
  char string_a[MAX_NAME_LENGTH + 1], string_b[MAX_NAME_LENGTH + 1];
  double value_a = 0, value_b = 0;

  if (_parse_function_math2(*in, &type_a, &type_b, &value_a, &value_b, string_a, string_b, &parsed_chars, name) == FAILED)
    return FAILED;
  *in += parsed_chars;

  si[*q].type = STACK_ITEM_TYPE_OPERATOR;
  si[*q].value = operator;
  si[*q].sign = SI_SIGN_POSITIVE;
  
  (*q)++;

  if (*q+2 >= MAX_STACK_CALCULATOR_ITEMS-1) {
    print_error(ERROR_STC, "Out of stack space. Adjust MAX_STACK_CALCULATOR_ITEMS in defines.h and recompile WLA!\n");
    return FAILED;
  }

  si[*q].sign = SI_SIGN_POSITIVE;
  if (type_a == SUCCEEDED || type_a == INPUT_NUMBER_FLOAT) {
    si[*q].type = STACK_ITEM_TYPE_VALUE;
    si[*q].value = value_a;
  }
  else if (type_a == INPUT_NUMBER_ADDRESS_LABEL) {
    si[*q].type = STACK_ITEM_TYPE_LABEL;
    strcpy(si[*q].string, string_a);
  }
  else if (type_a == INPUT_NUMBER_STACK) {
    si[*q].type = STACK_ITEM_TYPE_STACK;
    si[*q].value = value_a;
  }

  (*q)++;

  si[*q].sign = SI_SIGN_POSITIVE;
  if (type_b == SUCCEEDED || type_b == INPUT_NUMBER_FLOAT) {
    si[*q].type = STACK_ITEM_TYPE_VALUE;
    si[*q].value = value_b;
  }
  else if (type_b == INPUT_NUMBER_ADDRESS_LABEL) {
    si[*q].type = STACK_ITEM_TYPE_LABEL;
    strcpy(si[*q].string, string_b);
  }
  else if (type_b == INPUT_NUMBER_STACK) {
    si[*q].type = STACK_ITEM_TYPE_STACK;
    si[*q].value = value_b;
  }

  return SUCCEEDED;
}


static int _parse_function_math3_base(char **in, struct stack_item *si, int *q, char *name, int operator) {

  int parsed_chars = 0, type_a = -1, type_b = -1, type_c = -1;
  char string_a[MAX_NAME_LENGTH + 1], string_b[MAX_NAME_LENGTH + 1], string_c[MAX_NAME_LENGTH + 1];
  double value_a = 0, value_b = 0, value_c = 0;

  if (_parse_function_math3(*in, &type_a, &type_b, &type_c, &value_a, &value_b, &value_c, string_a, string_b, string_c, &parsed_chars, name) == FAILED)
    return FAILED;
  *in += parsed_chars;

  si[*q].type = STACK_ITEM_TYPE_OPERATOR;
  si[*q].value = operator;
  si[*q].sign = SI_SIGN_POSITIVE;
  
  (*q)++;

  if (*q+3 >= MAX_STACK_CALCULATOR_ITEMS-1) {
    print_error(ERROR_STC, "Out of stack space. Adjust MAX_STACK_CALCULATOR_ITEMS in defines.h and recompile WLA!\n");
    return FAILED;
  }

  si[*q].sign = SI_SIGN_POSITIVE;
  if (type_a == SUCCEEDED || type_a == INPUT_NUMBER_FLOAT) {
    si[*q].type = STACK_ITEM_TYPE_VALUE;
    si[*q].value = value_a;
  }
  else if (type_a == INPUT_NUMBER_ADDRESS_LABEL) {
    si[*q].type = STACK_ITEM_TYPE_LABEL;
    strcpy(si[*q].string, string_a);
  }
  else if (type_a == INPUT_NUMBER_STACK) {
    si[*q].type = STACK_ITEM_TYPE_STACK;
    si[*q].value = value_a;
  }

  (*q)++;

  si[*q].sign = SI_SIGN_POSITIVE;
  if (type_b == SUCCEEDED || type_b == INPUT_NUMBER_FLOAT) {
    si[*q].type = STACK_ITEM_TYPE_VALUE;
    si[*q].value = value_b;
  }
  else if (type_b == INPUT_NUMBER_ADDRESS_LABEL) {
    si[*q].type = STACK_ITEM_TYPE_LABEL;
    strcpy(si[*q].string, string_b);
  }
  else if (type_b == INPUT_NUMBER_STACK) {
    si[*q].type = STACK_ITEM_TYPE_STACK;
    si[*q].value = value_b;
  }

  (*q)++;

  si[*q].sign = SI_SIGN_POSITIVE;
  if (type_c == SUCCEEDED || type_c == INPUT_NUMBER_FLOAT) {
    si[*q].type = STACK_ITEM_TYPE_VALUE;
    si[*q].value = value_c;
  }
  else if (type_c == INPUT_NUMBER_ADDRESS_LABEL) {
    si[*q].type = STACK_ITEM_TYPE_LABEL;
    strcpy(si[*q].string, string_c);
  }
  else if (type_c == INPUT_NUMBER_STACK) {
    si[*q].type = STACK_ITEM_TYPE_STACK;
    si[*q].value = value_c;
  }
  
  return SUCCEEDED;
}


static int _stack_calculate(char *in, int *value, int *bytes_parsed, unsigned char from_substitutor, struct stack_item *si, struct stack_item *ta) {

  int q = 0, b = 0, d, k, n, curly_braces = 0, got_label = NO, can_skip_newline = NO;
  unsigned char e, op[MAX_STACK_CALCULATOR_ITEMS], sign[MAX_STACK_CALCULATOR_ITEMS];
  double dou = 0.0, dom;
  char *in_original = in;
  struct stack *stack;

  /* slice the data into infix format */
  while (*in != 0) {
    if (*in == 0xA) {
      if (can_skip_newline == YES && q > 1) {
        can_skip_newline = NO;
        in++;
        next_line();
        continue;
      }
      else
        break;
    }
    if (q >= MAX_STACK_CALCULATOR_ITEMS-1) {
      print_error(ERROR_STC, "Out of stack space. Adjust MAX_STACK_CALCULATOR_ITEMS in defines.h and recompile WLA!\n");
      return FAILED;
    }

    can_skip_newline = NO;

    /* init the stack item */
    si[q].type = 0x123456;
    si[q].sign = -1;
    si[q].value = 0x123456;
    si[q].can_calculate_deltas = NO;
    si[q].has_been_replaced = NO;
    si[q].is_in_postfix = NO;
    si[q].string[0] = 0;

    /* for sanity check */
    d = 0x123456;

    if (*in == ' ') {
      in++;
      continue;
    }
    else if (*in == '-') {
      k = YES;
      e = *(in + 2);
      if ((e >= '0' && e <= '9') || (e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z') || e == '_')
        k = NO;
      
      if (*(in + 1) == '-' && k == YES) {
        si[q].type = STACK_ITEM_TYPE_LABEL;
        si[q].sign = SI_SIGN_POSITIVE;
        for (k = 0; *in == '-' && k < 32; k++, in++)
          si[q].string[k] = '-';
        si[q].string[k] = 0;
      }
      else {
        si[q].type = STACK_ITEM_TYPE_OPERATOR;
        si[q].sign = SI_SIGN_POSITIVE;
        si[q].value = SI_OP_SUB;
        in++;
        can_skip_newline = YES;
      }
      q++;
    }
    else if (*in == '+') {
      if (*(in + 1) == '+') {
        si[q].type = STACK_ITEM_TYPE_LABEL;
        si[q].sign = SI_SIGN_POSITIVE;
        for (k = 0; *in == '+' && k < 32; k++, in++)
          si[q].string[k] = '+';
        si[q].string[k] = 0;
      }
      else {
        si[q].type = STACK_ITEM_TYPE_OPERATOR;
        si[q].sign = SI_SIGN_POSITIVE;
        si[q].value = SI_OP_ADD;
        in++;
        can_skip_newline = YES;
      }
      q++;
    }
    else if (*in == '*') {
      si[q].type = STACK_ITEM_TYPE_OPERATOR;
      si[q].sign = SI_SIGN_POSITIVE;
      si[q].value = SI_OP_MULTIPLY;
      can_skip_newline = YES;
      q++;
      in++;
    }
    else if (*in == '/') {
      si[q].type = STACK_ITEM_TYPE_OPERATOR;
      si[q].sign = SI_SIGN_POSITIVE;
      si[q].value = SI_OP_DIVIDE;
      can_skip_newline = YES;
      q++;
      in++;
    }
    else if (*in == '|' && *(in + 1) == '|') {
      if (g_input_parse_if == YES) {
        si[q].type = STACK_ITEM_TYPE_OPERATOR;
        si[q].sign = SI_SIGN_POSITIVE;
        si[q].value = SI_OP_LOGICAL_OR;
        can_skip_newline = YES;
        q++;
        in += 2;
      }
      else
        break;
    }
    else if (*in == '|') {
      si[q].type = STACK_ITEM_TYPE_OPERATOR;
      si[q].sign = SI_SIGN_POSITIVE;
      si[q].value = SI_OP_OR;
      can_skip_newline = YES;
      q++;
      in++;
    }
    else if (*in == '&' && *(in + 1) == '&') {
      if (g_input_parse_if == YES) {
        si[q].type = STACK_ITEM_TYPE_OPERATOR;
        si[q].sign = SI_SIGN_POSITIVE;
        si[q].value = SI_OP_LOGICAL_AND;
        can_skip_newline = YES;
        q++;
        in += 2;
      }
      else
        break;
    }
    else if (*in == '&') {
      si[q].type = STACK_ITEM_TYPE_OPERATOR;
      si[q].sign = SI_SIGN_POSITIVE;
      si[q].value = SI_OP_AND;
      can_skip_newline = YES;
      q++;
      in++;
    }
    else if (*in == '^') {
      si[q].type = STACK_ITEM_TYPE_OPERATOR;
      si[q].sign = SI_SIGN_POSITIVE;
      si[q].value = SI_OP_POWER;
      can_skip_newline = YES;
      q++;
      in++;
    }
    else if (*in == '#') {
      if (q == 0) {
        if (g_input_number_error_msg == YES)
          print_error(ERROR_STC, "Syntax error. Invalid use of modulo.\n");
        return FAILED;
      }
      si[q].type = STACK_ITEM_TYPE_OPERATOR;
      si[q].sign = SI_SIGN_POSITIVE;
      si[q].value = SI_OP_MODULO;
      can_skip_newline = YES;
      q++;
      in++;
    }
    else if (*in == '<' && *(in + 1) == '<') {
      si[q].type = STACK_ITEM_TYPE_OPERATOR;
      si[q].sign = SI_SIGN_POSITIVE;
      si[q].value = SI_OP_SHIFT_LEFT;
      can_skip_newline = YES;
      q++;
      in += 2;
    }
    else if (*in == '<' && *(in + 1) == '=') {
      if (g_input_parse_if == YES) {
        si[q].type = STACK_ITEM_TYPE_OPERATOR;
        si[q].sign = SI_SIGN_POSITIVE;
        si[q].value = SI_OP_COMPARE_LTE;
        can_skip_newline = YES;
        q++;
        in += 2;
      }
      else
        break;
    }
    else if (*in == '<') {
      if (g_input_parse_if == YES) {
        si[q].type = STACK_ITEM_TYPE_OPERATOR;
        si[q].sign = SI_SIGN_POSITIVE;
        si[q].value = SI_OP_COMPARE_LT;
        can_skip_newline = YES;
        q++;
        in++;
      }
      else {
        /* should we end parsing here? */
        if (b == 0 && q > 0) {
          if ((si[q-1].type == STACK_ITEM_TYPE_OPERATOR && si[q-1].value == SI_OP_RIGHT) ||
              si[q-1].type == STACK_ITEM_TYPE_VALUE || si[q-1].type == STACK_ITEM_TYPE_STRING ||
              si[q-1].type == STACK_ITEM_TYPE_LABEL)
            break;
        }

        si[q].type = STACK_ITEM_TYPE_OPERATOR;
        si[q].sign = SI_SIGN_POSITIVE;
        si[q].value = SI_OP_LOW_BYTE;
        can_skip_newline = YES;
        q++;
        in++;
      }
    }
    else if (*in == '>' && *(in + 1) == '>') {
      si[q].type = STACK_ITEM_TYPE_OPERATOR;
      si[q].sign = SI_SIGN_POSITIVE;
      si[q].value = SI_OP_SHIFT_RIGHT;
      can_skip_newline = YES;
      q++;
      in += 2;
    }
    else if (*in == '>' && *(in + 1) == '=') {
      if (g_input_parse_if == YES) {
        si[q].type = STACK_ITEM_TYPE_OPERATOR;
        si[q].sign = SI_SIGN_POSITIVE;
        si[q].value = SI_OP_COMPARE_GTE;
        can_skip_newline = YES;
        q++;
        in += 2;
      }
      else
        break;
    }
    else if (*in == '>') {
      if (g_input_parse_if == YES) {
        si[q].type = STACK_ITEM_TYPE_OPERATOR;
        si[q].sign = SI_SIGN_POSITIVE;
        si[q].value = SI_OP_COMPARE_GT;
        can_skip_newline = YES;
        q++;
        in++;
      }
      else {
        /* should we end parsing here? */
        if (b == 0 && q > 0) {
          if ((si[q-1].type == STACK_ITEM_TYPE_OPERATOR && si[q-1].value == SI_OP_RIGHT) ||
              si[q-1].type == STACK_ITEM_TYPE_VALUE || si[q-1].type == STACK_ITEM_TYPE_STRING ||
              si[q-1].type == STACK_ITEM_TYPE_LABEL)
            break;
        }

        si[q].type = STACK_ITEM_TYPE_OPERATOR;
        si[q].sign = SI_SIGN_POSITIVE;
        si[q].value = SI_OP_HIGH_BYTE;
        can_skip_newline = YES;
        q++;
        in++;
      }
    }
    else if (*in == '~') {
      si[q].type = STACK_ITEM_TYPE_OPERATOR;
      si[q].sign = SI_SIGN_POSITIVE;
      si[q].value = SI_OP_XOR;
      can_skip_newline = YES;
      q++;
      in++;
    }
    else if (*in == ':') {
      si[q].type = STACK_ITEM_TYPE_OPERATOR;
      si[q].sign = SI_SIGN_POSITIVE;
      si[q].value = SI_OP_BANK;
      q++;
      in++;
    }
    else if (*in == '\\' && *(in + 1) == '@') {
      if (g_macro_runtime_current != NULL) {
        si[q].type = STACK_ITEM_TYPE_VALUE;
        si[q].value = g_macro_runtime_current->macro->calls - 1;
        si[q].sign = SI_SIGN_POSITIVE;
        in += 2;
        q++;
      }
      else {
        print_error(ERROR_NUM, "\"\\@\" cannot be used here as we are not inside a .MACRO.\n");
        return FAILED;
      }
    }
    else if (*in == '=' && *(in + 1) == '=') {
      if (g_input_parse_if == YES) {
        si[q].type = STACK_ITEM_TYPE_OPERATOR;
        si[q].sign = SI_SIGN_POSITIVE;
        si[q].value = SI_OP_COMPARE_EQ;
        can_skip_newline = YES;
        q++;
        in += 2;
      }
      else
        break;
    }
    else if (*in == '!' && *(in + 1) == '=') {
      if (g_input_parse_if == YES) {
        si[q].type = STACK_ITEM_TYPE_OPERATOR;
        si[q].sign = SI_SIGN_POSITIVE;
        si[q].value = SI_OP_COMPARE_NEQ;
        can_skip_newline = YES;
        q++;
        in += 2;
      }
      else
        break;
    }
    else if (*in == '!') {
      si[q].type = STACK_ITEM_TYPE_OPERATOR;
      si[q].sign = SI_SIGN_POSITIVE;
      si[q].value = SI_OP_NOT;
      q++;
      in++;
    }
    else if (*in == '(') {
      si[q].type = STACK_ITEM_TYPE_OPERATOR;
      si[q].sign = SI_SIGN_POSITIVE;
      si[q].value = SI_OP_LEFT;
      can_skip_newline = YES;
      /* was previous token ')'? */
      if (q > 0 && si[q-1].type == STACK_ITEM_TYPE_OPERATOR && si[q-1].value == SI_OP_RIGHT)
        break;      
      q++;
      b++;
      in++;
    }
    else if (*in == '.' && (*(in+1) == 'b' || *(in+1) == 'B' || *(in+1) == 'w' || *(in+1) == 'W' || *(in+1) == 'l' || *(in+1) == 'L' || *(in+1) == 'd' || *(in+1) == 'D')) {
      in++;
      d = g_operand_hint;
      k = g_operand_hint_type;
      if (*in == 'b' || *in == 'B') {
        g_operand_hint = HINT_8BIT;
        g_operand_hint_type = HINT_TYPE_GIVEN;
        in++;
      }
      else if (*in == 'w' || *in == 'W') {
        g_operand_hint = HINT_16BIT;
        g_operand_hint_type = HINT_TYPE_GIVEN;
        in++;
      }
      else if (*in == 'l' || *in == 'L') {
        g_operand_hint = HINT_24BIT;
        g_operand_hint_type = HINT_TYPE_GIVEN;
        in++;
      }
      else if (*in == 'd' || *in == 'D') {
        g_operand_hint = HINT_32BIT;
        g_operand_hint_type = HINT_TYPE_GIVEN;
        in++;
      }
      else
        break;

      if (d != HINT_NONE && k == HINT_TYPE_GIVEN && d != g_operand_hint) {
        print_error(ERROR_STC, "Confusing operand hint!\n");
        in++;
      }
    }
    else if (*in == ')') {
      si[q].type = STACK_ITEM_TYPE_OPERATOR;
      si[q].sign = SI_SIGN_POSITIVE;
      si[q].value = SI_OP_RIGHT;
      /* end of expression? */
      if (b == 0)
        break;
      b--;
      q++;
      in++;
    }
    else if (*in == '}')
      break;
    else if (*in == ',')
      break;
    else if (*in == ']')
      break;
    else if (*in == '%' || (*in == '0' && (*(in+1) == 'b' || *(in+1) == 'B'))) {
      if (*in == '0')
        in++;
      d = 0;
      for (k = 0; k < 32; k++) {
        in++;
        e = *in;
        if (e == '0' || e == '1')
          d = (d << 1) + (e - '0');
        else if (e == ' ' || e == ')' || e == '|' || e == '&' || e == '+' || e == '-' || e == '*' ||
                 e == '/' || e == ',' || e == '^' || e == '<' || e == '>' || e == '#' || e == '~' ||
                 e == ']' || e == '.' || e == '=' || e == '!' || e == '}' || e == '(' || e == 0xA || e == 0)
          break;
        else {
          if (g_input_number_error_msg == YES)
            print_error(ERROR_NUM, "Got '%c' (%d) when expected a 0 or 1.\n", e, e);
          return FAILED;
        }
      }

      if (k == 32) {
        in++;
        if (*in == '0' || *in == '1') {
          if (g_input_number_error_msg == YES)
            print_error(ERROR_NUM, "Too many bits in a binary value, max is 32.\n");
          return FAILED;
        }
      }

      si[q].type = STACK_ITEM_TYPE_VALUE;
      si[q].value = d;
      si[q].sign = SI_SIGN_POSITIVE;
      q++;
    }
    else if (*in == '\'') {
      in++;
      if (*in == '\\' && (*(in+1) == 't' || *(in+1) == 'r' || *(in+1) == 'n' || *(in+1) == '0')) {
        in++;
        if (*in == 't')
          d = '\t';
        else if (*in == 'r')
          d = '\r';
        else if (*in == 'n')
          d = '\n';
        else /* if (*in == '0') */
          d = '\0';
      }
      else
        d = *in;
      in++;
      if (*in != '\'') {
        print_error(ERROR_NUM, "Got '%c' (%d) when expected \"'\".\n", *in, *in);
        return FAILED;
      }
      in++;

      si[q].type = STACK_ITEM_TYPE_VALUE;
      si[q].value = d;
      si[q].sign = SI_SIGN_POSITIVE;
      q++;
    }
    else if (*in == '$' || (*in == '0' && (*(in+1) == 'x' || *(in+1) == 'X'))) {
      int needs_shifting = NO;
      
      /* we'll break if the previous item in the stack was a value or a string / label */
      if (_break_before_value_or_string(q, &si[0]) == SUCCEEDED)
        break;

      if (*in == '0')
        in++;
      
      d = 0;
      for (k = 0, in++; k < 8; k++, in++) {
        e = *in;
        if (e >= '0' && e <= '9')
          d += e - '0';
        else if (e >= 'A' && e <= 'F')
          d += e - 'A' + 10;
        else if (e >= 'a' && e <= 'f')
          d += e - 'a' + 10;
        else if (e == ' ' || e == ')' || e == '|' || e == '&' || e == '+' || e == '-' || e == '(' ||
                 e == '*' || e == '/' || e == ',' || e == '^' || e == '<' || e == '>' ||
                 e == '#' || e == '~' || e == ']' || e == '.' || e == '=' || e == '!' || e == '}' || e == 0xA || e == 0) {
          needs_shifting = YES;
          break;
        }
        else {
          if (g_input_number_error_msg == YES) {
            print_error(ERROR_NUM, "Got '%c' (%d) when expected [0-F].\n", e, e);
          }
          return FAILED;
        }

        if (k < 7)
          d = d << 4;
      }

      if (needs_shifting == YES)
        d = d >> 4;

      if (*in == 'h' || *in == 'H')
        in++;
      
      si[q].type = STACK_ITEM_TYPE_VALUE;
      si[q].value = d;
      si[q].sign = SI_SIGN_POSITIVE;
      q++;
    }
    else if (*in >= '0' && *in <= '9') {
      /* we'll break if the previous item in the stack was a value or a string / label */
      if (_break_before_value_or_string(q, &si[0]) == SUCCEEDED)
        break;

      /* is it a hexadecimal value after all? */
      n = 0;
      for (k = 0; k < 9; k++) {
        if (in[k] >= '0' && in[k] <= '9')
          continue;
        if (in[k] >= 'a' && in[k] <= 'f') {
          n = 1;
          break;
        }
        if (in[k] >= 'A' && in[k] <= 'F') {
          n = 1;
          break;
        }
        if (in[k] == 'h' || in[k] == 'H') {
          n = 1;
          break;
        }
        break;
      }

      if (n == 1) {
        /* it's hex */
        int needs_shifting = NO;

        d = 0;
        for (k = 0; k < 8; k++, in++) {
          e = *in;
          if (e >= '0' && e <= '9')
            d += e - '0';
          else if (e >= 'A' && e <= 'F')
            d += e - 'A' + 10;
          else if (e >= 'a' && e <= 'f')
            d += e - 'a' + 10;
          else if (e == ' ' || e == ')' || e == '|' || e == '&' || e == '+' || e == '-' ||
                   e == '*' || e == '/' || e == ',' || e == '^' || e == '<' || e == '>' ||
                   e == '#' || e == '~' || e == ']' || e == '.' || e == 'h' || e == 'H' ||
                   e == '=' || e == '!' || e == '}' || e == '(' || e == 0xA || e == 0) {
            needs_shifting = YES;
            break;
          }
          else {
            if (g_input_number_error_msg == YES) {
              print_error(ERROR_NUM, "Got '%c' (%d) when expected [0-F].\n", e, e);
            }
            return FAILED;
          }

          if (k < 7)
            d = d << 4;
        }

        if (needs_shifting == YES)
          d = d >> 4;

        if (*in == 'h' || *in == 'H')
          in++;

        si[q].type = STACK_ITEM_TYPE_VALUE;
        si[q].value = d;
        si[q].sign = SI_SIGN_POSITIVE;
        q++;
      }
      else {
        int max_digits = 10;
        
        /* it's decimal */
        dou = (*in - '0')*10.0;
        dom = 1.0;
        n = 0;
        for (k = 0; k < max_digits; k++) {
          in++;
          e = *in;
          if (e >= '0' && e <= '9') {
            if (k == max_digits - 1) {
              if (n == 0)
                print_error(ERROR_NUM, "Too many digits in the integer value. Max 10 is supported.\n");
              else {
                print_error(ERROR_NUM, "Too many digits in the floating point value. Max %d is supported.\n", MAX_FLOAT_DIGITS);
              }
              return FAILED;
            }

            if (n == 0) {
              dou += e - '0';
              dou *= 10.0;
            }
            else if (n == 1) {
              dou += dom*(e - '0');
              dom /= 10.0;
            }
          }
          else if (e == ' ' || e == ')' || e == '|' || e == '&' || e == '+' || e == '-' || e == '*' ||
                   e == '/' || e == ',' || e == '^' || e == '<' || e == '>' || e == '#' || e == '~' ||
                   e == ']' || e == '=' || e == '!' || e == '}' || e == '(' || e == 0xA || e == 0)
            break;
          else if (e == '.') {
            if (*(in+1) == 'b' || *(in+1) == 'B' || *(in+1) == 'w' || *(in+1) == 'W' || *(in+1) == 'l' || *(in+1) == 'L' || *(in+1) == 'd' || *(in+1) == 'D')
              break;
            if (g_parse_floats == NO)
              break;
            if (n == 1) {
              if (g_input_number_error_msg == YES)
                print_error(ERROR_NUM, "Syntax error.\n");
              return FAILED;
            }
            n = 1;
            max_digits = MAX_FLOAT_DIGITS+1;
          }
          else {
            if (g_input_number_error_msg == YES)
              print_error(ERROR_NUM, "Got '%c' (%d) when expected [0-9].\n", e, e);
            return FAILED;
          }
        }

        dou /= 10;

        si[q].type = STACK_ITEM_TYPE_VALUE;
        si[q].value = dou;
        si[q].sign = SI_SIGN_POSITIVE;
        q++;
      }
    }
    else if (*in == '"') {
      /* definitely a string! */

      /* we'll break if the previous item in the stack was a value or a string / label */
      if (_break_before_value_or_string(q, &si[0]) == SUCCEEDED)
        break;

      /* skip '"' */
      in++;
      
      si[q].sign = SI_SIGN_POSITIVE;
      e = *in;
      for (k = 0; k < MAX_NAME_LENGTH; k++) {
        e = *in++;

        if (e == 0xA)
          break;
        if (e == '"')
          break;
        if (e == '\\' && *in == '"')
          e = *in++;

        si[q].string[k] = e;
      }

      if (e != '"') {
        print_error(ERROR_NUM, "Malformed string.\n");
        return FAILED;
      }
      
      si[q].string[k] = 0;
      si[q].type = STACK_ITEM_TYPE_STRING;
      q++;

      if (process_string_for_special_characters(si[q].string, NULL) == FAILED)
        return FAILED;
    }
    else {
      /* it must be a label! */
      int is_label = YES, is_already_processed_function = NO, unknown_function = NO;

      /* we'll break if the previous item in the stack was a value or a string / label */
      if (_break_before_value_or_string(q, &si[0]) == SUCCEEDED)
        break;

      si[q].sign = SI_SIGN_POSITIVE;
      for (k = 0; k < MAX_NAME_LENGTH; k++) {
        e = *in;

        if (e == '{')
          curly_braces++;
        else if (e == '}')
          curly_braces--;

        if (curly_braces <= 0) {
          if (from_substitutor == YES && curly_braces < 0) {
            si[q].string[k] = 0;
            break;
          }
          if (e == ' ' || e == ')' || e == '|' || e == '&' || e == '+' || e == '-' || e == '*' ||
              e == '/' || e == ',' || e == '^' || e == '<' || e == '>' || e == '#' || e == ']' ||
              e == '~' || e == '=' || e == '!' || e == 0xA || e == 0)
            break;
          if (e == '.' && (*(in+1) == 'b' || *(in+1) == 'B' || *(in+1) == 'w' || *(in+1) == 'W' || *(in+1) == 'l' || *(in+1) == 'L' || *(in+1) == 'd' || *(in+1) == 'D') &&
              (*(in+2) == ' ' || *(in+2) == ')' || *(in+2) == '|' || *(in+2) == '&' || *(in+2) == '+' || *(in+2) == '-' || *(in+2) == '*' ||
               *(in+2) == '/' || *(in+2) == ',' || *(in+2) == '^' || *(in+2) == '<' || *(in+2) == '>' || *(in+2) == '#' || *(in+2) == ']' ||
               *(in+2) == '~' || *(in+2) == 0xA || *(in+2) == 0))
            break;
        }
        else if (e == 0xA)
          break;
        
        si[q].string[k] = e;
        in++;

        if (k == 3 && strcaselesscmpn(si[q].string, "asc(", 4) == 0) {
          int parsed_chars = 0;
          
          if (_parse_function_asc(in, &d, &parsed_chars) == FAILED)
            return FAILED;
          in += parsed_chars;
          is_label = NO;
          break;
        }
        else if (k == 3 && strcaselesscmpn(si[q].string, "min(", 4) == 0) {
          if (_parse_function_math2_base(&in, si, &q, "min(a,b)", SI_OP_MIN) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 3 && strcaselesscmpn(si[q].string, "max(", 4) == 0) {
          if (_parse_function_math2_base(&in, si, &q, "max(a,b)", SI_OP_MAX) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 3 && strcaselesscmpn(si[q].string, "pow(", 4) == 0) {
          if (_parse_function_math2_base(&in, si, &q, "pow(a,b)", SI_OP_POW) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 3 && strcaselesscmpn(si[q].string, "abs(", 4) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "abs(a)", SI_OP_ABS) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 3 && strcaselesscmpn(si[q].string, "cos(", 4) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "cos(a)", SI_OP_COS) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 3 && strcaselesscmpn(si[q].string, "sin(", 4) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "sin(a)", SI_OP_SIN) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 3 && strcaselesscmpn(si[q].string, "tan(", 4) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "tan(a)", SI_OP_TAN) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 3 && strcaselesscmpn(si[q].string, "log(", 4) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "log(a)", SI_OP_LOG) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 5 && strcaselesscmpn(si[q].string, "log10(", 6) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "log10(a)", SI_OP_LOG10) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 4 && strcaselesscmpn(si[q].string, "cosh(", 5) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "cosh(a)", SI_OP_COSH) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 4 && strcaselesscmpn(si[q].string, "sinh(", 5) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "sinh(a)", SI_OP_SINH) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 4 && strcaselesscmpn(si[q].string, "tanh(", 5) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "tanh(a)", SI_OP_TANH) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 4 && strcaselesscmpn(si[q].string, "acos(", 5) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "acos(a)", SI_OP_ACOS) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 4 && strcaselesscmpn(si[q].string, "asin(", 5) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "asin(a)", SI_OP_ASIN) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 4 && strcaselesscmpn(si[q].string, "atan(", 5) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "atan(a)", SI_OP_ATAN) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 4 && strcaselesscmpn(si[q].string, "sign(", 5) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "sign(a)", SI_OP_SIGN) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 5 && strcaselesscmpn(si[q].string, "clamp(", 6) == 0) {
          if (_parse_function_math3_base(&in, si, &q, "clamp(v,min,max)", SI_OP_CLAMP) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 5 && strcaselesscmpn(si[q].string, "atan2(", 6) == 0) {
          if (_parse_function_math2_base(&in, si, &q, "atan2(a,b)", SI_OP_ATAN2) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 5 && strcaselesscmpn(si[q].string, "round(", 6) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "round(a)", SI_OP_ROUND) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 4 && strcaselesscmpn(si[q].string, "ceil(", 5) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "ceil(a)", SI_OP_CEIL) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 5 && strcaselesscmpn(si[q].string, "floor(", 6) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "floor(a)", SI_OP_FLOOR) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 4 && strcaselesscmpn(si[q].string, "sqrt(", 5) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "sqrt(a)", SI_OP_SQRT) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 6 && strcaselesscmpn(si[q].string, "lobyte(", 7) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "lobyte(a)", SI_OP_LOW_BYTE) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 6 && strcaselesscmpn(si[q].string, "hibyte(", 7) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "hibyte(a)", SI_OP_HIGH_BYTE) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 6 && strcaselesscmpn(si[q].string, "loword(", 7) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "loword(a)", SI_OP_LOW_WORD) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 6 && strcaselesscmpn(si[q].string, "hiword(", 7) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "hiword(a)", SI_OP_HIGH_WORD) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 8 && strcaselesscmpn(si[q].string, "bankbyte(", 9) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "bankbyte(a)", SI_OP_BANK_BYTE) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 4 && strcaselesscmpn(si[q].string, "bank(", 5) == 0) {
          if (_parse_function_math1_base(&in, si, &q, "bank(a)", SI_OP_BANK) == FAILED)
            return FAILED;
          is_already_processed_function = YES;
          break;
        }
        else if (k == 6 && strcaselesscmpn(si[q].string, "random(", 7) == 0) {
          int parsed_chars = 0;
          
          if (_parse_function_random(in, &d, &parsed_chars) == FAILED)
            return FAILED;
          in += parsed_chars;
          is_label = NO;
          break;
        }
        else if (k == 7 && strcaselesscmpn(si[q].string, "defined(", 8) == 0) {
          int parsed_chars = 0;
          
          if (_parse_function_defined(in, &d, &parsed_chars) == FAILED)
            return FAILED;
          in += parsed_chars;
          is_label = NO;
          break;
        }
        else if (k == 6 && strcaselesscmpn(si[q].string, "exists(", 7) == 0) {
          int parsed_chars = 0;

          if (_parse_function_exists(in, &d, &parsed_chars) == FAILED)
            return FAILED;
          in += parsed_chars;
          is_label = NO;          
          break;
        }

        if (e == '(') {
          /* are we calling a user created function? */
          int found_function = NO, res, parsed_chars = 0;
          
          si[q].string[k] = 0;

          res = parse_function(in, si[q].string, &found_function, &parsed_chars);
          if (found_function == NO) {
            if (g_fail_quetly_on_non_found_functions == YES) {
              in--;
              unknown_function = YES;
              break;
            }

            print_error(ERROR_NUM, "Could not find function \"%s\".\n", si[q].string);
            return FAILED;
          }

          if (res == FAILED)
            return FAILED;
          else if (res == SUCCEEDED) {
            si[q].type = STACK_ITEM_TYPE_VALUE;
            si[q].value = g_parsed_double;
            si[q].sign = SI_SIGN_POSITIVE;
          }
          else if (res == INPUT_NUMBER_STACK) {
            if (g_parsing_function_body == YES) {
              struct stack *s;
              int item;

              s = find_stack_calculation(g_latest_stack, YES);
              if (s == NULL)
                return FAILED;

              /* WARNING: here we embed a postfix calculation inside an infix calculation!
                 it should work as the infix -> postfix converter should notice the postfix
                 parts and just copy them directly... */
              si[q].type = STACK_ITEM_TYPE_OPERATOR;
              si[q].sign = SI_SIGN_POSITIVE;
              si[q].value = SI_OP_LEFT;
              si[q].is_in_postfix = YES;
              /* abuse the struct */
              si[q].string[0] = 'X';
              si[q].string[1] = 0;
              q++;

              for (item = 0; q < MAX_STACK_CALCULATOR_ITEMS && item < s->stacksize; item++) {
                si[q].type = s->stack_items[item].type;
                si[q].sign = s->stack_items[item].sign;
                si[q].value = s->stack_items[item].value;
                si[q].is_in_postfix = YES;
                si[q].can_calculate_deltas = s->stack_items[item].can_calculate_deltas;
                si[q].has_been_replaced = s->stack_items[item].has_been_replaced;
                if (si[q].type == STACK_ITEM_TYPE_LABEL)
                  strcpy(si[q].string, s->stack_items[item].string);
                else
                  si[q].string[0] = 0;
                q++;
              }
              
              if (q >= MAX_STACK_CALCULATOR_ITEMS-1) {
                print_error(ERROR_STC, "Out of stack space. Adjust MAX_STACK_CALCULATOR_ITEMS in defines.h and recompile WLA!\n");
                return FAILED;
              }

              si[q].type = STACK_ITEM_TYPE_OPERATOR;
              si[q].sign = SI_SIGN_POSITIVE;
              si[q].value = SI_OP_RIGHT;
              si[q].is_in_postfix = YES;
              /* abuse the struct */
              si[q].string[0] = 'X';
              si[q].string[1] = 0;
            }
            else {
              si[q].type = STACK_ITEM_TYPE_STACK;
              si[q].value = g_latest_stack;
              si[q].sign = SI_SIGN_POSITIVE;
            }           
          }
          else {
            print_error(ERROR_NUM, "Function \"%s\" didn't return a stack calculation or a value.\n", si[q].string);
            return FAILED;
          }

          in += parsed_chars;
          is_already_processed_function = YES;    

          break;
        }
      }

      if (is_already_processed_function == YES) {
      }
      else if (is_label == YES) {
        si[q].string[k] = 0;
        process_special_labels(si[q].string);
        si[q].type = STACK_ITEM_TYPE_LABEL;
        got_label = YES;

        if (from_substitutor == NO && expand_variables_inside_string(si[q].string, sizeof(((struct stack_item *)0)->string), NULL) == FAILED)
          return FAILED;
      }
      else {
        if (d == 0x123456) {
          /* sanity check */
          fprintf(stderr, "d = 0x123456! Internal error! Please submit a bug report!\n");
          return FAILED;
        }
        
        si[q].type = STACK_ITEM_TYPE_VALUE;
        si[q].value = d;
        si[q].sign = SI_SIGN_POSITIVE;
      }

      q++;

      if (unknown_function == YES)
        break;
    }
  }

  if (q == 0) {
    print_error(ERROR_STC, "Expected a calculation, label or a value, but got nothing!\n");
    return FAILED;
  }
  
  if (b != 0) {
    print_error(ERROR_STC, "Unbalanced parentheses.\n");
    return FAILED;
  }

  /* are labels 16-bit by default? */
  if (got_label == YES && g_operand_hint_type != HINT_TYPE_GIVEN && g_global_label_hint == HINT_16BIT) {
    g_operand_hint = HINT_16BIT;
    g_operand_hint_type = HINT_TYPE_GIVEN;
  }

  /* only one item found -> let the item parser handle it? */
  if (q == 1) {
    if (si[0].type == STACK_ITEM_TYPE_VALUE) {
      /* update the source pointer */
      *bytes_parsed += (int)(in - in_original) - 1;

      g_parsed_double = si[0].value;

      if (g_input_float_mode == ON)
        return INPUT_NUMBER_FLOAT;

      *value = (int)si[0].value;

      return SUCCEEDED;
    }
    if (from_substitutor == NO) {
      if (si[0].type == STACK_ITEM_TYPE_STACK) {
        /* update the source pointer */
        *bytes_parsed += (int)(in - in_original) - 1;

        g_latest_stack = (int)si[0].value;

        return INPUT_NUMBER_STACK;
      }

      return STACK_CALCULATE_DELAY;
    }
  }
  
  /* check if there was data before the computation */
  if (q > 1 && (si[0].type == STACK_ITEM_TYPE_LABEL || si[0].type == STACK_ITEM_TYPE_VALUE)) {
    if (si[1].type == STACK_ITEM_TYPE_LABEL || si[1].type == STACK_ITEM_TYPE_VALUE)
      return STACK_CALCULATE_DELAY;
    if (si[1].type == STACK_ITEM_TYPE_OPERATOR) {
      if (si[1].value == SI_OP_LEFT)
        return STACK_CALCULATE_DELAY;
    }
  }

#ifdef WLA_DEBUG
  fprintf(stderr, "PREOPT:\n");
  debug_print_stack(g_active_file_info_last->line_current, -1, si, q, 0, NULL);
#endif
  
#ifdef SPC700
  /* check if the computation is of the form "y+X" or "y+Y" and remove that "+X" or "+Y" */
  if (q > 2 && si[q - 2].type == STACK_ITEM_TYPE_OPERATOR && si[q - 2].value == SI_OP_ADD) {
    if (si[q - 1].type == STACK_ITEM_TYPE_LABEL && si[q - 1].string[1] == 0) {
      char w = si[q - 1].string[0];

      if (w == 'x' || w == 'X' || w == 'y' || w == 'Y') {
        q -= 2;
        while (*in != '+')
          in--;
      }
    }
  }
#endif

  /* check if the computation is of the form "+-..." and remove that leading "+" */
  if (q > 2 && si[0].type == STACK_ITEM_TYPE_OPERATOR && si[0].value == SI_OP_ADD &&
      si[1].type == STACK_ITEM_TYPE_OPERATOR && si[1].value == SI_OP_SUB) {
    si[0].type = STACK_ITEM_TYPE_DELETED;
  }

  /* update the source pointer */
  *bytes_parsed += (int)(in - in_original) - 1;

  /* fix the sign in every operand */
  for (b = 1, k = 0; k < q; k++) {
    if (si[k].is_in_postfix == YES) {
      b = 0;
      continue;
    }
    if (g_input_parse_if == NO) {
      /* *-1 or *-LABEL? */
      if (k < q-2 && si[k].type == STACK_ITEM_TYPE_OPERATOR && si[k + 1].type == STACK_ITEM_TYPE_OPERATOR && si[k].value != SI_OP_RIGHT &&
          si[k + 1].value == SI_OP_SUB && (si[k + 2].type == STACK_ITEM_TYPE_VALUE || si[k + 2].type == STACK_ITEM_TYPE_LABEL)) {
        if (si[k + 2].sign == SI_SIGN_POSITIVE)
          si[k + 2].sign = SI_SIGN_NEGATIVE;
        else
          si[k + 2].sign = SI_SIGN_POSITIVE;
        /* it wasn't a minus operator, it was a sign */
        si[k + 1].type = STACK_ITEM_TYPE_DELETED;
      }
      /* -abs()? */
      if (k < q-1 && si[k].type == STACK_ITEM_TYPE_OPERATOR && si[k].value == SI_OP_SUB && si[k+1].type == STACK_ITEM_TYPE_OPERATOR &&
          (si[k + 1].value == SI_OP_ROUND ||
           si[k + 1].value == SI_OP_FLOOR ||
           si[k + 1].value == SI_OP_CEIL ||
           si[k + 1].value == SI_OP_MIN ||
           si[k + 1].value == SI_OP_MAX ||
           si[k + 1].value == SI_OP_ABS ||
           si[k + 1].value == SI_OP_COS ||
           si[k + 1].value == SI_OP_SIN ||
           si[k + 1].value == SI_OP_TAN ||
           si[k + 1].value == SI_OP_COSH ||
           si[k + 1].value == SI_OP_SINH ||
           si[k + 1].value == SI_OP_TANH ||
           si[k + 1].value == SI_OP_ACOS ||
           si[k + 1].value == SI_OP_ASIN ||
           si[k + 1].value == SI_OP_ATAN ||
           si[k + 1].value == SI_OP_ATAN2 ||
           si[k + 1].value == SI_OP_LOW_BYTE ||
           si[k + 1].value == SI_OP_HIGH_BYTE ||
           si[k + 1].value == SI_OP_LOW_WORD ||
           si[k + 1].value == SI_OP_HIGH_WORD ||
           si[k + 1].value == SI_OP_BANK_BYTE ||
           si[k + 1].value == SI_OP_BANK ||
           si[k + 1].value == SI_OP_LOG ||
           si[k + 1].value == SI_OP_LOG10 ||
           si[k + 1].value == SI_OP_POW ||
           si[k + 1].value == SI_OP_CLAMP ||
           si[k + 1].value == SI_OP_SIGN ||
           si[k + 1].value == SI_OP_SQRT)) {
        if (k == 0 || (si[k - 1].type == STACK_ITEM_TYPE_OPERATOR && si[k - 1].value == SI_OP_LEFT)) {
          si[k].type = STACK_ITEM_TYPE_DELETED;
          if (si[k + 1].sign == SI_SIGN_POSITIVE)
            si[k + 1].sign = SI_SIGN_NEGATIVE;
          else
            si[k + 1].sign = SI_SIGN_POSITIVE;
        }
      }
      /* -C(?)? (stack calculation) */
      if (k < q-1 && si[k].type == STACK_ITEM_TYPE_OPERATOR && si[k].value == SI_OP_SUB && si[k+1].type == STACK_ITEM_TYPE_STACK) {
        if (k == 0 || (si[k - 1].type == STACK_ITEM_TYPE_OPERATOR && si[k - 1].value == SI_OP_LEFT)) {
          si[k].type = STACK_ITEM_TYPE_DELETED;
          if (si[k + 1].sign == SI_SIGN_POSITIVE)
            si[k + 1].sign = SI_SIGN_NEGATIVE;
          else
            si[k + 1].sign = SI_SIGN_POSITIVE;
        }
      }
      if ((q - k) != 1 && si[k].type == STACK_ITEM_TYPE_OPERATOR && si[k + 1].type == STACK_ITEM_TYPE_OPERATOR &&
          si[k + 1].value != SI_OP_BANK &&
          si[k + 1].value != SI_OP_BANK_BYTE &&
          si[k + 1].value != SI_OP_HIGH_BYTE &&
          si[k + 1].value != SI_OP_LOW_BYTE &&
          si[k + 1].value != SI_OP_HIGH_WORD &&
          si[k + 1].value != SI_OP_LOW_WORD &&
          si[k + 1].value != SI_OP_ROUND &&
          si[k + 1].value != SI_OP_FLOOR &&
          si[k + 1].value != SI_OP_CEIL &&
          si[k + 1].value != SI_OP_MIN &&
          si[k + 1].value != SI_OP_MAX &&
          si[k + 1].value != SI_OP_ABS &&
          si[k + 1].value != SI_OP_COS &&
          si[k + 1].value != SI_OP_SIN &&
          si[k + 1].value != SI_OP_TAN &&
          si[k + 1].value != SI_OP_COSH &&
          si[k + 1].value != SI_OP_SINH &&
          si[k + 1].value != SI_OP_TANH &&
          si[k + 1].value != SI_OP_ACOS &&
          si[k + 1].value != SI_OP_ASIN &&
          si[k + 1].value != SI_OP_ATAN &&
          si[k + 1].value != SI_OP_ATAN2 &&
          si[k + 1].value != SI_OP_LOG &&
          si[k + 1].value != SI_OP_LOG10 &&
          si[k + 1].value != SI_OP_POW &&
          si[k + 1].value != SI_OP_CLAMP &&
          si[k + 1].value != SI_OP_SIGN &&
          si[k + 1].value != SI_OP_SQRT) {
        if (si[k].value != SI_OP_LEFT && si[k].value != SI_OP_RIGHT && si[k + 1].value != SI_OP_LEFT && si[k + 1].value != SI_OP_RIGHT) {
#ifdef WLA_DEBUG
          debug_print_stack(g_active_file_info_last->line_current, -1, si, q, 0, NULL);
#endif
          print_error(ERROR_STC, "Error in computation syntax.\n");
          return FAILED;
        }
      }
    }
    if (si[k].type == STACK_ITEM_TYPE_OPERATOR && si[k].value == SI_OP_SUB && b == 1) {
      if (si[k + 1].type == STACK_ITEM_TYPE_VALUE || si[k + 1].type == STACK_ITEM_TYPE_LABEL) {        
        if (si[k + 1].sign == SI_SIGN_POSITIVE)
          si[k + 1].sign = SI_SIGN_NEGATIVE;
        else
          si[k + 1].sign = SI_SIGN_POSITIVE;
        /* it wasn't a minus operator, it was a sign */
        si[k].type = STACK_ITEM_TYPE_DELETED;
      }
      else if (si[k + 1].type == STACK_ITEM_TYPE_OPERATOR && si[k + 1].value == SI_OP_LEFT)
        si[k].value = SI_OP_NEGATE;
    }
    /* remove unnecessary + */
    if (si[k].type == STACK_ITEM_TYPE_OPERATOR && si[k].value == SI_OP_ADD && b == 1) {
      if (si[k + 1].type == STACK_ITEM_TYPE_VALUE || si[k + 1].type == STACK_ITEM_TYPE_LABEL)
        si[k].type = STACK_ITEM_TYPE_DELETED;
      else if (si[k + 1].type == STACK_ITEM_TYPE_OPERATOR && si[k + 1].value == SI_OP_LEFT)
        si[k].type = STACK_ITEM_TYPE_DELETED;
    }
    else if (si[k].type == STACK_ITEM_TYPE_VALUE || si[k].type == STACK_ITEM_TYPE_LABEL || si[k].type == STACK_ITEM_TYPE_STACK)
      b = 0;
    else if (si[k].type == STACK_ITEM_TYPE_OPERATOR && si[k].value == SI_OP_LEFT)
      b = 1;
    else if (si[k].type == STACK_ITEM_TYPE_OPERATOR && (si[k].value == SI_OP_COMPARE_EQ ||
                                                        si[k].value == SI_OP_COMPARE_NEQ ||
                                                        si[k].value == SI_OP_COMPARE_LTE ||
                                                        si[k].value == SI_OP_COMPARE_LT ||
                                                        si[k].value == SI_OP_COMPARE_GT ||
                                                        si[k].value == SI_OP_COMPARE_GTE))
      b = 1;
  }

  /* turn unary XORs into NOTs */
  for (b = 1, k = 0; k < q; k++) {
    if (si[k].is_in_postfix == YES) {
      b = 0;
      continue;
    }
    if (si[k].type == STACK_ITEM_TYPE_OPERATOR && si[k].value == SI_OP_XOR && b == 1)
      si[k].value = SI_OP_NOT;
    else if (si[k].type == STACK_ITEM_TYPE_VALUE || si[k].type == STACK_ITEM_TYPE_LABEL || si[k].type == STACK_ITEM_TYPE_STACK)
      b = 0;
    else if (si[k].type == STACK_ITEM_TYPE_OPERATOR && si[k].value == SI_OP_LEFT)
      b = 1;
  }

  /* embed signs into values */
  for (k = 0; k < q; k++) {
    if (si[k].type == STACK_ITEM_TYPE_VALUE && si[k].sign == SI_SIGN_NEGATIVE) {
      si[k].sign = SI_SIGN_POSITIVE;
      si[k].value = -si[k].value;
    }
  }
  
  /* are we calculating deltas between two labels? */
  if (g_can_calculate_a_minus_b == YES) {
    for (k = 0; k < q; k++) {
      if (si[k].is_in_postfix == YES)
        continue;
      if (si[k].type == STACK_ITEM_TYPE_LABEL) {
        if (k+2 < q && si[k+1].type == STACK_ITEM_TYPE_OPERATOR && si[k+1].value == SI_OP_SUB && si[k+2].type == STACK_ITEM_TYPE_LABEL) {
          /* yes! mark such labels! */
          si[k].can_calculate_deltas = YES;
          si[k+2].can_calculate_deltas = YES;
          k += 2;
        }
      }
    }
  }

#ifdef WLA_DEBUG
  fprintf(stderr, "INFIX:\n");
  debug_print_stack(g_active_file_info_last->line_current, -1, si, q, 0, NULL);
#endif
    
  /* convert infix stack into postfix stack */
  for (b = 0, k = 0, d = 0; k < q; k++) {
    struct stack_item *out = &ta[d], *in = &si[k];
    int type = si[k].type;
    
    out->can_calculate_deltas = in->can_calculate_deltas;
    out->is_in_postfix = NO;
    out->has_been_replaced = in->has_been_replaced;

    /* postfix sections are copied 1:1 */
    if (in->is_in_postfix == YES && !(type == STACK_ITEM_TYPE_OPERATOR && in->string[0] == 'X')) {
      out->type = in->type;
      out->value = in->value;
      out->sign = in->sign;
      if (type == STACK_ITEM_TYPE_STRING || type == STACK_ITEM_TYPE_LABEL)
        strcpy(out->string, in->string);
      else
        out->string[0] = 0;
      d++;
      continue;
    }
    
    /* operands pass through */
    if (type == STACK_ITEM_TYPE_VALUE) {
      out->type = in->type;
      out->value = in->value;
      out->sign = in->sign;
      d++;
    }
    else if (type == STACK_ITEM_TYPE_STRING || type == STACK_ITEM_TYPE_LABEL) {
      out->type = in->type;
      strcpy(out->string, in->string);
      out->sign = in->sign;
      d++;
    }
    else if (type == STACK_ITEM_TYPE_STACK) {
      out->type = in->type;
      out->value = in->value;
      out->sign = in->sign;
      d++;
    }
    /* operators get inspected */
    else if (type == STACK_ITEM_TYPE_OPERATOR) {
      if (b == 0) {
        op[0] = (unsigned char)in->value;
        sign[0] = in->sign;
        b++;
      }
      else {
        if (in->value == SI_OP_LEFT) {
          op[b] = SI_OP_LEFT;
          b++;
        }
        else if (in->value == SI_OP_RIGHT) {
          b--;
          while (op[b] != SI_OP_LEFT) {
            out->type = STACK_ITEM_TYPE_OPERATOR;
            out->value = op[b];
            out->sign = sign[b];
            b--;
            d++;
            out = &ta[d];
          }
        }
        else {
          int priority = _get_op_priority((int)(in->value));

          b--;
          while (b != -1 && op[b] != SI_OP_LEFT && _get_op_priority(op[b]) >= priority) {
            out->type = STACK_ITEM_TYPE_OPERATOR;
            out->value = op[b];
            out->sign = sign[b];
            b--;
            d++;
            out = &ta[d];
          }
          b++;
          op[b] = (unsigned char)in->value;
          sign[b] = in->sign;
          b++;
        }
      }
    }
  }

  /* empty the operator stack */
  while (b > 0) {
    b--;
    ta[d].type = STACK_ITEM_TYPE_OPERATOR;
    ta[d].sign = sign[b];
    ta[d].value = op[b];
    d++;
  }

#ifdef WLA_DEBUG
  fprintf(stderr, "POSTFIX:\n");
  debug_print_stack(g_active_file_info_last->line_current, -1, ta, d, 0, NULL);
#endif
    
  /* are all the symbols known? */
  if ((g_resolve_stack_calculations == YES || from_substitutor == YES) && resolve_stack(ta, d) == SUCCEEDED) {
    struct stack s;

    /* sanity check */
    if (g_delta_counter != 0) {
      print_error(ERROR_STC, "g_delta_counter == %d != 0! Internal error. Please submit a bug report!\n", g_delta_counter);
      return FAILED;
    }

    init_stack_struct(&s);
    s.stack_items = ta;
    s.linenumber = g_active_file_info_last->line_current;
    s.filename_id = g_active_file_info_last->filename_id;

    if (compute_stack(&s, d, &dou) == FAILED)
      return FAILED;
    
    g_parsed_double = dou;

    if (g_input_float_mode == ON) {
#ifdef WLA_DEBUG
      fprintf(stderr, "RETURN FLOAT %f\n", dou);
#endif
      return INPUT_NUMBER_FLOAT;
    }

#ifdef WLA_DEBUG
    fprintf(stderr, "RETURN INT %d\n", (int)dou);
#endif
    
    *value = (int)dou;

    return SUCCEEDED;
  }

  /* only one string? */
  if (d == 1 && ta[0].type == STACK_ITEM_TYPE_STRING && ta[0].sign == SI_SIGN_POSITIVE) {
    strcpy(g_label, ta[0].string);
    process_special_labels(g_label);
#ifdef WLA_DEBUG
    fprintf(stderr, "RETURN STRING %s\n", g_label);
#endif
    return STACK_RETURN_STRING;
  }
  /* only one label? */
  if (d == 1 && ta[0].type == STACK_ITEM_TYPE_LABEL && ta[0].sign == SI_SIGN_POSITIVE) {
    strcpy(g_label, ta[0].string);
    process_special_labels(g_label);
#ifdef WLA_DEBUG
    fprintf(stderr, "RETURN LABEL %s\n", g_label);
#endif
    return STACK_RETURN_LABEL;
  }

  /*
    printf("%d %d %s\n", d, ta[0].type, ta[0].string);
  */

  /* we have a stack full of computation and we save it for wlalink */
  stack = allocate_struct_stack(d);
  if (stack == NULL)
    return FAILED;
  
  stack->linenumber = g_active_file_info_last->line_current;
  stack->filename_id = g_active_file_info_last->filename_id;
  stack->is_function_body = g_parsing_function_body;

  /* all stacks will be definition stacks by default. phase_4 will mark
     those that are referenced to be STACK_POSITION_CODE stacks */
  stack->position = STACK_POSITION_DEFINITION;

  for (q = 0; q < d; q++) {
    struct stack_item *out = &stack->stack_items[q], *in = &ta[q];
    int type = ta[q].type;
    
    out->can_calculate_deltas = in->can_calculate_deltas;
    out->has_been_replaced = in->has_been_replaced;
    out->is_in_postfix = NO;
    
    if (type == STACK_ITEM_TYPE_OPERATOR) {
      out->type = STACK_ITEM_TYPE_OPERATOR;
      out->value = in->value;
      out->sign = in->sign;
    }
    else if (type == STACK_ITEM_TYPE_VALUE) {
      out->type = STACK_ITEM_TYPE_VALUE;
      out->value = in->value;
      out->sign = in->sign;
    }
    else if (type == STACK_ITEM_TYPE_STACK) {
      out->type = STACK_ITEM_TYPE_STACK;
      out->value = in->value;
      out->sign = in->sign;
    }
    else if (type == STACK_ITEM_TYPE_STRING) {
      /* fail if we have a string inside a pending calculation! */
      print_error(ERROR_STC, "A string (\"%s\") inside a calculation doesn't make any sense...\n", in->string);
      return FAILED;
    }
    else if (type == STACK_ITEM_TYPE_LABEL) {
      out->type = STACK_ITEM_TYPE_LABEL;
      out->sign = in->sign;
      strcpy(out->string, in->string);
    }
    else {
      print_error(ERROR_STC, "Unhandled stack item type '%d' in _stack_calculate()! Please submit a bug report!\n", type);
      return FAILED;
    }
  }

#ifdef WLA_DEBUG
  debug_print_stack(stack->linenumber, g_last_stack_id, stack->stack_items, d, 0, stack);
#endif

  calculation_stack_insert(stack);

#ifdef WLA_DEBUG
    fprintf(stderr, "RETURN STACK %d\n", g_latest_stack);
    debug_print_stack(g_active_file_info_last->line_current, g_latest_stack, ta, d, 0, NULL);
#endif

  return INPUT_NUMBER_STACK;
}


static unsigned char s_stack_calculate_initialized = NO;
static struct stack_item *s_stack_calculate_si_pointers[2*MAX_STACK_CALCULATE_CALL_DEPTH];
static int s_stack_calculate_pointer_index = 0;


static struct stack_item *_stack_calculate_get_array(void) {

  /* out of arrays? */
  if (s_stack_calculate_pointer_index >= 2*MAX_STACK_CALCULATE_CALL_DEPTH)
    return NULL;

  if (s_stack_calculate_si_pointers[s_stack_calculate_pointer_index] == NULL)
    s_stack_calculate_si_pointers[s_stack_calculate_pointer_index] = calloc(sizeof(struct stack_item) * MAX_STACK_CALCULATOR_ITEMS, 1);
  
  return s_stack_calculate_si_pointers[s_stack_calculate_pointer_index++];
}


int stack_calculate_free_allocations(void) {

  int i;

  if (s_stack_calculate_initialized == NO)
    return SUCCEEDED;

  for (i = 0; i < 2*MAX_STACK_CALCULATE_CALL_DEPTH; i++)
    free(s_stack_calculate_si_pointers[i]);

  return SUCCEEDED;
}


int stack_calculate(char *in, int *value, int *bytes_parsed, unsigned char from_substitutor) {

  struct stack_item *si, *ta;
  int result;
  PROFILE_VARIABLES();

  PROFILE_START();
  
  if (s_stack_calculate_initialized == NO) {
    int i;

    for (i = 0; i < 2*MAX_STACK_CALCULATE_CALL_DEPTH; i++)
      s_stack_calculate_si_pointers[i] = NULL;

    s_stack_calculate_initialized = YES;
  }

  /* get arrays (or allocate them if they don't exist) */
  si = _stack_calculate_get_array();
  ta = _stack_calculate_get_array();

  if (si == NULL || ta == NULL) {
    print_error(ERROR_STC, "STACK_CALCULATE: Out of struct stack_item arrays. Please submit a bug report!\n");
    return FAILED;
  }

  result = _stack_calculate(in, value, bytes_parsed, from_substitutor, si, ta);

  /* release the arrays */
  s_stack_calculate_pointer_index -= 2;

  PROFILE_END("stack_calculate");
  
  return result;
}


static void _remove_can_calculate_deltas_pair(struct stack_item *s) {

  int i = 1;

  /* delta calculation has failed for this A-B pair -> set can_calculate_deltas to NO
     for convenience */
  
  s->can_calculate_deltas = NO;

  if (g_delta_counter == 0) {
    while (1) {
      if ((s+i)->type == STACK_ITEM_TYPE_LABEL) {
        (s+i)->can_calculate_deltas = NO;
        break;
      }
      i++;
    }
  }
  else {
    while (1) {
      if ((s-i)->type == STACK_ITEM_TYPE_LABEL) {
        (s-i)->can_calculate_deltas = NO;
        break;
      }
      i++;
    }
  }
  
  g_delta_counter = 0;
}


static int _resolve_string(struct stack_item *s, int *cannot_resolve) {

  struct definition *tmp_def;

  /*
  if (s->type == STACK_ITEM_TYPE_STRING)
    fprintf(stderr, "STR 1 ***%s***\n", s->string);
  if (s->type == STACK_ITEM_TYPE_LABEL)
    fprintf(stderr, "LBL 1 ***%s***\n", s->string);
  else if (s->type == STACK_ITEM_TYPE_VALUE)
    fprintf(stderr, "VAL 1 ***%d***\n", (int)s->value);
  else if (s->type == STACK_ITEM_TYPE_STACK)
    fprintf(stderr, "CAL 1 ***%d***\n", (int)s->value);
  else
    fprintf(stderr, "??? 1\n");
  */
  
  if (g_macro_active != 0) {
    /* expand e.g., \1 and \@ */
    if (expand_macro_arguments(s->string, NULL, NULL) == FAILED)
      return FAILED;
  }

  /*
  if (s->type == STACK_ITEM_TYPE_STRING)
    fprintf(stderr, "STR 2 ***%s***\n", s->string);
  if (s->type == STACK_ITEM_TYPE_LABEL)
    fprintf(stderr, "LBL 2 ***%s***\n", s->string);
  else if (s->type == STACK_ITEM_TYPE_VALUE)
    fprintf(stderr, "VAL 2 ***%d***\n", (int)s->value);
  else if (s->type == STACK_ITEM_TYPE_STACK)
    fprintf(stderr, "CAL 2 ***%d***\n", (int)s->value);
  else
    fprintf(stderr, "??? 2\n");
  */

  hashmap_get(g_defines_map, s->string, (void*)&tmp_def);
  if (tmp_def != NULL) {
    if (tmp_def->type == DEFINITION_TYPE_STRING) {
      if (g_input_parse_if == NO) {
        /* change the contents */
        s->type = STACK_ITEM_TYPE_STRING;
        strcpy(s->string, tmp_def->string);
        /*
        print_error(ERROR_STC, "Definition \"%s\" is a string definition.\n", tmp_def->alias);
        */
        return FAILED;
      }
      else {
        *cannot_resolve = 1;
        strcpy(s->string, tmp_def->string);
      }
    }
    else if (tmp_def->type == DEFINITION_TYPE_STACK) {
      /* turn this reference to a stack calculation define into a direct reference to the stack calculation as */
      /* this way we don't have to care if the define is exported or not as stack calculations are always exported */
      s->type = STACK_ITEM_TYPE_STACK;
      s->value = tmp_def->value;
    }
    else if (tmp_def->type == DEFINITION_TYPE_ADDRESS_LABEL) {
      /* wla cannot resolve address labels (unless outside a section) -> only wlalink can do that */
      *cannot_resolve = 1;
      strcpy(s->string, tmp_def->string);
    }
    else {
      s->type = STACK_ITEM_TYPE_VALUE;
      s->value = tmp_def->value;
    }

    if (s->can_calculate_deltas == YES) {
      /* delta calculation failed! */
      _remove_can_calculate_deltas_pair(s);
    }
  }
  else if (g_can_calculate_a_minus_b == YES) {
    if (s->can_calculate_deltas == YES) {
      /* the current calculation we are trying to solve contains at least one label pair A-B, and no other
         uses of labels. if we come here then a label wasn't a definition, but we can try to find the label's
         (possibly) non-final address, and use that as that should work when calculating deltas... */
      struct data_stream_item *dSI = NULL;

      /* read the labels and their addresses from the internal data stream */
      data_stream_parser_parse();

      if (s->type == STACK_ITEM_TYPE_LABEL)
        dSI = data_stream_parser_find_label(s->string, s_dsp_file_name_id, s_dsp_line_number);

      if (dSI != NULL) {
        if (g_delta_counter == 0) {
          g_delta_section = dSI->section_id;
          g_delta_address = dSI->address;

          /* store the pointer to a stack_item as we'll turn it into a value later if we can calculate the delta */
          g_delta_old_pointer = s;

          s->value = dSI->address;

          g_delta_counter = 1;
        }
        else if (g_delta_counter == 1) {
          _remove_can_calculate_deltas_pair(s);

          if (g_delta_section != dSI->section_id) {
            /* ABORT! labels A-B are from different sections, we cannot calculate the delta here... */
            *cannot_resolve = 1;
          }
          else {
            /* success! A-B makes sense! */
            s->type = STACK_ITEM_TYPE_VALUE;
            s->value = dSI->address;

            /* turn A to a value as well for the delta calculation to work */
            g_delta_old_pointer->type = STACK_ITEM_TYPE_VALUE;
          }
        }
      }
      else {
        /* ABORT! cannot find the label thus we fail here at calculating the delta... */
        *cannot_resolve = 1;

        _remove_can_calculate_deltas_pair(s);
      }
    }
  }

  process_special_labels(s->string);
  
  /* is this form "string".length? */
  if (is_string_ending_with(s->string, ".length") > 0 ||
      is_string_ending_with(s->string, ".LENGTH") > 0) {
    /* we have a X.length -> calculate */
    s->string[strlen(s->string) - 7] = 0;
    s->value = get_label_length(s->string);
    s->type = STACK_ITEM_TYPE_VALUE;
  }
  
  return SUCCEEDED;
}


static int _process_string(struct stack_item *s, int *cannot_resolve) {

  struct macro_argument *ma;
  int a, b, k, did_resolve_string = NO;
  char c;

  if (g_macro_active != 0 && strlen(s->string) > 1 && s->string[0] == '?' && s->string[1] >= '1' && s->string[1] <= '9') {
    for (a = 0, b = 0; s->string[a + 1] != 0 && a < 10; a++) {
      c = s->string[a + 1];
      if (c < '0' || c > '9')
        return FAILED;
      b = (b * 10) + (c - '0');
    }

    if (b > g_macro_runtime_current->supplied_arguments) {
      print_error(ERROR_NUM, "Referencing argument number %d inside .MACRO \"%s\". The .MACRO has only %d arguments.\n", b, g_macro_runtime_current->macro->name, g_macro_runtime_current->supplied_arguments);
      return FAILED;
    }
    if (b == 0) {
      print_error(ERROR_NUM, ".MACRO arguments are counted from 1.\n");
      return FAILED;
    }

    /* use the macro argument to find its definition */
    ma = g_macro_runtime_current->argument_data[b - 1];
    k = ma->type;

    if (k == INPUT_NUMBER_ADDRESS_LABEL) {
      strcpy(s->string, ma->string);

      did_resolve_string = YES;
      if (_resolve_string(s, cannot_resolve) == FAILED)
        return FAILED;
    }
    else {
      print_error(ERROR_ERR, "? can be only used to evaluate definitions.\n");
      return FAILED;
    }
  }
  else if (g_macro_active != 0 && s->string[0] == '\\') {
    if (s->string[1] == '@' && s->string[2] == 0) {
      s->type = STACK_ITEM_TYPE_VALUE;
      s->value = g_macro_runtime_current->macro->calls - 1;
    }
    else {
      int try_resolve_string = NO;
      
      for (a = 0, b = 0; s->string[a + 1] != 0 && a < 10; a++) {
        c = s->string[a + 1];
        if (c < '0' || c > '9') {
          try_resolve_string = YES;
          break;
        }
        b = (b * 10) + (c - '0');
      }

      if (try_resolve_string == YES) {
        did_resolve_string = YES;
        if (_resolve_string(s, cannot_resolve) == FAILED)
          return FAILED;
      }
      else {
        if (b > g_macro_runtime_current->supplied_arguments) {
          print_error(ERROR_STC, "Reference to .MACRO argument number %d (\"%s\") is out of range. The .MACRO has %d arguments.\n", b, s->string, g_macro_runtime_current->supplied_arguments);
          return FAILED;
        }
        if (b == 0) {
          print_error(ERROR_STC, ".MACRO arguments are counted from 1.\n");
          return FAILED;
        }
          
        /* return the macro argument */
        ma = g_macro_runtime_current->argument_data[b - 1];
        k = ma->type;
          
        if (k == INPUT_NUMBER_ADDRESS_LABEL)
          strcpy(g_label, ma->string);
        else if (k == INPUT_NUMBER_STRING) {
          strcpy(g_label, ma->string);
          g_string_size = (int)strlen(ma->string);
        }
        else if (k == INPUT_NUMBER_STACK)
          g_latest_stack = (int)ma->value;
        else if (k == INPUT_NUMBER_FLOAT) {
          g_parsed_int = (int)ma->value;
          g_parsed_double = ma->value;
        }
        else if (k == SUCCEEDED) {
          g_parsed_int = (int)ma->value;
          g_parsed_double = ma->value;
        }
          
        if (!(k == SUCCEEDED || k == INPUT_NUMBER_ADDRESS_LABEL || k == INPUT_NUMBER_STACK || k == INPUT_NUMBER_FLOAT))
          return FAILED;
          
        if (k == SUCCEEDED) {
          s->type = STACK_ITEM_TYPE_VALUE;
          s->value = g_parsed_double;
        }
        else if (k == INPUT_NUMBER_STACK) {
          s->type = STACK_ITEM_TYPE_STACK;
          s->value = g_latest_stack;
        }
        else if (k == INPUT_NUMBER_FLOAT) {
          s->type = STACK_ITEM_TYPE_VALUE;
          s->value = g_parsed_double;
        }
        else
          strcpy(s->string, g_label);

        if (s->can_calculate_deltas == YES) {
          /* delta calculation failed! */
          _remove_can_calculate_deltas_pair(s);
        }
      }
    }
  }
  else {
    did_resolve_string = YES;
    if (_resolve_string(s, cannot_resolve) == FAILED)
      return FAILED;
  }

  if (did_resolve_string == NO) {
    if (s->can_calculate_deltas == YES) {
      /* delta calculation failed! */
      _remove_can_calculate_deltas_pair(s);
    }
  }
  
  return SUCCEEDED;
}


static int _try_to_calculate(struct stack_item *st) {

  struct stack *s;

  s = find_stack_calculation((int)st->value, YES);
  if (s == NULL)
    return FAILED;

  if (s->has_been_calculated == YES) {
    st->type = STACK_ITEM_TYPE_VALUE;
    st->value = s->value;

    return SUCCEEDED;
  }

  if (resolve_stack(s->stack_items, s->stacksize) == SUCCEEDED) {
    double dou;

    if (compute_stack(s, s->stacksize, &dou) == FAILED)
      return FAILED;

    if (s->is_single_instance == YES) {
      free(s->stack_items);
      s->stack_items = NULL;
      s->stacksize = 0;
      s->has_been_calculated = YES;
      s->value = dou;

      /* HACK: don't export this calculation in phase_4.c */
      s->is_function_body = YES;
    }

    st->type = STACK_ITEM_TYPE_VALUE;
    st->value = dou;

    return SUCCEEDED;
  }

  return FAILED;
}


int resolve_stack(struct stack_item s[], int stack_item_count) {

  struct stack_item *st;
  int backup = stack_item_count, cannot_resolve = 0;

  st = s;
  while (stack_item_count > 0) {
    int process_single = YES;

    if (stack_item_count >= 3 && g_input_parse_if == YES) {
      /* [string] [string] ==/!=/</>/<=/>= ? */
      s += 2;
      stack_item_count -= 2;
      
      if (s->type == STACK_ITEM_TYPE_OPERATOR && (s->value == SI_OP_COMPARE_EQ || s->value == SI_OP_COMPARE_NEQ || s->value == SI_OP_COMPARE_LT ||
                                                  s->value == SI_OP_COMPARE_GT || s->value == SI_OP_COMPARE_LTE || s->value == SI_OP_COMPARE_GTE)) {
        s -= 2;
        stack_item_count += 2;

        if (s->type == STACK_ITEM_TYPE_LABEL) {
          int cannot;

          if (_process_string(s, &cannot) == FAILED)
            return FAILED;
        }
        
        s++;
        stack_item_count--;

        if (s->type == STACK_ITEM_TYPE_LABEL) {
          int cannot;

          if (_process_string(s, &cannot) == FAILED)
            return FAILED;
        }

        s += 2;
        stack_item_count -= 2;

        process_single = NO;
      }
      else {
        s -= 2;
        stack_item_count += 2;
      }
    }

    if (process_single == YES) {
      if (s->type == STACK_ITEM_TYPE_LABEL) {
        if (_process_string(s, &cannot_resolve) == FAILED)
          return FAILED;
      }
      s++;
      stack_item_count--;
    }
  }

  if (cannot_resolve != 0)
    return FAILED;

  /* find a string, a stack that cannot be calculated and turned into a value here, bank, or a NOT and fail */
  stack_item_count = backup;
  while (stack_item_count > 0) {
    int process_single = YES;

    if (stack_item_count >= 3 && g_input_parse_if == YES) {
      /* [string] [string] ==/!= ? */
      st += 2;
      stack_item_count -= 2;

      if (st->type == STACK_ITEM_TYPE_OPERATOR && (st->value == SI_OP_COMPARE_EQ || st->value == SI_OP_COMPARE_NEQ || st->value == SI_OP_COMPARE_LT ||
                                                   st->value == SI_OP_COMPARE_GT || st->value == SI_OP_COMPARE_LTE || st->value == SI_OP_COMPARE_GTE)) {
        st -= 2;
        stack_item_count += 2;

        if (st->type == STACK_ITEM_TYPE_STACK) {
          if (_try_to_calculate(st) == FAILED)
            return FAILED;
        }
        else if ((st->type == STACK_ITEM_TYPE_OPERATOR && st->value == SI_OP_BANK))
          return FAILED;

        st++;
        stack_item_count--;

        if (st->type == STACK_ITEM_TYPE_STACK) {
          if (_try_to_calculate(st) == FAILED)
            return FAILED;
        }
        else if ((st->type == STACK_ITEM_TYPE_OPERATOR && st->value == SI_OP_BANK))
          return FAILED;
        
        st += 2;
        stack_item_count -= 2;

        process_single = NO;
      }
      else {
        st -= 2;
        stack_item_count += 2;
      }
    }

    if (process_single == YES) {
      if (st->type == STACK_ITEM_TYPE_STACK) {
        if (_try_to_calculate(st) == FAILED)
          return FAILED;
      }
      if (st->type == STACK_ITEM_TYPE_STRING || st->type == STACK_ITEM_TYPE_LABEL || (st->type == STACK_ITEM_TYPE_OPERATOR && st->value == SI_OP_BANK))
        return FAILED;
      if (g_input_parse_if == NO && st->type == STACK_ITEM_TYPE_OPERATOR && st->value == SI_OP_NOT)
        return FAILED;
      stack_item_count--;
      st++;
    }
  }

  return SUCCEEDED;
}


static int _comparing_a_string_with_a_number(char *sp1, char *sp2, struct stack *sta) {

  if ((sp1 != NULL && sp2 == NULL) || (sp1 == NULL && sp2 != NULL)) {  
    fprintf(stderr, "%s:%d: COMPUTE_STACK: Comparison between a string and a number doesn't work.\n", get_file_name(sta->filename_id), sta->linenumber);
    return YES;
  }

  return NO;
}


static double _round(double d) {

  int i = (int)d;
  double delta = d - (double)i;

  if (delta < 0.0) {
    if (delta <= -0.5)
      return (double)(i - 1);
    else
      return (double)i;
  }
  else {
    if (delta < 0.5)
      return (double)i;
    else
      return (double)(i + 1);
  }
}


int compute_stack(struct stack *sta, int stack_item_count, double *result) {

  struct stack_item *s;
  double v[MAX_STACK_CALCULATOR_ITEMS];
  char *sp[MAX_STACK_CALCULATOR_ITEMS];
  int r, t, z;

  if (sta->has_been_calculated == YES) {
    *result = sta->value;
    return SUCCEEDED;
  }

  v[0] = 0.0;

  s = sta->stack_items;

#ifdef WLA_DEBUG
  fprintf(stderr, "SOLVING:\n");
  debug_print_stack(0, 0, s, stack_item_count, 0, NULL);
#endif
  
  for (r = 0, t = 0; r < stack_item_count; r++, s++) {
    if (s->type == STACK_ITEM_TYPE_VALUE) {
      if (s->sign == SI_SIGN_NEGATIVE)
        v[t] = -s->value;
      else
        v[t] = s->value;
      sp[t] = NULL;
      t++;
    }
    else if (s->type == STACK_ITEM_TYPE_LABEL || s->type == STACK_ITEM_TYPE_STRING) {
      sp[t] = s->string;
      v[t] = 0;
      t++;
    }
    else {
      switch ((int)s->value) {
      case SI_OP_ADD:
        v[t - 2] += v[t - 1];
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_SUB:
        v[t - 2] -= v[t - 1];
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_MULTIPLY:
        if (t <= 1) {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: Multiply is missing an operand.\n", get_file_name(sta->filename_id), sta->linenumber);
          return FAILED;
        }
        v[t - 2] *= v[t - 1];
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_LOW_BYTE:
        z = (int)v[t - 1];
#ifdef AMIGA
        /* on Amiga this needs to be done twice - a bug in SAS/C? */
        z = z & 0xFF;
#endif
        v[t - 1] = z & 0xFF;
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_HIGH_BYTE:
        z = ((int)v[t - 1]) >> 8;
#ifdef AMIGA
        /* on Amiga this needs to be done twice - a bug in SAS/C? */
        z = z & 0xFF;
#endif
        v[t - 1] = z & 0xFF;
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_BANK_BYTE:
        z = ((int)v[t - 1]) >> 16;
#ifdef AMIGA
        /* on Amiga this needs to be done twice - a bug in SAS/C? */
        z = z & 0xFF;
#endif
        v[t - 1] = z & 0xFF;
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
     case SI_OP_LOW_WORD:
        z = (int)v[t - 1];
#ifdef AMIGA
        /* on Amiga this needs to be done twice - a bug in SAS/C? */
        z = z & 0xFFFF;
#endif
        v[t - 1] = z & 0xFFFF;
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_HIGH_WORD:
        z = ((int)v[t - 1]) >> 16;
#ifdef AMIGA
        /* on Amiga this needs to be done twice - a bug in SAS/C? */
        z = z & 0xFFFF;
#endif
        v[t - 1] = z & 0xFFFF;
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_ROUND:
        v[t - 1] = _round(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_CEIL:
        v[t - 1] = ceil(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_FLOOR:
        v[t - 1] = floor(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_MIN:
        if (v[t - 1] < v[t - 2])
          v[t - 2] = v[t - 1];
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 2] = -v[t - 2];
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_MAX:
        if (v[t - 1] > v[t - 2])
          v[t - 2] = v[t - 1];
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 2] = -v[t - 2];
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_ABS:
        v[t - 1] = fabs(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_SIGN:
        if (v[t - 1] == 0.0)
          v[t - 1] = 0.0;
        else if (v[t - 1] < 0.0)
          v[t - 1] = -1.0;
        else
          v[t - 1] = 1.0;
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_CLAMP:
        if (v[t - 3] < v[t - 2])
          v[t - 3] = v[t - 2];
        else if (v[t - 3] > v[t - 1])
          v[t - 3] = v[t - 1];
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 3] = -v[t - 3];
        sp[t - 3] = NULL;
        t -= 2;
        break;
      case SI_OP_COS:
        v[t - 1] = cos(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_SIN:
        v[t - 1] = sin(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_TAN:
        v[t - 1] = tan(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_LOG:
        v[t - 1] = log(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_LOG10:
        v[t - 1] = log10(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_COSH:
        v[t - 1] = cosh(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_SINH:
        v[t - 1] = sinh(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_TANH:
        v[t - 1] = tanh(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_ACOS:
        if (v[t - 1] < -1.0 || v[t - 1] > 1.0) {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: acos() needs a value that is [-1.0, 1.0], %f doesn't work!\n", get_file_name(sta->filename_id), sta->linenumber, v[t - 1]);
          return FAILED;
        }
        v[t - 1] = acos(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_ASIN:
        if (v[t - 1] < -1.0 || v[t - 1] > 1.0) {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: asin() needs a value that is [-1.0, 1.0], %f doesn't work!\n", get_file_name(sta->filename_id), sta->linenumber, v[t - 1]);
          return FAILED;
        }
        v[t - 1] = asin(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_ATAN:
        v[t - 1] = atan(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_ATAN2:
        v[t - 2] = atan2(v[t - 2], v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 2] = -v[t - 2];
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_POW:
        v[t - 2] = pow(v[t - 2], v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 2] = -v[t - 2];
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_SQRT:
        if (v[t - 1] < 0.0) {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: sqrt() needs a value that is >= 0.0, %f doesn't work!\n", get_file_name(sta->filename_id), sta->linenumber, v[t - 1]);
          return FAILED;
        }
        v[t - 1] = sqrt(v[t - 1]);
        if (s->sign == SI_SIGN_NEGATIVE)
          v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_LOGICAL_OR:
        if (v[t-1] != 0 || v[t-2] != 0)
          v[t-2] = 1;
        else
          v[t-2] = 0;
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_LOGICAL_AND:
        if (v[t-1] != 0 && v[t-2] != 0)
          v[t-2] = 1;
        else
          v[t-2] = 0;
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_COMPARE_LT:
        if (_comparing_a_string_with_a_number(sp[t-2], sp[t-1], sta) == YES)
          return FAILED;
        if (sp[t-2] != NULL && sp[t-1] != NULL) {
          if (strcmp(sp[t-2], sp[t-1]) < 0)
            v[t-2] = 1;
          else
            v[t-2] = 0;
        }
        else {
          if (v[t-2] < v[t-1])
            v[t-2] = 1;
          else
            v[t-2] = 0;
        }
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_COMPARE_GT:
        if (_comparing_a_string_with_a_number(sp[t-2], sp[t-1], sta) == YES)
          return FAILED;
        if (sp[t-2] != NULL && sp[t-1] != NULL) {
          if (strcmp(sp[t-2], sp[t-1]) > 0)
            v[t-2] = 1;
          else
            v[t-2] = 0;
        }
        else {
          if (v[t-2] > v[t-1])
            v[t-2] = 1;
          else
            v[t-2] = 0;
        }
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_COMPARE_EQ:
        if (_comparing_a_string_with_a_number(sp[t-2], sp[t-1], sta) == YES)
          return FAILED;
        if (sp[t-2] != NULL && sp[t-1] != NULL) {
          if (strcmp(sp[t-2], sp[t-1]) == 0)
            v[t-2] = 1;
          else
            v[t-2] = 0;
        }
        else {
          if (v[t-2] == v[t-1])
            v[t-2] = 1;
          else
            v[t-2] = 0;
        }
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_COMPARE_NEQ:
        if (_comparing_a_string_with_a_number(sp[t-2], sp[t-1], sta) == YES)
          return FAILED;
        if (sp[t-2] != NULL && sp[t-1] != NULL) {
          if (strcmp(sp[t-2], sp[t-1]) != 0)
            v[t-2] = 1;
          else
            v[t-2] = 0;
        }
        else {
          if (v[t-2] != v[t-1])
            v[t-2] = 1;
          else
            v[t-2] = 0;
        }
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_COMPARE_LTE:
        if (_comparing_a_string_with_a_number(sp[t-2], sp[t-1], sta) == YES)
          return FAILED;
        if (sp[t-2] != NULL && sp[t-1] != NULL) {
          if (strcmp(sp[t-2], sp[t-1]) <= 0)
            v[t-2] = 1;
          else
            v[t-2] = 0;
        }
        else {
          if (v[t-2] <= v[t-1])
            v[t-2] = 1;
          else
            v[t-2] = 0;
        }
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_COMPARE_GTE:
        if (_comparing_a_string_with_a_number(sp[t-2], sp[t-1], sta) == YES)
          return FAILED;
        if (sp[t-2] != NULL && sp[t-1] != NULL) {
          if (strcmp(sp[t-2], sp[t-1]) >= 0)
            v[t-2] = 1;
          else
            v[t-2] = 0;
        }
        else {
          if (v[t-2] >= v[t-1])
            v[t-2] = 1;
          else
            v[t-2] = 0;
        }
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_NEGATE:
        v[t - 1] = -v[t - 1];
        sp[t - 1] = NULL;
        break;
      case SI_OP_NOT:
        if (g_input_parse_if == YES) {
          if ((int)v[t - 1] == 0)
            v[t - 1] = 1;
          else
            v[t - 1] = 0;
          sp[t - 1] = NULL;
        }
        else {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: NOT cannot determine the output size.\n", get_file_name(sta->filename_id), sta->linenumber);
          return FAILED;
        }
        break;
      case SI_OP_XOR:
        if (t <= 1) {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: XOR is missing an operand.\n", get_file_name(sta->filename_id), sta->linenumber);
          return FAILED;
        }
        v[t - 2] = (int)v[t - 1] ^ (int)v[t - 2];
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_OR:
        if (t <= 1) {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: OR is missing an operand.\n", get_file_name(sta->filename_id), sta->linenumber);
          return FAILED;
        }
        v[t - 2] = (int)v[t - 1] | (int)v[t - 2];
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_AND:
        if (t <= 1) {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: AND is missing an operand.\n", get_file_name(sta->filename_id), sta->linenumber);
          return FAILED;
        }
        v[t - 2] = (int)v[t - 1] & (int)v[t - 2];
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_MODULO:
        if (((int)v[t - 1]) == 0) {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: Modulo by zero.\n", get_file_name(sta->filename_id), sta->linenumber);
          return FAILED;
        }
        if (t <= 1) {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: Modulo is missing an operand.\n", get_file_name(sta->filename_id), sta->linenumber);
          return FAILED;
        }
        v[t - 2] = (int)v[t - 2] % (int)v[t - 1];
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_DIVIDE:
        if (v[t - 1] == 0.0) {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: Division by zero.\n", get_file_name(sta->filename_id), sta->linenumber);
          return FAILED;
        }
        if (t <= 1) {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: Division is missing an operand.\n", get_file_name(sta->filename_id), sta->linenumber);
          return FAILED;
        }
        v[t - 2] /= v[t - 1];
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_POWER:
        if (t <= 1) {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: Power is missing an operand.\n", get_file_name(sta->filename_id), sta->linenumber);
          return FAILED;
        }
        v[t - 2] = pow(v[t - 2], v[t - 1]);
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_SHIFT_LEFT:
        if (t <= 1) {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: Shift left is missing an operand.\n", get_file_name(sta->filename_id), sta->linenumber);
          return FAILED;
        }
        v[t - 2] = (int)v[t - 2] << (int)v[t - 1];
        sp[t - 2] = NULL;
        t--;
        break;
      case SI_OP_SHIFT_RIGHT:
        if (t <= 1) {
          fprintf(stderr, "%s:%d: COMPUTE_STACK: Shift right is missing an operand.\n", get_file_name(sta->filename_id), sta->linenumber);
          return FAILED;
        }
        v[t - 2] = (int)v[t - 2] >> (int)v[t - 1];
        sp[t - 2] = NULL;
        t--;
        break;
      }
    }
  }

  /*
    #ifdef W65816
    if (v[0] < -8388608 || v[0] > 16777215) {
    print_error(ERROR_STC, "Out of 24-bit range.\n");
    return FAILED;
    }
    #else
    if (v[0] < -32768 || v[0] > 65536) {
    print_error(ERROR_STC, "Out of 16-bit range.\n");
    return FAILED;
    }
    #endif
  */

  *result = v[0];

#ifdef WLA_DEBUG
  fprintf(stderr, "RESULT = %f\n", *result);
#endif
  
  return SUCCEEDED;
}


int stack_create_label_stack(char *label) {

  struct stack *stack;
  struct stack_item *si;
  
  if (label == NULL)
    return FAILED;

  /* we need to create a stack that holds just one label */
  stack = allocate_struct_stack(1);
  if (stack == NULL)
    return FAILED;

  stack->linenumber = g_active_file_info_last->line_current;
  stack->filename_id = g_active_file_info_last->filename_id;
    
  /* all stacks will be definition stacks by default. phase_4 will mark
     those that are referenced to be STACK_POSITION_CODE stacks */
  stack->position = STACK_POSITION_DEFINITION;

  si = &stack->stack_items[0];
  si->type = STACK_ITEM_TYPE_LABEL;
  si->sign = SI_SIGN_POSITIVE;
  si->can_calculate_deltas = NO;
  si->has_been_replaced = NO;
  si->is_in_postfix = NO;
  strcpy(si->string, label);

  calculation_stack_insert(stack);

#ifdef WLA_DEBUG
  debug_print_stack(stack->linenumber, g_last_stack_id, stack->stack_items, 1, 1, stack);
#endif

  return SUCCEEDED;
}


int stack_create_stack_stack(int stack_id) {

  struct stack *stack;
  struct stack_item *si;
  
  /* we need to create a stack that holds just one computation stack */
  stack = allocate_struct_stack(1);
  if (stack == NULL)
    return FAILED;

  stack->linenumber = g_active_file_info_last->line_current;
  stack->filename_id = g_active_file_info_last->filename_id;
  
  /* all stacks will be definition stacks by default. phase_4 will mark
     those that are referenced to be STACK_POSITION_CODE stacks */
  stack->position = STACK_POSITION_DEFINITION;

  si = &stack->stack_items[0];
  si->type = STACK_ITEM_TYPE_STACK;
  si->value = stack_id;
  si->sign = SI_SIGN_POSITIVE;
  si->can_calculate_deltas = NO;
  si->has_been_replaced = NO;
  si->is_in_postfix = NO;

#if WLA_DEBUG
  debug_print_stack(stack->linenumber, stack_id, stack->stack_items, 1, 2, stack);
#endif

  calculation_stack_insert(stack);
    
  return SUCCEEDED;
}


#if defined(MC68000)

static int _stack_create_stack_caddr_offset(int type, int data, char *label, struct stack *stack) {

  struct stack_item *si;

  /* 0 */
  si = &stack->stack_items[0];
  si->value = data;

  if (type == SUCCEEDED)
    si->type = STACK_ITEM_TYPE_VALUE;
  else if (type == INPUT_NUMBER_ADDRESS_LABEL) {
    si->type = STACK_ITEM_TYPE_LABEL;
    strcpy(si->string, label);
  }
  else if (type == INPUT_NUMBER_STACK)
    si->type = STACK_ITEM_TYPE_STACK;
  else {
    fprintf(stderr, "%s:%d: STACK_CREATE_STACK_CADDR_OFFSET: Unhandled data type %d! Please submit a bug report!\n", get_file_name(stack->filename_id), stack->linenumber, type);
    return FAILED;
  }
  
  si->sign = SI_SIGN_POSITIVE;
  si->can_calculate_deltas = NO;
  si->has_been_replaced = NO;
  si->is_in_postfix = YES;

  /* 1 */
  si = &stack->stack_items[1];
  si->type = STACK_ITEM_TYPE_LABEL;
  si->value = 0;
  strcpy(si->string, "CADDR");
  si->sign = SI_SIGN_POSITIVE;
  si->can_calculate_deltas = NO;
  si->has_been_replaced = NO;
  si->is_in_postfix = YES;
  
  /* 2 */
  si = &stack->stack_items[2];
  si->type = STACK_ITEM_TYPE_OPERATOR;
  si->value = SI_OP_SUB;
  si->sign = SI_SIGN_POSITIVE;
  si->can_calculate_deltas = NO;
  si->has_been_replaced = NO;
  si->is_in_postfix = YES;

  return SUCCEEDED;
}


int stack_create_stack_caddr_offset(int type, int data, char *label) {

  struct stack *stack;
  
  stack = allocate_struct_stack(3);
  if (stack == NULL)
    return FAILED;

  stack->linenumber = g_active_file_info_last->line_current;
  stack->filename_id = g_active_file_info_last->filename_id;
  
  /* all stacks will be definition stacks by default. phase_4 will mark
     those that are referenced to be STACK_POSITION_CODE stacks */
  stack->position = STACK_POSITION_DEFINITION;

  if (_stack_create_stack_caddr_offset(type, data, label, stack) == FAILED)
    return FAILED;

  calculation_stack_insert(stack);
    
#if WLA_DEBUG
  debug_print_stack(stack->linenumber, g_latest_stack, stack->stack_items, 3, -1, stack);
#endif

  return SUCCEEDED;
}


int does_stack_contain_one_label(int id) {

  struct stack *stack = find_stack_calculation(id, YES);
  int i, labels = 0;
  
  if (stack == NULL)
    return NO;

  for (i = 0; i < stack->stacksize; i++) {
    if (stack->stack_items[i].type == STACK_ITEM_TYPE_LABEL)
      labels++;
  }

  if (labels == 1)
    return YES;
  else
    return NO;
}


int stack_add_offset_plus_n_to_stack(int id, int n) {

  struct stack *stack = find_stack_calculation(id, YES);
  struct stack_item *si;

  if (stack == NULL)
    return FAILED;

  stack->stacksize += 2;
  stack->stack_items = realloc(stack->stack_items, sizeof(struct stack_item) * stack->stacksize);
  if (stack->stack_items == NULL) {
    print_error(ERROR_STC, "Out of memory error while allocating room for a new calculation stack.\n");
    return FAILED;
  }
  
  /* x */
  si = &stack->stack_items[stack->stacksize - 2];
  si->type = STACK_ITEM_TYPE_VALUE;
  si->value = n;
  si->sign = SI_SIGN_POSITIVE;
  si->can_calculate_deltas = NO;
  si->has_been_replaced = NO;
  si->is_in_postfix = YES;
  
  /* x+1 */
  si = &stack->stack_items[stack->stacksize - 1];
  si->type = STACK_ITEM_TYPE_OPERATOR;
  si->value = SI_OP_ADD;
  si->sign = SI_SIGN_POSITIVE;
  si->can_calculate_deltas = NO;
  si->has_been_replaced = NO;
  si->is_in_postfix = YES;

#if WLA_DEBUG
  debug_print_stack(stack->linenumber, id, stack->stack_items, stack->stacksize, -1, stack);
#endif

  return SUCCEEDED;
}


int stack_create_stack_caddr_offset_plus_n(int type, int data, char *label, int n) {

  struct stack *stack;
  struct stack_item *si;
  
  stack = allocate_struct_stack(5);
  if (stack == NULL)
    return FAILED;

  stack->linenumber = g_active_file_info_last->line_current;
  stack->filename_id = g_active_file_info_last->filename_id;
  
  /* all stacks will be definition stacks by default. phase_4 will mark
     those that are referenced to be STACK_POSITION_CODE stacks */
  stack->position = STACK_POSITION_DEFINITION;

  if (_stack_create_stack_caddr_offset(type, data, label, stack) == FAILED)
    return FAILED;

  /* 3 */
  si = &stack->stack_items[3];
  si->type = STACK_ITEM_TYPE_VALUE;
  si->value = n;
  si->sign = SI_SIGN_POSITIVE;
  si->can_calculate_deltas = NO;
  si->has_been_replaced = NO;
  si->is_in_postfix = YES;
  
  /* 4 */
  si = &stack->stack_items[4];
  si->type = STACK_ITEM_TYPE_OPERATOR;
  si->value = SI_OP_ADD;
  si->sign = SI_SIGN_POSITIVE;
  si->can_calculate_deltas = NO;
  si->has_been_replaced = NO;
  si->is_in_postfix = YES;

  calculation_stack_insert(stack);
    
#if WLA_DEBUG
  debug_print_stack(stack->linenumber, g_latest_stack, stack->stack_items, 5, -1, stack);
#endif

  return SUCCEEDED;
}

#endif


/* TODO: move the following code to its own file... */

#include "phase_3.h"

#define XSTRINGIFY(x) #x
#define STRINGIFY(x) XSTRINGIFY(x)
#define STRING_READ_FORMAT ("%" STRINGIFY(MAX_NAME_LENGTH) "s ")

struct data_stream_item *g_data_stream_items_first = NULL, *g_data_stream_items_last = NULL;

extern struct section_def *g_sections_first, *g_sections_last, *g_sec_tmp, *g_sec_next;
extern char g_namespace[MAX_NAME_LENGTH + 1];
extern FILE *g_file_out_ptr;

/* internal variables for data_stream_parser_parse(), saved here so that the function can continue next time it's called from
   where it left off previous call */
static struct data_stream_item *s_dsp_parent_labels[10];
static int s_dsp_last_data_stream_position = 0, s_dsp_has_data_stream_parser_been_initialized = NO;
static int s_dsp_add = 0, s_dsp_add_old = 0, s_dsp_section_id = -1, s_dsp_bits_current = 0, s_dsp_inz;
static int s_dstruct_start, s_dstruct_item_offset, s_dstruct_item_size;
static struct section_def *s_dsp_s = NULL;
static struct map_t *s_dsp_labels_map = NULL;


struct section_def *data_stream_parser_get_current_section(void) {

  return s_dsp_s;
}


int data_stream_parser_get_current_address(void) {

  return s_dsp_add;
}


int data_stream_parser_free(void) {

  if (s_dsp_labels_map != NULL) {
    hashmap_free(s_dsp_labels_map);
    s_dsp_labels_map = NULL;
  }
  
  while (g_data_stream_items_first != NULL) {
    struct data_stream_item *next = g_data_stream_items_first->next;
    free(g_data_stream_items_first);
    g_data_stream_items_first = next;
  }

  g_data_stream_items_first = NULL;
  g_data_stream_items_last = NULL;

  return SUCCEEDED;
}


int data_stream_parser_parse(void) {

  char c;
  
  if (g_file_out_ptr == NULL) {
    print_error(ERROR_STC, "The internal data stream is closed! It should be open. Please submit a bug report!\n");
    return FAILED;
  }

  if (s_dsp_has_data_stream_parser_been_initialized == NO) {
    /* do the init when we first time come here */
    s_dsp_has_data_stream_parser_been_initialized = YES;
    
    memset(s_dsp_parent_labels, 0, sizeof(s_dsp_parent_labels));
    g_namespace[0] = 0;

    s_dsp_labels_map = hashmap_new();
  
    /* seek to the beginning of the file for parsing */
    fseek(g_file_out_ptr, 0, SEEK_SET);
  }
  else
    fseek(g_file_out_ptr, s_dsp_last_data_stream_position, SEEK_SET);

  /* parse the internal data stream to find labels and their possibly non-final addresses */
  
  while (fread(&c, 1, 1, g_file_out_ptr) != 0) {
    switch (c) {

    case ' ':
    case 'E':
      continue;

    case 'j':
      continue;
    case 'J':
      continue;

    case 'i':
      fscanf(g_file_out_ptr, "%*d %*s ");
      continue;
    case 'I':
      fscanf(g_file_out_ptr, "%*d %*s ");
      continue;

    case 'P':
      s_dsp_add_old = s_dsp_add;
      continue;
    case 'p':
      s_dsp_add = s_dsp_add_old;
      continue;

    case 'A':
    case 'S':
      if (c == 'A')
        fscanf(g_file_out_ptr, "%d %*d", &s_dsp_section_id);
      else
        fscanf(g_file_out_ptr, "%d ", &s_dsp_section_id);

      s_dsp_add_old = s_dsp_add;

      s_dsp_s = g_sections_first;
      while (s_dsp_s != NULL && s_dsp_s->id != s_dsp_section_id)
        s_dsp_s = s_dsp_s->next;

      if (s_dsp_s == NULL) {
        print_error(ERROR_ERR, "Section with ID \"%d\" has gone missing! Please submit a bug report!\n", s_dsp_section_id);
        return FAILED;
      }

      if (s_dsp_s->status == SECTION_STATUS_FREE || s_dsp_s->status == SECTION_STATUS_RAM_FREE || s_dsp_s->status == SECTION_STATUS_SEMISUPERFREE) {
        s_dsp_add = 0;
        s_dsp_s->address_from_dsp = 0;
      }
      else {
        if (s_dsp_s->address < 0)
          s_dsp_s->address_from_dsp = s_dsp_add;
        else {
          s_dsp_add = s_dsp_s->address;
          s_dsp_s->address_from_dsp = s_dsp_add;
        }
      }
      
      continue;

    case 's':
      s_dsp_s->size = s_dsp_add - s_dsp_s->address_from_dsp;
        
      if (s_dsp_s->advance_org == NO)
        s_dsp_add = s_dsp_add_old;
      else
        s_dsp_add = s_dsp_add_old + s_dsp_s->size;
      
      s_dsp_section_id = -1;
      s_dsp_s = NULL;
      continue;

    case 'x':
    case 'o':
      fscanf(g_file_out_ptr, "%d %*d ", &s_dsp_inz);
      s_dsp_add += s_dsp_inz;
      continue;

    case 'X':
      fscanf(g_file_out_ptr, "%d %*d ", &s_dsp_inz);
      s_dsp_add += s_dsp_inz * 2;
      continue;

    case 'h':
      fscanf(g_file_out_ptr, "%d %*d ", &s_dsp_inz);
      s_dsp_add += s_dsp_inz * 3;
      continue;

    case 'w':
      fscanf(g_file_out_ptr, "%d %*d ", &s_dsp_inz);
      s_dsp_add += s_dsp_inz * 4;
      continue;

    case 'z':
    case 'q':
      fscanf(g_file_out_ptr, "%*s ");
      s_dsp_add += 3;
      continue;

    case 'T':
      fscanf(g_file_out_ptr, "%*d ");
      s_dsp_add += 3;
      continue;

    case 'u':
    case 'V':
      fscanf(g_file_out_ptr, "%*s ");
      s_dsp_add += 4;
      continue;

    case 'U':
      fscanf(g_file_out_ptr, "%*d ");
      s_dsp_add += 4;
      continue;

    case 'v':
      fscanf(g_file_out_ptr, "%*d ");
      continue;
        
    case 'b':
      fscanf(g_file_out_ptr, "%*d ");
      continue;

    case 'R':
    case 'Q':
    case 'd':
    case 'c':
      fscanf(g_file_out_ptr, "%*s ");
      s_dsp_add++;
      continue;

    case 'M':
    case 'r':
      fscanf(g_file_out_ptr, "%*s ");
      s_dsp_add += 2;
      continue;

    case 'y':
    case 'C':
      fscanf(g_file_out_ptr, "%*d ");
      s_dsp_add += 2;
      continue;

#ifdef SUPERFX
    case '*':
      fscanf(g_file_out_ptr, "%*s ");
      s_dsp_add++;
      continue;
      
    case '-':
      fscanf(g_file_out_ptr, "%*d ");
      s_dsp_add++;
      continue;
#endif

    case '+':
      {
        int bits_to_add;
        char type;
          
        fscanf(g_file_out_ptr, "%d ", &bits_to_add);

        if (bits_to_add == 999) {
          s_dsp_bits_current = 0;

          continue;
        }
        else {
          if (s_dsp_bits_current == 0)
            s_dsp_add++;
          s_dsp_bits_current += bits_to_add;
          while (s_dsp_bits_current > 8) {
            s_dsp_bits_current -= 8;
            s_dsp_add++;
          }
          if (bits_to_add == 8)
            s_dsp_bits_current = 0;
        }

        fscanf(g_file_out_ptr, "%c", &type);

        if (type == 'a')
          fscanf(g_file_out_ptr, "%*d");
        else if (type == 'b')
          fscanf(g_file_out_ptr, "%*s");
        else if (type == 'c')
          fscanf(g_file_out_ptr, "%*d");

        continue;
      }

#ifdef SPC700
    case 'n':
      fscanf(g_file_out_ptr, "%*d %*s ");
      s_dsp_add += 2;
      continue;

    case 'N':
      fscanf(g_file_out_ptr, "%*d %*d ");
      s_dsp_add += 2;
      continue;
#endif

    case 'D':
      fscanf(g_file_out_ptr, "%*d %*d %*d %d ", &s_dsp_inz);
      s_dsp_add += s_dsp_inz;
      continue;

    case 'O':
      fscanf(g_file_out_ptr, "%d ", &s_dsp_add);
      continue;

    case 'B':
      fscanf(g_file_out_ptr, "%*d %*d ");
      continue;

    case 'g':
      fscanf(g_file_out_ptr, "%*d ");
      continue;

    case 'G':
      continue;

    case 't':
      fscanf(g_file_out_ptr, "%d ", &s_dsp_inz);
      if (s_dsp_inz == 0)
        g_namespace[0] = 0;
      else
        fscanf(g_file_out_ptr, STRING_READ_FORMAT, g_namespace);
      continue;

    case 'Z': /* breakpoint */
    case 'Y': /* symbol */
    case 'L': /* label */
      if (c == 'Z') {
      }
      else if (c == 'Y')
        fscanf(g_file_out_ptr, "%*s ");
      else {
        struct data_stream_item *dSI;
        int mangled_label = NO;

        dSI = calloc(sizeof(struct data_stream_item), 1);
        if (dSI == NULL) {
          print_error(ERROR_ERR, "Out of memory error while allocating a data_stream_item.\n");
          return FAILED;
        }

        fscanf(g_file_out_ptr, "%s ", dSI->label);

        if (is_label_anonymous(dSI->label) == YES) {
          /* we skip anonymous labels here, too much trouble */
          free(dSI);
        }
        else {
          /* if the label has '@' at the start, mangle the label name to make it unique */
          int n = 0, m;

          while (n < 10 && dSI->label[n] == '@')
            n++;
          m = n;
          while (m < 10)
            s_dsp_parent_labels[m++] = NULL;

          if (n < 10)
            s_dsp_parent_labels[n] = dSI;
          n--;
          while (n >= 0 && s_dsp_parent_labels[n] == 0)
            n--;

          if (n >= 0) {
            /* mangle the label so that we'll save only full forms of labels */
            if (mangle_label(dSI->label, s_dsp_parent_labels[n]->label, n, MAX_NAME_LENGTH, s_dsp_file_name_id, s_dsp_line_number) == FAILED)
              return FAILED;

            mangled_label = YES;
          }

          if (is_label_anonymous(dSI->label) == NO && g_namespace[0] != 0 && mangled_label == NO) {
            if (s_dsp_section_id < 0 || s_dsp_s->nspace == NULL) {
              if (add_namespace(dSI->label, g_namespace, sizeof(dSI->label), s_dsp_file_name_id, s_dsp_line_number) == FAILED)
                return FAILED;
            }
          }
        
          dSI->next = NULL;
          dSI->address = s_dsp_add;
          dSI->section_id = s_dsp_section_id;

          /* store the entry in a hashmap for quick discovery */
          if (hashmap_put(s_dsp_labels_map, dSI->label, dSI) == MAP_OMEM) {
            fprintf(stderr, "data_stream_parser_parse(): Out of memory error while trying to insert label \"%s\" into a hashmap.\n", dSI->label);
            return FAILED;
          }

          /* store the entry in a linked list so we can free it later */
          if (g_data_stream_items_first == NULL) {
            g_data_stream_items_first = dSI;
            g_data_stream_items_last = dSI;
          }
          else {
            g_data_stream_items_last->next = dSI;
            g_data_stream_items_last = dSI;
          }
        }
      }

      continue;

    case 'f':
      fscanf(g_file_out_ptr, "%d ", &s_dsp_file_name_id);
      continue;

    case 'k':
      fscanf(g_file_out_ptr, "%d ", &s_dsp_line_number);
      continue;

    case 'e':
      {
        int x, y;
        
        fscanf(g_file_out_ptr, "%d %d ", &x, &y);
        if (y == -1) {
          /* mark start of .DSTRUCT */
          s_dstruct_start = s_dsp_add;
          s_dstruct_item_offset = -1;
        }
        else {
          if (s_dstruct_item_offset != -1 && s_dsp_add - s_dstruct_item_offset > s_dstruct_item_size) {
            fprintf(stderr, "%s:%d: INTERNAL_PHASE_1: %d too many bytes in struct field.\n", get_file_name(s_dsp_file_name_id), s_dsp_line_number, (s_dsp_add - s_dstruct_item_offset) - s_dstruct_item_size);
            return FAILED;
          }
          
          s_dsp_add = s_dstruct_start + x;
          if (y < 0)
            s_dstruct_item_offset = -1;
          else {
            s_dstruct_item_offset = s_dsp_add;
            s_dstruct_item_size = y;
          }
        }
      }
      continue;

    default:
      fprintf(stderr, "data_stream_parser_parse(): Unknown internal symbol \"%c\". Please submit a bug report!\n", c);
      return FAILED;
    }
  }

  /* remember the data stream position for the next time this function is called */
  s_dsp_last_data_stream_position = (int)ftell(g_file_out_ptr);
  
  /* seek to the very end of the file so we can continue writing to it */
  fseek(g_file_out_ptr, 0, SEEK_END);
  
  return SUCCEEDED;
}


struct data_stream_item *data_stream_parser_find_label(char *label, int file_name_id, int line_number) {

  char mangled_label[MAX_NAME_LENGTH+1];
  struct data_stream_item *dSI;

  strcpy(mangled_label, label);
  
  if (is_label_anonymous(label) == NO) {
    /* if the label has '@' at the start, mangle the label name to get its full form */
    int n = 0;

    while (n < 10 && label[n] == '@')
      n++;
    n--;

    if (n >= 0) {
      if (s_dsp_parent_labels[n] == NULL) {
        fprintf(stderr, "DATA_STREAM_PARSER_FIND_LABEL: Parent of label \"%s\" is missing! Please submit a bug report!\n", label);
        return NULL;
      }
      if (mangle_label(mangled_label, s_dsp_parent_labels[n]->label, n, MAX_NAME_LENGTH, file_name_id, line_number) == FAILED)
        return NULL;
    }
  }

  hashmap_get(s_dsp_labels_map, mangled_label, (void *)&dSI);

  return dSI;
}

