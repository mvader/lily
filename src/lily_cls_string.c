#include <ctype.h>
#include <string.h>

#include "lily_impl.h"
#include "lily_value.h"
#include "lily_vm.h"

static lily_string_val *try_make_sv(int size)
{
    lily_string_val *new_sv = lily_malloc(sizeof(lily_string_val));
    char *new_string = lily_malloc(sizeof(char) * size);

    if (new_sv == NULL || new_string == NULL) {
        lily_free(new_sv);
        lily_free(new_string);
        return NULL;
    }

    new_sv->string = new_string;
    new_sv->size = size - 1;
    new_sv->refcount = 1;

    return new_sv;
}

/*  lily_string_concat
    Implements str::concat

    Arguments:
    * input: The base string.
    * other: The string to add.

    This creates a new string comprised of 'self' and 'other'. */
void lily_string_concat(lily_vm_state *vm, lily_function_val *self,
        uintptr_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *result_arg = vm_regs[code[2]];
    lily_value *self_arg = vm_regs[code[0]];
    lily_value *other_arg = vm_regs[code[1]];

    if (self_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Input string is nil.\n");

    if (other_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "String to append is nil.\n");

    lily_string_val *self_sv = self_arg->value.string;
    lily_string_val *other_sv = other_arg->value.string;

    int new_size = self_sv->size + other_sv->size + 1;
    lily_string_val *new_sv = try_make_sv(new_size);
    if (new_sv == NULL)
        lily_raise_nomem(vm->raiser);

    char *new_str = new_sv->string;
    strcpy(new_str, self_sv->string);
    strcat(new_str, other_sv->string);

    if ((result_arg->flags & VAL_IS_NIL_OR_PROTECTED) == 0)
        lily_deref_string_val(result_arg->value.string);

    result_arg->flags = 0;
    result_arg->value.string = new_sv;
}

#define CTYPE_WRAP(WRAP_NAME, WRAPPED_CALL) \
void WRAP_NAME(lily_vm_state *vm, lily_function_val *self, uintptr_t *code) \
{ \
    lily_value **vm_regs = vm->vm_regs; \
    lily_value *ret_arg = vm_regs[code[1]]; \
    lily_value *input_arg = vm_regs[code[0]]; \
\
    if (input_arg->flags & VAL_IS_NIL || \
        input_arg->value.string->size == 0) { \
        ret_arg->value.integer = 0; \
        ret_arg->flags = 0; \
        return; \
    } \
\
    char *loop_str = input_arg->value.string->string; \
    int i = 0; \
\
    ret_arg->value.integer = 1; \
    ret_arg->flags = 0; \
    for (i = 0;i < input_arg->value.string->size;i++) { \
        if (WRAPPED_CALL(loop_str[i]) == 0) { \
            ret_arg->value.integer = 0; \
            break; \
        } \
    } \
}

CTYPE_WRAP(lily_string_isdigit, isdigit)
CTYPE_WRAP(lily_string_isalpha, isalpha)
CTYPE_WRAP(lily_string_isspace, isspace)
CTYPE_WRAP(lily_string_isalnum, isalnum)

/* This table indicates how many more bytes need to be successfully read after
   that particular byte for proper utf-8. -1 = invalid.
   Table copied from lily_lexer.c */
static const char follower_table[256] =
{
     /* 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
/* 0 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 1 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 2 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 3 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 4 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 5 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 6 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 7 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 8 */-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
/* 9 */-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
/* A */-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
/* B */-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
/* C */-1,-1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
/* D */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
/* E */ 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
/* F */ 4, 4, 4, 4, 4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

/*  lstrip_utf8_start
    This is a helper for lstrip where input_arg's string has been checked to
    hold at least one utf8 chunk. */
static int lstrip_utf8_start(lily_value *input_arg, lily_string_val *strip_sv)
{
    char *input_str = input_arg->value.string->string;
    int input_length = input_arg->value.string->size;

    char *strip_str = strip_sv->string;
    int strip_length = strip_sv->size;
    int i = 0, j = 0, match = 1;

    char ch = strip_str[0];
    if (follower_table[(unsigned char)ch] == strip_length) {
        /* Only a single utf-8 char. This is a bit simpler. */
        char strip_start_ch = ch;
        int char_width = follower_table[(unsigned char)ch];
        while (i < input_length) {
            if (input_str[i] == strip_start_ch) {
                /* j starts at 1 because the first byte was already checked.
                   This compares the inner part of the strip string and the
                   input string to make sure the whole utf-8 chunk matches. */
                for (j = 1;j < char_width;j++) {
                    if (input_str[i + j] != strip_str[j]) {
                        match = 0;
                        break;
                    }
                }
                if (match == 0)
                    break;

                i += char_width;
            }
            else
                break;
        }
    }
    else {
        /* There's at least one utf-8 chunk. There may be ascii bytes to strip
           as well, or more utf-8 chunks. This is the most complicated case. */
        char input_ch;
        int char_width, k;
        while (1) {
            input_ch = input_str[i];
            if (input_ch == strip_str[j]) {
                char_width = follower_table[(unsigned char)strip_str[j]];
                match = 1;
                /* This has to use k, unlike the above loop, because j is being
                   used to hold the current position in strip_str. */
                for (k = 1;k < char_width;k++) {
                    if (input_str[i + k] != strip_str[j + k]) {
                        match = 0;
                        break;
                    }
                }
                if (match == 1) {
                    /* Found a match, so eat the valid utf-8 chunk and start
                       from the beginning of the strip string again. This makes
                       sure that each chunk of the input string is matched to
                       each chunk of the strip string. */
                    i += char_width;
                    if (i >= input_length)
                        break;
                    else {
                        j = 0;
                        continue;
                    }
                }
            }

            /* This assumes that strip_str is valid utf-8. */
            j += follower_table[(unsigned char)strip_str[j]];

            /* If all chunks in the strip str have been checked, then
               everything that can be removed has been removed. */
            if (j == strip_length) {
                match = 0;
                break;
            }
        }
    }

    return i;
}

/*  lstrip_ascii_start
    This is a helper for lstrip where input_arg's string has been checked to
    hold no utf8 chunks. This does byte stripping, which is simpler than utf8
    chunk check+strip. */
static int lstrip_ascii_start(lily_value *input_arg, lily_string_val *strip_sv)
{
    int i;
    char *input_str = input_arg->value.string->string;
    int input_length = input_arg->value.string->size;

    if (strip_sv->size == 1) {
        /* Strip a single byte really fast. The easiest case. */
        char strip_ch;
        strip_ch = strip_sv->string[0];
        for (i = 0;i < input_length;i++) {
            if (input_str[i] != strip_ch)
                break;
        }
    }
    else {
        /* Strip one of many ascii bytes. A bit tougher, but not much. */
        char *strip_str = strip_sv->string;
        int strip_length = strip_sv->size;
        for (i = 0;i < input_length;i++) {
            char ch = input_str[i];
            int found = 0;
            int j;
            for (j = 0;j < strip_length;j++) {
                if (ch == strip_str[j]) {
                    found = 1;
                    break;
                }
            }
            if (found == 0)
                break;
        }
    }

    return i;
}

void lily_string_lstrip(lily_vm_state *vm, lily_function_val *self,
        uintptr_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *input_arg = vm_regs[code[0]];
    lily_value *strip_arg = vm_regs[code[1]];
    lily_value *result_arg = vm_regs[code[2]];

    char *strip_str;
    unsigned char ch;
    int copy_from, i, has_multibyte_char, strip_str_len;
    lily_string_val *strip_sv;

    if (input_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Input string is nil.\n");

    if (strip_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Cannot strip nil value.\n");

    /* Either there is nothing to strip (1st), or stripping nothing (2nd). */
    if (input_arg->value.string->size == 0 ||
        strip_arg->value.string->size == 0) {
        lily_assign_value(vm, result_arg, input_arg);
        return;
    }

    strip_sv = strip_arg->value.string;
    strip_str = strip_sv->string;
    strip_str_len = strlen(strip_str);
    has_multibyte_char = 0;

    for (i = 0;i < strip_str_len;i++) {
        ch = (unsigned char)strip_str[i];
        if (ch > 127) {
            has_multibyte_char = 1;
            break;
        }
    }

    if (has_multibyte_char == 0)
        copy_from = lstrip_ascii_start(input_arg, strip_sv);
    else
        copy_from = lstrip_utf8_start(input_arg, strip_sv);

    int new_size = (input_arg->value.string->size - copy_from) + 1;
    lily_string_val *new_sv = try_make_sv(new_size);
    if (new_sv == NULL)
        lily_raise_nomem(vm->raiser);

    strcpy(new_sv->string, input_arg->value.string->string + copy_from);
    if ((result_arg->flags & VAL_IS_NIL_OR_PROTECTED) == 0)
        lily_deref_string_val(result_arg->value.string);

    result_arg->flags = 0;
    result_arg->value.string = new_sv;
}

void lily_string_startswith(lily_vm_state *vm, lily_function_val *self,
        uintptr_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *input_arg = vm_regs[code[0]];
    lily_value *prefix_arg = vm_regs[code[1]];
    lily_value *result_arg = vm_regs[code[2]];

    if (input_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Input is nil.\n");

    if (prefix_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Prefix is nil.\n");

    char *input_raw_str = input_arg->value.string->string;
    char *prefix_raw_str = prefix_arg->value.string->string;
    int prefix_size = prefix_arg->value.string->size;

    if (input_arg->value.string->size < prefix_size) {
        result_arg->value.integer = 0;
        result_arg->flags = 0;
        return;
    }

    int i, ok = 1;
    for (i = 0;i < prefix_size;i++) {
        if (input_raw_str[i] != prefix_raw_str[i]) {
            ok = 0;
            break;
        }
    }

    result_arg->flags = 0;
    result_arg->value.integer = ok;
}

/*  rstrip_utf8_stop
    This is a helper for str's rstrip that handles the case where there are
    no utf-8 chunks. This has a fast loop for stripping one byte, and a more
    general one for stripping out different bytes.
    This returns where string copying should stop at. */
static int rstrip_ascii_stop(lily_value *input_arg, lily_string_val *strip_sv)
{
    int i;
    char *input_str = input_arg->value.string->string;
    int input_length = input_arg->value.string->size;

    if (strip_sv->size == 1) {
        char strip_ch = strip_sv->string[0];
        for (i = input_length - 1;i >= 0;i--) {
            if (input_str[i] != strip_ch)
                break;
        }
    }
    else {
        char *strip_str = strip_sv->string;
        int strip_length = strip_sv->size;
        for (i = input_length - 1;i >= 0;i--) {
            char ch = input_str[i];
            int found = 0;
            int j;
            for (j = 0;j < strip_length;j++) {
                if (ch == strip_str[j]) {
                    found = 1;
                    break;
                }
            }
            if (found == 0)
                break;
        }
    }

    return i + 1;
}

/*  rstrip_utf8_stop
    This is a helper for str's rstrip that handles the case where the part to
    remove has at least one utf-8 chunk inside of it.
    This returns where string copying should stop at. */
static int rstrip_utf8_stop(lily_value *input_arg, lily_string_val *strip_sv)
{
    char *input_str = input_arg->value.string->string;
    int input_length = input_arg->value.string->size;

    char *strip_str = strip_sv->string;
    int strip_length = strip_sv->size;
    int i, j;

    i = input_length - 1;
    j = 0;
    while (i >= 0) {
        /* First find out how many bytes are in the current chunk. */
        int follow_count = follower_table[(unsigned char)strip_str[j]];
        /* Now get the last byte of this chunk. Since the follower table
           includes the total, offset by -1. */
        char last_strip_byte = strip_str[j + (follow_count - 1)];
        /* Input is going from right to left. See if input matches the last
           byte of the current utf-8 chunk. But also check that there are
           enough chars left to protect against underflow. */
        if (input_str[i] == last_strip_byte &&
            i + 1 >= follow_count) {
            int match = 1;
            int input_i, strip_i, k;
            /* input_i starts at i - 1 to skip the last byte.
               strip_i starts at follow_count so it can stop things. */
            for (input_i = i - 1, strip_i = j + (follow_count - 2), k = 1;
                 k < follow_count;
                 input_i--, strip_i--, k++) {
                if (input_str[input_i] != strip_str[strip_i]) {
                    match = 0;
                    break;
                }
            }

            if (match == 1) {
                i -= follow_count;
                j = 0;
                continue;
            }
        }

        /* Either the first byte or one of the inner bytes didn't match.
           Go to the next chunk and try again. */
        j += follow_count;
        if (j == strip_length)
            break;

        continue;
    }

    return i + 1;
}

void lily_string_rstrip(lily_vm_state *vm, lily_function_val *self,
        uintptr_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *input_arg = vm_regs[code[0]];
    lily_value *strip_arg = vm_regs[code[1]];
    lily_value *result_arg = vm_regs[code[2]];

    char *strip_str;
    unsigned char ch;
    int copy_to, i, has_multibyte_char, strip_str_len;
    lily_string_val *strip_sv;

    if (input_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Input string is nil.\n");

    if (strip_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Cannot strip nil value.\n");

    /* Either there is nothing to strip (1st), or stripping nothing (2nd). */
    if (input_arg->value.string->size == 0 ||
        strip_arg->value.string->size == 0) {
        lily_assign_value(vm, result_arg, input_arg);
        return;
    }

    strip_sv = strip_arg->value.string;
    strip_str = strip_sv->string;
    strip_str_len = strlen(strip_str);
    has_multibyte_char = 0;

    for (i = 0;i < strip_str_len;i++) {
        ch = (unsigned char)strip_str[i];
        if (ch > 127) {
            has_multibyte_char = 1;
            break;
        }
    }

    if (has_multibyte_char == 0)
        copy_to = rstrip_ascii_stop(input_arg, strip_sv);
    else
        copy_to = rstrip_utf8_stop(input_arg, strip_sv);

    int new_size = copy_to + 1;
    lily_string_val *new_sv = try_make_sv(new_size);
    if (new_sv == NULL)
        lily_raise_nomem(vm->raiser);

    strncpy(new_sv->string, input_arg->value.string->string, copy_to);
    /* This will always copy a partial string, so make sure to add a terminator. */
    new_sv->string[copy_to] = '\0';

    if ((result_arg->flags & VAL_IS_NIL_OR_PROTECTED) == 0)
        lily_deref_string_val(result_arg->value.string);

    result_arg->flags = 0;
    result_arg->value.string = new_sv;
}

void lily_string_endswith(lily_vm_state *vm, lily_function_val *self,
        uintptr_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *input_arg = vm_regs[code[0]];
    lily_value *suffix_arg = vm_regs[code[1]];
    lily_value *result_arg = vm_regs[code[2]];

    if (input_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Input is nil.\n");

    if (suffix_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Suffix is nil.\n");

    char *input_raw_str = input_arg->value.string->string;
    char *suffix_raw_str = suffix_arg->value.string->string;
    int input_size = input_arg->value.string->size;
    int suffix_size = suffix_arg->value.string->size;

    if (suffix_size > input_size) {
        result_arg->value.integer = 0;
        result_arg->flags = 0;
        return;
    }

    int input_i, suffix_i, ok = 1;
    for (input_i = input_size - 1, suffix_i = suffix_size - 1;
         suffix_i > 0;
         input_i--, suffix_i--) {
        if (input_raw_str[input_i] != suffix_raw_str[suffix_i]) {
            ok = 0;
            break;
        }
    }

    result_arg->flags = 0;
    result_arg->value.integer = ok;
}

void lily_string_lower(lily_vm_state *vm, lily_function_val *self,
        uintptr_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *input_arg = vm_regs[code[0]];
    lily_value *result_arg = vm_regs[code[1]];

    if (input_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Input is nil.\n");

    int new_size = input_arg->value.string->size + 1;
    lily_string_val *new_sv = try_make_sv(new_size);
    if (new_sv == NULL)
        lily_raise_nomem(vm->raiser);

    char *new_str = new_sv->string;
    char *input_str = input_arg->value.string->string;
    int input_length = input_arg->value.string->size;
    int i;

    for (i = 0;i < input_length;i++) {
        char ch = input_str[i];
        if (isupper(ch))
            new_str[i] = tolower(ch);
        else
            new_str[i] = ch;
    }
    new_str[input_length] = '\0';

    if ((result_arg->flags & VAL_IS_NIL_OR_PROTECTED) == 0)
        lily_deref_string_val(result_arg->value.string);

    result_arg->flags = 0;
    result_arg->value.string = new_sv;
}

void lily_string_upper(lily_vm_state *vm, lily_function_val *self,
        uintptr_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *input_arg = vm_regs[code[0]];
    lily_value *result_arg = vm_regs[code[1]];

    if (input_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Input is nil.\n");

    int new_size = input_arg->value.string->size + 1;
    lily_string_val *new_sv = try_make_sv(new_size);
    if (new_sv == NULL)
        lily_raise_nomem(vm->raiser);

    char *new_str = new_sv->string;
    char *input_str = input_arg->value.string->string;
    int input_length = input_arg->value.string->size;
    int i;

    for (i = 0;i < input_length;i++) {
        char ch = input_str[i];
        if (islower(ch))
            new_str[i] = toupper(ch);
        else
            new_str[i] = ch;
    }
    new_str[input_length] = '\0';

    if ((result_arg->flags & VAL_IS_NIL_OR_PROTECTED) == 0)
        lily_deref_string_val(result_arg->value.string);

    result_arg->flags = 0;
    result_arg->value.string = new_sv;
}

void lily_string_find(lily_vm_state *vm, lily_function_val *self,
        uintptr_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *input_arg = vm_regs[code[0]];
    lily_value *find_arg = vm_regs[code[1]];
    lily_value *result_arg = vm_regs[code[2]];

    if (input_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Input is nil.\n");

    if (find_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Find str is nil.\n");

    char *input_str = input_arg->value.string->string;
    int input_length = input_arg->value.string->size;

    char *find_str = find_arg->value.string->string;
    int find_length = find_arg->value.string->size;

    if (find_length > input_length) {
        result_arg->flags = 0;
        result_arg->value.integer = -1;
        return;
    }
    else if (find_length == 0) {
        result_arg->flags = 0;
        result_arg->value.integer = 0;
        return;
    }

    char find_ch;
    int i, j, k, length_diff, match;

    length_diff = input_length - find_length;
    find_ch = find_str[0];
    match = 0;

    /* This stops at length_diff for two reasons:
       * The inner loop won't have to do a boundary check.
       * Search will stop if there isn't enough length left for a match
         (ex: "abcdef".find("defg")) */
    for (i = 0;i <= length_diff;i++) {
        if (input_str[i] == find_ch) {
            match = 1;
            /* j starts at i + 1 to skip the first match.
               k starts at 1 for the same reason. */
            for (j = i + 1, k = 1;k < find_length;j++, k++) {
                if (input_str[j] != find_str[k]) {
                    match = 0;
                    break;
                }
            }
            if (match == 1)
                break;
        }
    }

    if (match == 0)
        i = -1;

    result_arg->flags = 0;
    result_arg->value.integer = i;
}

void lily_string_strip(lily_vm_state *vm, lily_function_val *self,
        uintptr_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *input_arg = vm_regs[code[0]];
    lily_value *strip_arg = vm_regs[code[1]];
    lily_value *result_arg = vm_regs[code[2]];

    if (input_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Input string is nil.\n");

    if (strip_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Cannot strip nil value.\n");

    /* Either there is nothing to strip (1st), or stripping nothing (2nd). */
    if (input_arg->value.string->size == 0 ||
        strip_arg->value.string->size == 0) {
        lily_assign_value(vm, result_arg, input_arg);
        return;
    }

    char ch;
    lily_string_val *strip_sv = strip_arg->value.string;
    char *strip_str = strip_sv->string;
    int strip_str_len = strlen(strip_str);
    int has_multibyte_char = 0;
    int copy_from, copy_to, i;

    for (i = 0;i < strip_str_len;i++) {
        ch = (unsigned char)strip_str[i];
        if (ch > 127) {
            has_multibyte_char = 1;
            break;
        }
    }

    if (has_multibyte_char == 0)
        copy_from = lstrip_ascii_start(input_arg, strip_sv);
    else
        copy_from = lstrip_utf8_start(input_arg, strip_sv);

    if (copy_from != input_arg->value.string->size) {
        if (has_multibyte_char)
            copy_to = rstrip_ascii_stop(input_arg, strip_sv);
        else
            copy_to = rstrip_utf8_stop(input_arg, strip_sv);
    }
    else
        /* The whole string consists of stuff in strip_str. Do this so the
           result is an empty string. */
        copy_to = copy_from;

    int new_size = (copy_to - copy_from) + 1;
    lily_string_val *new_sv = try_make_sv(new_size);
    if (new_sv == NULL)
        lily_raise_nomem(vm->raiser);

    char *new_str = new_sv->string;
    strncpy(new_str, input_arg->value.string->string + copy_from, new_size - 1);
    new_str[new_size - 1] = '\0';

    if ((result_arg->flags & VAL_IS_NIL_OR_PROTECTED) == 0)
        lily_deref_string_val(result_arg->value.string);

    result_arg->flags = 0;
    result_arg->value.string = new_sv;
}

/*  lily_string_trim
    Implements str::trim

    Arguments:
    * input: The string to be stripped. If this is nil, ErrBadValue is raised.

    This removes all whitespace from the front and the back of 'input'.
    Whitespace is any of: ' \t\r\n'.

    Returns the newly made string. */
void lily_string_trim(lily_vm_state *vm, lily_function_val *self,
        uintptr_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *input_arg = vm_regs[code[0]];
    lily_value *result_arg = vm_regs[code[1]];

    if (input_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Input is nil.\n");

    char fake_buffer[5] = " \t\r\n";
    lily_string_val fake_sv;
    fake_sv.string = fake_buffer;
    fake_sv.size = strlen(fake_buffer);

    int copy_from, copy_to;
    copy_from = lstrip_ascii_start(input_arg, &fake_sv);
    copy_to = rstrip_ascii_stop(input_arg, &fake_sv);

    int new_size = (copy_to - copy_from) + 1;
    lily_string_val *new_sv = try_make_sv(new_size);
    if (new_sv == NULL)
        lily_raise_nomem(vm->raiser);

    char *new_str = new_sv->string;

    strncpy(new_str, input_arg->value.string->string + copy_from, new_size - 1);
    new_str[new_size - 1] = '\0';

    if ((result_arg->flags & VAL_IS_NIL_OR_PROTECTED) == 0)
        lily_deref_string_val(result_arg->value.string);

    result_arg->flags = 0;
    result_arg->value.string = new_sv;
}

/*  lily_string_htmlencode
    Implements str::htmlencode

    Arguments:
    * input: The string to be encoded. If this is nil, ErrBadValue is raised.

    This transforms any html entities to their encoded versions:
    < becomes &lt;
    > becomes &gt;
    & becomes &amp; */
void lily_string_htmlencode(lily_vm_state *vm, lily_function_val *self,
        uintptr_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_value *input_arg = vm_regs[code[0]];
    lily_value *result_arg = vm_regs[code[1]];

    if (input_arg->flags & VAL_IS_NIL)
        lily_raise(vm->raiser, lily_ErrBadValue, "Input is nil.\n");

    lily_vm_stringbuf *string_buffer = vm->string_buffer;
    char *str = string_buffer->data;
    int str_size = string_buffer->data_size;
    int string_fakemax = string_buffer->data_size - 5;

    int i = 0;
    char *input_str = input_arg->value.string->string;
    char *ch = &input_str[0];

    while (1) {
        if (i == string_fakemax) {
            char *new_str = lily_realloc(str, str_size * 2);
            if (new_str == NULL)
                lily_raise_nomem(vm->raiser);

            string_buffer->data = new_str;
            str = new_str;

            str_size *= 2;
            string_buffer->data_size = str_size;
            string_fakemax = str_size - 5;
        }

        if (*ch == '&') {
            str[i    ] = '&';
            str[i + 1] = 'a';
            str[i + 2] = 'm';
            str[i + 3] = 'p';
            str[i + 4] = ';';
            i += 5;
        }
        else if (*ch == '<') {
            str[i    ] = '&';
            str[i + 1] = 'l';
            str[i + 2] = 't';
            str[i + 3] = ';';
            i += 4;
        }
        else if (*ch == '>') {
            str[i    ] = '&';
            str[i + 1] = 'g';
            str[i + 2] = 't';
            str[i + 3] = ';';
            i += 4;
        }
        else {
            str[i] = *ch;
            i++;
            if (*ch == '\0')
                break;
        }

        ch++;
    }

    lily_string_val *new_sv = try_make_sv(i + 1);
    if (new_sv == NULL)
        lily_raise_nomem(vm->raiser);

    strcpy(new_sv->string, str);

    if ((result_arg->flags & VAL_IS_NIL_OR_PROTECTED) == 0)
        lily_deref_string_val(result_arg->value.string);

    result_arg->flags = 0;
    result_arg->value.string = new_sv;
}

static const lily_func_seed htmlencode =
    {"htmlencode", lily_string_htmlencode, NULL,
        {SYM_CLASS_FUNCTION, 2, 0, SYM_CLASS_STRING, SYM_CLASS_STRING}};

static const lily_func_seed trim =
    {"trim", lily_string_trim, &htmlencode,
        {SYM_CLASS_FUNCTION, 2, 0, SYM_CLASS_STRING, SYM_CLASS_STRING}};

static const lily_func_seed strip =
    {"strip", lily_string_strip, &trim,
        {SYM_CLASS_FUNCTION, 3, 0, SYM_CLASS_STRING, SYM_CLASS_STRING, SYM_CLASS_STRING}};

static const lily_func_seed find =
    {"find", lily_string_find, &strip,
        {SYM_CLASS_FUNCTION, 3, 0, SYM_CLASS_INTEGER, SYM_CLASS_STRING, SYM_CLASS_STRING}};

static const lily_func_seed upper =
    {"upper", lily_string_upper, &find,
        {SYM_CLASS_FUNCTION, 2, 0, SYM_CLASS_STRING, SYM_CLASS_STRING}};

static const lily_func_seed lower =
    {"lower", lily_string_lower, &upper,
        {SYM_CLASS_FUNCTION, 2, 0, SYM_CLASS_STRING, SYM_CLASS_STRING}};

static const lily_func_seed endswith =
    {"endswith", lily_string_endswith, &lower,
        {SYM_CLASS_FUNCTION, 3, 0, SYM_CLASS_INTEGER, SYM_CLASS_STRING, SYM_CLASS_STRING}};

static const lily_func_seed rstrip =
    {"rstrip", lily_string_rstrip, &endswith,
        {SYM_CLASS_FUNCTION, 3, 0, SYM_CLASS_STRING, SYM_CLASS_STRING, SYM_CLASS_STRING}};

static const lily_func_seed startswith =
    {"startswith", lily_string_startswith, &rstrip,
        {SYM_CLASS_FUNCTION, 3, 0, SYM_CLASS_INTEGER, SYM_CLASS_STRING, SYM_CLASS_STRING}};

static const lily_func_seed lstrip =
    {"lstrip", lily_string_lstrip, &startswith,
        {SYM_CLASS_FUNCTION, 3, 0, SYM_CLASS_STRING, SYM_CLASS_STRING, SYM_CLASS_STRING}};

static const lily_func_seed isalnum_fn =
    {"isalnum", lily_string_isalnum, &lstrip,
        {SYM_CLASS_FUNCTION, 2, 0, SYM_CLASS_INTEGER, SYM_CLASS_STRING}};

static const lily_func_seed isdigit_fn =
    {"isdigit", lily_string_isdigit, &isalnum_fn,
        {SYM_CLASS_FUNCTION, 2, 0, SYM_CLASS_INTEGER, SYM_CLASS_STRING}};

static const lily_func_seed isalpha_fn =
    {"isalpha", lily_string_isalpha, &isdigit_fn,
        {SYM_CLASS_FUNCTION, 2, 0, SYM_CLASS_INTEGER, SYM_CLASS_STRING}};

static const lily_func_seed isspace_fn =
    {"isspace", lily_string_isspace, &isalpha_fn,
        {SYM_CLASS_FUNCTION, 2, 0, SYM_CLASS_INTEGER, SYM_CLASS_STRING}};

static const lily_func_seed concat =
    {"concat", lily_string_concat, &isspace_fn,
        {SYM_CLASS_FUNCTION, 3, 0, SYM_CLASS_STRING, SYM_CLASS_STRING, SYM_CLASS_STRING}};

#define SEED_START concat

int lily_string_setup(lily_class *cls)
{
    cls->seed_table = &SEED_START;
    return 1;
}
