#include "ruby/ruby.h"
#define USE_INSN_STACK_INCREASE 1
#include "vm_core.h"
#include "iseq.h"
#include "insns.inc"
#include "insns_info.inc"


typedef struct iseq_link_element {
    enum {
	ISEQ_ELEMENT_NONE   = INT2FIX(0x00),
	ISEQ_ELEMENT_LABEL  = INT2FIX(0x01),
	ISEQ_ELEMENT_INSN   = INT2FIX(0x02),
	ISEQ_ELEMENT_ADJUST = INT2FIX(0x03)
    } type;
    struct iseq_link_element *next;
    struct iseq_link_element *prev;
} LINK_ELEMENT;

typedef struct iseq_link_anchor {
    LINK_ELEMENT anchor;
    LINK_ELEMENT *last;
} LINK_ANCHOR;

typedef struct iseq_label_data {
    LINK_ELEMENT link;
    int label_no;
    int position;
    int sc_state;
    int set;
    int sp;
} LABEL;

typedef struct iseq_insn_data {
    LINK_ELEMENT link;
    enum ruby_vminsn_type insn_id;
    int line_no;
    int operand_size;
    int sc_state;
    VALUE *operands;
} INSN;

typedef struct iseq_adjust_data {
    LINK_ELEMENT link;
    LABEL *label;
    int line_no;
} ADJUST;

struct ensure_range {
    LABEL *begin;
    LABEL *end;
    struct ensure_range *next;
};

struct iseq_compile_data_ensure_node_stack {
    NODE *ensure_node;
    struct iseq_compile_data_ensure_node_stack *prev;
    struct ensure_range *erange;
};

/**
 * debug function(macro) interface depend on CPDEBUG
 * if it is less than 0, runtime option is in effect.
 *
 * debug level:
 *  0: no debug output
 *  1: show node type
 *  2: show node important parameters
 *  ...
 *  5: show other parameters
 * 10: show every AST array
 */

#ifndef CPDEBUG
#define CPDEBUG 0
#endif

#if CPDEBUG >= 0
#define compile_debug CPDEBUG
#else
#define compile_debug iseq->compile_data->option->debug_level
#endif

NORETURN(PRINTF_ARGS(void rb_compile_bug(const char*, int, const char*, ...), 3, 4));

#if CPDEBUG

#define compile_debug_print_indent(level) \
    ruby_debug_print_indent(level, compile_debug, gl_node_level * 2)

#define debugp(header, value) (void) \
  (compile_debug_print_indent(1) && \
   ruby_debug_print_value(1, compile_debug, header, value))

#define debugi(header, id)  (void) \
  (compile_debug_print_indent(1) && \
   ruby_debug_print_id(1, compile_debug, header, id))

#define debugp_param(header, value)  (void) \
  (compile_debug_print_indent(1) && \
   ruby_debug_print_value(1, compile_debug, header, value))

#define debugp_verbose(header, value)  (void) \
  (compile_debug_print_indent(2) && \
   ruby_debug_print_value(2, compile_debug, header, value))

#define debugp_verbose_node(header, value)  (void) \
  (compile_debug_print_indent(10) && \
   ruby_debug_print_value(10, compile_debug, header, value))

#define debug_node_start(node)  ((void) \
  (compile_debug_print_indent(1) && \
   (ruby_debug_print_node(1, CPDEBUG, "", (NODE *)node), gl_node_level)), \
   gl_node_level++)

#define debug_node_end()  gl_node_level --;

#else

static inline ID
r_id(ID id)
{
    return id;
}

static inline VALUE
r_value(VALUE value)
{
    return value;
}

#define debugi(header, id)                 r_id(id)
#define debugp(header, value)              r_value(value)
#define debugp_verbose(header, value)      r_value(value)
#define debugp_verbose_node(header, value) r_value(value)
#define debugp_param(header, value)        r_value(value)
#define debug_node_start(node)             ((void)0)
#define debug_node_end()                   ((void)0)
#endif

#if CPDEBUG > 1 || CPDEBUG < 0
PRINTF_ARGS(void ruby_debug_printf(const char*, ...), 1, 2);
#define debugs if (compile_debug_print_indent(1)) ruby_debug_printf
#define debug_compile(msg, v) ((void)(compile_debug_print_indent(1) && fputs(msg, stderr)), (v))
#else
#define debugs                             if(0)printf
#define debug_compile(msg, v) (v)
#endif


/* create new label */
#define NEW_LABEL(l) new_label_body(iseq, l)

#define iseq_filename(iseq) \
  (((rb_iseq_t*)DATA_PTR(iseq))->filename)

#define NEW_ISEQVAL(node, name, type)       \
  new_child_iseq(iseq, node, name, 0, type)

#define NEW_CHILD_ISEQVAL(node, name, type)       \
  new_child_iseq(iseq, node, name, iseq->self, type)

#define NEW_SPECIAQL_BLOCK_ISEQVAL(iseq, sym) \
  new_child_iseq(iseq, iseq->node, iseq->name, iseq->parent_iseq, iseq->type, sym)

/* add instructions */
#define ADD_SEQ(seq1, seq2) \
  APPEND_LIST(seq1, seq2)

/* add an instruction */
#define ADD_INSN(seq, line, insn) \
  ADD_ELEM(seq, (LINK_ELEMENT *) new_insn_body(iseq, line, BIN(insn), 0))

/* add an instruction with label operand */
#define ADD_INSNL(seq, line, insn, label) \
  ADD_ELEM(seq, (LINK_ELEMENT *) \
           new_insn_body(iseq, line, BIN(insn), 1, (VALUE)label))

/* add an instruction with some operands (1, 2, 3, 5) */
#define ADD_INSN1(seq, line, insn, op1) \
  ADD_ELEM(seq, (LINK_ELEMENT *) \
           new_insn_body(iseq, line, BIN(insn), 1, (VALUE)op1))

#define ADD_INSN2(seq, line, insn, op1, op2) \
  ADD_ELEM(seq, (LINK_ELEMENT *) \
           new_insn_body(iseq, line, BIN(insn), 2, (VALUE)op1, (VALUE)op2))

#define ADD_INSN3(seq, line, insn, op1, op2, op3) \
  ADD_ELEM(seq, (LINK_ELEMENT *) \
           new_insn_body(iseq, line, BIN(insn), 3, (VALUE)op1, (VALUE)op2, (VALUE)op3))

/* Specific Insn factory */
#define ADD_SEND(seq, line, id, argc) \
  ADD_SEND_R(seq, line, id, argc, (VALUE)Qfalse, (VALUE)INT2FIX(0))

#define ADD_CALL_RECEIVER(seq, line) \
  ADD_INSN(seq, line, putnil)

#define ADD_CALL(seq, line, id, argc) \
  ADD_SEND_R(seq, line, id, argc, (VALUE)Qfalse, (VALUE)INT2FIX(VM_CALL_FCALL_BIT))

#define ADD_CALL_WITH_BLOCK(seq, line, id, argc, block) \
  ADD_SEND_R(seq, line, id, argc, block, (VALUE)INT2FIX(VM_CALL_FCALL_BIT))

#define ADD_SEND_R(seq, line, id, argc, block, flag) \
  ADD_ELEM(seq, (LINK_ELEMENT *) \
           new_insn_send(iseq, line, \
                         (VALUE)id, (VALUE)argc, (VALUE)block, (VALUE)flag))

#define ADD_TRACE(seq, line, event) \
  do { \
      if ((event) == RUBY_EVENT_LINE && iseq->coverage && \
	  RARRAY_PTR(iseq->coverage)[(line) - 1] == Qnil) { \
	  RARRAY_PTR(iseq->coverage)[(line) - 1] = INT2FIX(0); \
	  ADD_INSN1(seq, line, trace, INT2FIX(RUBY_EVENT_COVERAGE)); \
      } \
      if (iseq->compile_data->option->trace_instruction) { \
	  ADD_INSN1(seq, line, trace, INT2FIX(event)); \
      } \
  }while(0);

/* add label */
#define ADD_LABEL(seq, label) \
  ADD_ELEM(seq, (LINK_ELEMENT *) label)

#define ADD_ADJUST(seq, line, label) \
  ADD_ELEM(seq, (LINK_ELEMENT *) new_adjust_body(iseq, label, line))

#define ADD_ADJUST_RESTORE(seq, label) \
  ADD_ELEM(seq, (LINK_ELEMENT *) new_adjust_body(iseq, label, -1))

#define ADD_CATCH_ENTRY(type, ls, le, iseqv, lc)		\
    (rb_ary_push(iseq->compile_data->catch_table_ary,		\
		 rb_ary_new3(5, type,				\
			     (VALUE)(ls) | 1, (VALUE)(le) | 1,	\
			     iseqv, (VALUE)(lc) | 1)))

/* compile node */
#define COMPILE(anchor, desc, node) \
  (debug_compile("== " desc "\n", \
                 iseq_compile_each(iseq, anchor, node, 0)))

/* compile node, this node's value will be poped */
#define COMPILE_POPED(anchor, desc, node)    \
  (debug_compile("== " desc "\n", \
                 iseq_compile_each(iseq, anchor, node, 1)))

/* compile node, which is poped when 'poped' is true */
#define COMPILE_(anchor, desc, node, poped)  \
  (debug_compile("== " desc "\n", \
                 iseq_compile_each(iseq, anchor, node, poped)))

#define OPERAND_AT(insn, idx) \
  (((INSN*)(insn))->operands[idx])

#define INSN_OF(insn) \
  (((INSN*)(insn))->insn_id)

/* error */
#define COMPILE_ERROR(strs)                        \
{                                                  \
  VALUE tmp = GET_THREAD()->errinfo;               \
  if (compile_debug) rb_compile_bug strs;          \
  GET_THREAD()->errinfo = iseq->compile_data->err_info;  \
  rb_compile_error strs;                           \
  iseq->compile_data->err_info = GET_THREAD()->errinfo; \
  GET_THREAD()->errinfo = tmp;                     \
  ret = 0;                                         \
  break;                                           \
}

#define ERROR_ARGS ruby_sourcefile, nd_line(node),


#define COMPILE_OK 1
#define COMPILE_NG 0


/* leave name uninitialized so that compiler warn if INIT_ANCHOR is
 * missing */
#define DECL_ANCHOR(name) \
  LINK_ANCHOR *name, name##_body__ = {{0,},}
#define INIT_ANCHOR(name) \
  (name##_body__.last = &name##_body__.anchor, name = &name##_body__)

#define hide_obj(obj) do {OBJ_FREEZE(obj); RBASIC(obj)->klass = 0;} while (0)

#include "optinsn.inc"
#if OPT_INSTRUCTIONS_UNIFICATION
#include "optunifs.inc"
#endif

/* for debug */
#if CPDEBUG < 0
#define ISEQ_ARG iseq,
#define ISEQ_ARG_DECLARE rb_iseq_t *iseq,
#else
#define ISEQ_ARG
#define ISEQ_ARG_DECLARE
#endif

#if CPDEBUG
#define gl_node_level iseq->compile_data->node_level
#if 0
static void debug_list(ISEQ_ARG_DECLARE LINK_ANCHOR *anchor);
#endif
#endif

static void dump_disasm_list(LINK_ELEMENT *elem);

static int insn_data_length(INSN *iobj);
static int insn_data_line_no(INSN *iobj);
static int calc_sp_depth(int depth, INSN *iobj);

static void ADD_ELEM(ISEQ_ARG_DECLARE LINK_ANCHOR *anchor, LINK_ELEMENT *elem);

static INSN *new_insn_body(rb_iseq_t *iseq, int line_no, int insn_id, int argc, ...);
static LABEL *new_label_body(rb_iseq_t *iseq, int line);
static ADJUST *new_adjust_body(rb_iseq_t *iseq, LABEL *label, int line);

static int iseq_compile_each(rb_iseq_t *iseq, LINK_ANCHOR *anchor, NODE * n, int);
static int iseq_setup(rb_iseq_t *iseq, LINK_ANCHOR *anchor);
static int iseq_optimize(rb_iseq_t *iseq, LINK_ANCHOR *anchor);
static int iseq_insns_unification(rb_iseq_t *iseq, LINK_ANCHOR *anchor);

static int iseq_set_local_table(rb_iseq_t *iseq, ID *tbl);
static int iseq_set_exception_local_table(rb_iseq_t *iseq);
static int iseq_set_arguments(rb_iseq_t *iseq, LINK_ANCHOR *anchor, NODE * node);

static int iseq_set_sequence_stackcaching(rb_iseq_t *iseq, LINK_ANCHOR *anchor);
static int iseq_set_sequence(rb_iseq_t *iseq, LINK_ANCHOR *anchor);
static int iseq_set_exception_table(rb_iseq_t *iseq);
static int iseq_set_optargs_table(rb_iseq_t *iseq);
static void lisp_compile(rb_iseq_t *iseq, LINK_ANCHOR *ret, NODE * node, int poped);
