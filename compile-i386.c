/*
 * sparse/compile-i386.c
 *
 * Copyright (C) 2003 Transmeta Corp.
 *               2003 Linus Torvalds
 * Copyright 2003 Jeff Garzik
 *
 * Licensed under the Open Software License version 1.1
 *
 * x86 backend
 *
 */
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>

#include "lib.h"
#include "token.h"
#include "parse.h"
#include "symbol.h"
#include "scope.h"
#include "expression.h"
#include "target.h"


struct textbuf;
struct textbuf {
	unsigned int	len;	/* does NOT include terminating null */
	char		*text;
	struct textbuf	*next;
	struct textbuf	*prev;
};

struct function {
	int pseudo_nr;
	struct ptr_list *pseudo_list;
	struct textbuf *buf;
	struct symbol **argv;
	unsigned int argc;
};

enum storage_type {
	STOR_PSEUDO,	/* variable stored on the stack */
	STOR_ARG,	/* function argument */
	STOR_SYM,	/* a symbol we can directly ref in the asm */
};

struct storage {
	enum storage_type type;
	union {
		/* stuff for pseudos */
		struct {
			int pseudo;
		};
		/* stuff for function arguments */
		struct {
			int idx;
		};
		/* stuff for symbols */
		struct {
			struct symbol *sym;
		};
	};
};

struct symbol_private {
	struct storage *addr;
};

struct function *current_func = NULL;
struct textbuf *unit_post_text = NULL;
static const char *current_section;


static struct storage *x86_address_gen(struct expression *expr);
static struct storage *x86_symbol_expr(struct symbol *sym);
static void x86_symbol(struct symbol *sym);
static struct storage *x86_statement(struct statement *stmt);
static struct storage *x86_expression(struct expression *expr);


static inline unsigned int pseudo_offset(struct storage *s)
{
	if (s->type != STOR_PSEUDO)
		return 123456;	/* intentionally bogus value */

	return ((s->pseudo - 1) * 4);
}

static inline unsigned int arg_offset(struct storage *s)
{
	if (s->type != STOR_ARG)
		return 123456;	/* intentionally bogus value */

	/* FIXME: this is wrong wrong wrong */
	return (current_func->pseudo_nr + 1) * 4;
}

static const char *pretty_offset(int ofs)
{
	static char esp_buf[64];

	if (ofs)
		sprintf(esp_buf, "%d(%%esp)", ofs);
	else
		strcpy(esp_buf, "(%esp)");

	return esp_buf;
}

static void stor_sym_init(struct symbol *sym)
{
	struct storage *stor;
	struct symbol_private *priv;

	priv = calloc(1, sizeof(*priv) + sizeof(*stor));
	if (!priv)
		die("OOM in stor_sym_init");

	stor = (struct storage *) (priv + 1);

	priv->addr = stor;
	stor->type = STOR_SYM;
	stor->sym = sym;
}

/* we don't yet properly locate arguments on the stack.  we generate
 * an offset based on the stack frame at the time the argument is
 * referenced, which is incorrect, because the stack frame pointer
 * may change after that point, and before the end of the function.
 */
static const char *stor_arg_warning(struct storage *s)
{
	static const char warning[32] = "stack offset WRONG!";

	if (s->type != STOR_ARG)
		return NULL;

	return warning;
}

static const char *stor_op_name(struct storage *s)
{
	static char name[32];

	switch (s->type) {
	case STOR_PSEUDO:
		strcpy(name, pretty_offset((int) pseudo_offset(s)));
		break;
	case STOR_ARG:
		strcpy(name, pretty_offset((int) arg_offset(s)));
		break;
	case STOR_SYM:
		strcpy(name, show_ident(s->sym->ident));
		break;
	}

	return name;
}

static struct storage *new_pseudo(void)
{
	struct function *f = current_func;
	struct storage *stor;

	assert(f != NULL);

	stor = calloc(1, sizeof(*stor));
	if (!stor)
		die("OOM in new_pseudo");

	stor->type = STOR_PSEUDO;
	stor->pseudo = ++f->pseudo_nr;

	add_ptr_list(&f->pseudo_list, stor);

	return stor;
}

static int new_label(void)
{
	static int label = 0;
	return ++label;
}

static void textbuf_push(struct textbuf **buf_p, const char *text)
{
	struct textbuf *tmp, *list = *buf_p;
	unsigned int text_len = strlen(text);
	unsigned int alloc_len = text_len + 1 + sizeof(*list);

	tmp = calloc(1, alloc_len);
	if (!tmp)
		die("OOM on textbuf alloc");

	tmp->text = ((void *) tmp) + sizeof(*tmp);
	memcpy(tmp->text, text, text_len + 1);
	tmp->len = text_len;

	/* add to end of list */
	if (!list) {
		list = tmp;
		tmp->prev = tmp;
	} else {
		tmp->prev = list->prev;
		tmp->prev->next = tmp;
		list->prev = tmp;
	}
	tmp->next = list;

	*buf_p = list;
}

static void textbuf_emit(struct textbuf **buf_p)
{
	struct textbuf *tmp, *list = *buf_p;

	while (list) {
		tmp = list;
		if (tmp->next == tmp)
			list = NULL;
		else {
			tmp->prev->next = tmp->next;
			tmp->next->prev = tmp->prev;
			list = tmp->next;
		}

		fputs(tmp->text, stdout);

		free(tmp);
	}

	*buf_p = list;
}

static void insn(const char *insn, const char *op1, const char *op2,
		 const char *comment_in)
{
	struct function *f = current_func;
	char s[128];
	char comment[64];

	assert(insn != NULL);

	if (comment_in && (*comment_in))
		sprintf(comment, "\t\t# %s", comment_in);
	else
		comment[0] = 0;

	if (op2 && (*op2))
		sprintf(s, "\t%s\t%s, %s%s\n", insn, op1, op2, comment);
	else if (op1 && (*op1))
		sprintf(s, "\t%s\t%s%s%s\n", insn, op1,
			comment[0] ? "\t" : "", comment);
	else
		sprintf(s, "\t%s\t%s%s\n", insn,
			comment[0] ? "\t\t" : "", comment);
	textbuf_push(&f->buf, s);
}

static void emit_unit_pre(const char *basename)
{
	printf("\t.file\t\"%s\"\n", basename);
}

static void emit_unit_post(void)
{
	textbuf_emit(&unit_post_text);
	printf("\t.ident\t\"sparse silly x86 backend (built %s)\"\n", __DATE__);
}

/* conditionally switch sections */
static void emit_section(const char *s)
{
	if (s == current_section)
		return;
	if (current_section && (!strcmp(s, current_section)))
		return;

	printf("\t%s\n", s);
	current_section = s;
}

static void func_cleanup(struct function *f)
{
	struct storage *stor;

	FOR_EACH_PTR(f->pseudo_list, stor) {
		free(stor);
	} END_FOR_EACH_PTR;

	free_ptr_list(&f->pseudo_list);
	free(f);
}

/* function prologue */
static void emit_func_pre(struct symbol *sym)
{
	const char *name = show_ident(sym->ident);
	struct function *f;
	struct symbol *arg;
	unsigned int i, argc = 0, alloc_len;
	unsigned char *mem;
	struct symbol_private *privbase;
	struct storage *storage_base;
	struct symbol *base_type = sym->ctype.base_type;

	FOR_EACH_PTR(base_type->arguments, arg) {
		argc++;
	} END_FOR_EACH_PTR;

	alloc_len =
		sizeof(*f) +
		(argc * sizeof(struct symbol *)) +
		(argc * sizeof(struct symbol_private)) +
		(argc * sizeof(struct storage));
	mem = calloc(1, alloc_len);
	if (!mem)
		die("OOM on func info");

	f		=  (struct function *) mem;
	mem		+= sizeof(*f);
	f->argv		=  (struct symbol **) mem;
	mem		+= (argc * sizeof(struct symbol *));
	privbase	=  (struct symbol_private *) mem;
	mem		+= (argc * sizeof(struct symbol_private));
	storage_base	=  (struct storage *) mem;

	f->argc = argc;

	i = 0;
	FOR_EACH_PTR(base_type->arguments, arg) {
		f->argv[i] = arg;
		arg->aux = &privbase[i];
		storage_base[i].type = STOR_ARG;
		storage_base[i].idx = i;
		privbase[i].addr = &storage_base[i];
		i++;
	} END_FOR_EACH_PTR;

	assert(current_func == NULL);
	current_func = f;

	emit_section(".text");
	if ((sym->ctype.modifiers & MOD_STATIC) == 0)
		printf(".globl %s\n", name);
	printf("\t.type\t%s, @function\n", name);
	printf("%s:\n", name);
}

/* function epilogue */
static void emit_func_post(struct symbol *sym, struct storage *val)
{
	const char *name = show_ident(sym->ident);
	struct function *f = current_func;
	int pseudo_nr = f->pseudo_nr;
	char s[16];

	sprintf(s, "$%d", pseudo_nr * 4);
	printf("\tsubl\t%s, %%esp\n", s);
	if (val)
		insn("movl", stor_op_name(val), "%eax", stor_arg_warning(val));
	insn("addl", s, "%esp", NULL);
	insn("ret", NULL, NULL, NULL);

	textbuf_emit(&f->buf);

	printf("\t.size\t%s, .-%s\n", name, name);

	func_cleanup(f);
	current_func = NULL;
}

/* emit object (a.k.a. variable, a.k.a. data) prologue */
static void emit_object_pre(const char *name, unsigned long modifiers,
			    unsigned long alignment, unsigned int byte_size)
{
	if ((modifiers & MOD_STATIC) == 0)
		printf(".globl %s\n", name);
	emit_section(".data");
	if (alignment)
		printf("\t.align %lu\n", alignment);
	printf("\t.type\t%s, @object\n", name);
	printf("\t.size\t%s, %d\n", name, byte_size);
	printf("%s:\n", name);
}

/* emit value (only) for an initializer scalar */
static void emit_scalar(struct expression *expr, unsigned int bit_size)
{
	const char *type;
	long long ll;

	assert(expr->type == EXPR_VALUE);

	if (expr->value == 0ULL) {
		printf("\t.zero\t%d\n", bit_size / 8);
		return;
	}

	ll = (long long) expr->value;

	switch (bit_size) {
	case 8:		type = "byte";	ll = (char) ll; break;
	case 16:	type = "value";	ll = (short) ll; break;
	case 32:	type = "long";	ll = (int) ll; break;
	case 64:	type = "quad";	break;
	default:	type = NULL;	break;
	}

	assert(type != NULL);

	printf("\t.%s\t%Ld\n", type, ll);
}

static void emit_global_noinit(const char *name, unsigned long modifiers,
			       unsigned long alignment, unsigned int byte_size)
{
	char s[64];

	if (modifiers & MOD_STATIC) {
		sprintf(s, "\t.local\t%s\n", name);
		textbuf_push(&unit_post_text, s);
	}
	if (alignment)
		sprintf(s, "\t.comm\t%s,%d,%lu\n", name, byte_size, alignment);
	else
		sprintf(s, "\t.comm\t%s,%d\n", name, byte_size);
	textbuf_push(&unit_post_text, s);
}

static int ea_current, ea_last;

static void emit_initializer(struct symbol *sym,
			     struct expression *expr)
{
	int distance = ea_current - ea_last - 1;

	if (distance > 0)
		printf("\t.zero\t%d\n", (sym->bit_size / 8) * distance);

	if (expr->type == EXPR_VALUE) {
		struct symbol *base_type = sym->ctype.base_type;
		assert(base_type != NULL);

		emit_scalar(expr, sym->bit_size / base_type->array_size);
		return;
	}
	if (expr->type != EXPR_INITIALIZER)
		return;

	assert(0); /* FIXME */
}

static int sort_array_cmp(const struct expression *a,
			  const struct expression *b)
{
	int a_ofs = 0, b_ofs = 0;

	if (a->type == EXPR_POS)
		a_ofs = (int) a->init_offset;
	if (b->type == EXPR_POS)
		b_ofs = (int) b->init_offset;

	return a_ofs - b_ofs;
}

/* move to front-end? */
static void sort_array(struct expression *expr)
{
	struct expression *entry, **list;
	unsigned int elem, sorted, i;

	elem = 0;
	FOR_EACH_PTR(expr->expr_list, entry) {
		elem++;
	} END_FOR_EACH_PTR;

	if (!elem)
		return;

	list = malloc(sizeof(entry) * elem);
	if (!list)
		die("OOM in sort_array");

	/* this code is no doubt evil and ignores EXPR_INDEX possibly
	 * to its detriment and other nasty things.  improvements
	 * welcome.
	 */
	i = 0;
	sorted = 0;
	FOR_EACH_PTR(expr->expr_list, entry) {
		if ((entry->type == EXPR_POS) || (entry->type == EXPR_VALUE)) {
			/* add entry to list[], in sorted order */
			if (sorted == 0) {
				list[0] = entry;
				sorted = 1;
			} else {
				unsigned int i;

				for (i = 0; i < sorted; i++)
					if (sort_array_cmp(entry, list[i]) <= 0)
						break;

				/* If inserting into the middle of list[]
				 * instead of appending, we memmove.
				 * This is ugly, but thankfully
				 * uncommon.  Input data with tons of
				 * entries very rarely have explicit
				 * offsets.  convert to qsort eventually...
				 */
				if (i != sorted)
					memmove(&list[i + 1], &list[i],
						(sorted - i) * sizeof(entry));
				list[i] = entry;
				sorted++;
			}
		}
	} END_FOR_EACH_PTR;

	i = 0;
	FOR_EACH_PTR(expr->expr_list, entry) {
		if ((entry->type == EXPR_POS) || (entry->type == EXPR_VALUE))
			__list->list[__i] = list[i++];
	} END_FOR_EACH_PTR;

}

static void emit_array(struct symbol *sym)
{
	struct symbol *base_type = sym->ctype.base_type;
	struct expression *expr = sym->initializer;
	struct expression *entry;

	assert(base_type != NULL);

	stor_sym_init(sym);

	ea_last = -1;

	emit_object_pre(show_ident(sym->ident), sym->ctype.modifiers,
		        sym->ctype.alignment,
			sym->bit_size / 8);

	sort_array(expr);

	FOR_EACH_PTR(expr->expr_list, entry) {
		if (entry->type == EXPR_VALUE) {
			ea_current = 0;
			emit_initializer(sym, entry);
			ea_last = ea_current;
		} else if (entry->type == EXPR_POS) {
			ea_current =
			    entry->init_offset / (base_type->bit_size / 8);
			emit_initializer(sym, entry->init_expr);
			ea_last = ea_current;
		}
	} END_FOR_EACH_PTR;
}

static void emit_one_symbol(struct symbol *sym, void *dummy, int flags)
{
	x86_symbol(sym);
}

void emit_unit(const char *basename, struct symbol_list *list)
{
	emit_unit_pre(basename);
	symbol_iterate(list, emit_one_symbol, NULL);
	emit_unit_post();
}

static void emit_move(struct expression *dest_expr, struct storage *dest,
		      struct storage *src, int bits)
{
	/* FIXME: Bitfield move! */

	/* FIXME: pay attention to arg 'bits' */
	insn("movl", stor_op_name(src), "%eax", stor_arg_warning(src));

	/* FIXME: pay attention to arg 'bits' */
	insn("movl", "%eax", stor_op_name(dest), stor_arg_warning(dest));
}

static void emit_store(struct expression *dest_expr, struct storage *dest,
		       struct storage *src, int bits)
{
	/* FIXME: Bitfield store! */
	printf("\tst.%d\t\tv%d,[v%d]\n", bits, src->pseudo, dest->pseudo);
}

static void emit_scalar_noinit(struct symbol *sym)
{
	emit_global_noinit(show_ident(sym->ident),
			   sym->ctype.modifiers, sym->ctype.alignment,
			   sym->bit_size / 8);
	stor_sym_init(sym);
}

static void emit_array_noinit(struct symbol *sym)
{
	emit_global_noinit(show_ident(sym->ident),
			   sym->ctype.modifiers, sym->ctype.alignment,
			   sym->array_size * (sym->bit_size / 8));
	stor_sym_init(sym);
}

static struct storage *emit_compare(struct expression *expr)
{
	struct storage *left = x86_expression(expr->left);
	struct storage *right = x86_expression(expr->right);
	struct storage *new;
	const char *opname = NULL;
	static const char *name[] = {
		['<'] = "cmovl",
		['>'] = "cmovg",
	};
	unsigned int op = expr->op;

	if (op < sizeof(name)/sizeof(*name)) {
		opname = name[op];
		assert(opname != NULL);
	} else {
		const char *tmp = show_special(op);
		if (!strcmp(tmp, "<="))
			opname = "cmovle";
		else if (!strcmp(tmp, ">="))
			opname = "cmovge";
		else if (!strcmp(tmp, "=="))
			opname = "cmove";
		else if (!strcmp(tmp, "!="))
			opname = "cmovne";
		else {
			assert(0);
		}
	}

	/* init ECX to 1 */
	insn("movl", "$1", "%ecx", "EXPR_COMPARE");

	/* init EDX to 0 */
	insn("xorl", "%edx", "%edx", NULL);

	/* FIXME: don't hardcode operand size */
	/* move op1 into EAX */
	insn("movl", stor_op_name(left), "%eax", NULL);

	/* perform comparison, EAX (op1) and op2 */
	insn("cmpl", "%eax", stor_op_name(right), NULL);

	/* store result of operation, 0 or 1, in EDX using CMOV */
	/* FIXME: does this need an operand size suffix? */
	insn(opname, "%ecx", "%edx", NULL);

	/* finally, store the result (EDX) in a new pseudo / stack slot */
	new = new_pseudo();
	insn("movl", "%edx", stor_op_name(new), "end EXPR_COMPARE");

	return new;
}

/*
 * TODO: create a new type STOR_VALUE.  This will allow us to store
 * the constant internally, and avoid assigning stack slots to them.
 */
static struct storage *emit_value(struct expression *expr)
{
	struct storage *new = new_pseudo();
	char s[32];

	sprintf(s, "$%Ld", (long long) expr->value);
	insn("movl", s, pretty_offset(pseudo_offset(new)), NULL);

	return new;
}

static struct storage *emit_binop(struct expression *expr)
{
	struct storage *left = x86_expression(expr->left);
	struct storage *right = x86_expression(expr->right);
	struct storage *new;
	const char *opname;
	static const char *name[] = {
		['+'] = "addl", ['-'] = "subl",
		['*'] = "mull", ['/'] = "divl",
		['%'] = "modl", ['&'] = "andl",
		['|'] = "orl", ['^'] = "xorl"
	};
	unsigned int op = expr->op;

	/*
	 * FIXME FIXME this routine is so wrong it's not even funny.
	 * On x86 both mod/div are handled with the same instruction.
	 * We don't pay attention to signed/unsigned issues,
	 * and like elsewhere we hardcode the operand size at 32 bits.
	 */

	opname = show_special(op);
	if (op < sizeof(name)/sizeof(*name)) {
		opname = name[op];
		assert(opname != NULL);
	} else
		assert(0); /* FIXME: no operations other than name[], ATM */

	/* load op2 into EAX */
	insn("movl", stor_op_name(right), "%eax", "EXPR_BINOP/COMMA/LOGICAL");

	/* perform binop */
	insn(opname, stor_op_name(left), "%eax", NULL);

	/* store result (EAX) in new pseudo / stack slot */
	new = new_pseudo();
	insn("movl", "%eax", stor_op_name(new), "end EXPR_BINOP");

	return new;
}

static void emit_if_conditional(struct statement *stmt)
{
	struct function *f = current_func;
	struct storage *val;
	int target;
	struct expression *cond = stmt->if_conditional;
	char s[16];

/* This is only valid if nobody can jump into the "dead" statement */
#if 0
	if (cond->type == EXPR_VALUE) {
		struct statement *s = stmt->if_true;
		if (!cond->value)
			s = stmt->if_false;
		x86_statement(s);
		break;
	}
#endif
	val = x86_expression(cond);

	/* load 'if' test result into EAX */
	insn("movl", stor_op_name(val), "%eax",
	     stor_arg_warning(val) ? stor_arg_warning(val) :
	     "begin if conditional");

	/* clear ECX */
	insn("xorl", "%ecx", "%ecx", NULL);

	/* compare 'if' test result */
	insn("cmpl", "%eax", "%ecx", NULL);

	/* create end-of-if label / if-failed labelto jump to,
	 * and jump to it if the expression returned zero.
	 */
	target = new_label();
	sprintf(s, ".L%d", target);
	insn("je", s, NULL, NULL);

	x86_statement(stmt->if_true);
	if (stmt->if_false) {
		int last = new_label();

		/* finished generating code for if-true statement.
		 * add a jump-to-end jump to avoid falling through
		 * to the if-false statement code.
		 */
		sprintf(s, ".L%d", last);
		insn("jmp", s, NULL, NULL);

		/* if we have both if-true and if-false statements,
		 * the failed-conditional case will fall through to here
		 */
		sprintf(s, ".L%d:\n", target);
		textbuf_push(&f->buf, s);

		target = last;
		x86_statement(stmt->if_false);
	}

	sprintf(s, ".L%d:\t\t\t\t\t# end if\n", target);
	textbuf_push(&f->buf, s);
}

static struct storage *emit_inc_dec(struct expression *expr, int postop)
{
	struct storage *addr = x86_address_gen(expr->unop);
	struct storage *retval;
	const char *opname = expr->op == SPECIAL_INCREMENT ? "incl" : "decl";
#if 0
	/* FIXME: don't hardware operand size */
	int bits = expr->ctype->bit_size;
#endif

	if (postop) {
		struct storage *new = new_pseudo();

		emit_move(NULL, new, addr, 1234 /* intentionally bogus */);

		retval = new;
	} else
		retval = addr;

	insn(opname, stor_op_name(addr), NULL, NULL);

	return retval;
}

static struct storage *emit_postop(struct expression *expr)
{
	return emit_inc_dec(expr, 1);
}

static void x86_struct_member(struct symbol *sym, void *data, int flags)
{
	if (flags & ITERATE_FIRST)
		printf(" {\n\t");
	printf("%s:%d:%ld at offset %ld", show_ident(sym->ident), sym->bit_size, sym->ctype.alignment, sym->offset);
	if (sym->fieldwidth)
		printf("[%d..%d]", sym->bit_offset, sym->bit_offset+sym->fieldwidth-1);
	if (flags & ITERATE_LAST)
		printf("\n} ");
	else
		printf(", ");
}

static void x86_symbol(struct symbol *sym)
{
	struct symbol *type;

	if (!sym)
		return;

	type = sym->ctype.base_type;
	if (!type)
		return;

	/*
	 * Show actual implementation information
	 */
	switch (type->type) {

	case SYM_ARRAY:
		if (sym->initializer)
			emit_array(sym);
		else
			emit_array_noinit(sym);
		break;

	case SYM_BASETYPE:
		if (sym->initializer) {
			emit_object_pre(show_ident(sym->ident),
					sym->ctype.modifiers,
				        sym->ctype.alignment,
					sym->bit_size / 8);
			emit_scalar(sym->initializer, sym->bit_size);
			stor_sym_init(sym);
		} else
			emit_scalar_noinit(sym);
		break;

	case SYM_STRUCT:
		symbol_iterate(type->symbol_list, x86_struct_member, NULL);
		break;

	case SYM_UNION:
		symbol_iterate(type->symbol_list, x86_struct_member, NULL);
		break;

	case SYM_FN: {
		struct statement *stmt = type->stmt;
		if (stmt) {
			struct storage *val;

			emit_func_pre(sym);
			val = x86_statement(stmt);
			emit_func_post(sym, val);
		}
		break;
	}

	default:
		break;
	}

	if (sym->initializer && (type->type != SYM_BASETYPE) &&
	    (type->type != SYM_ARRAY)) {
		printf(" = \n");
		x86_expression(sym->initializer);
	}
}

static void x86_symbol_init(struct symbol *sym);

static void x86_switch_statement(struct statement *stmt)
{
	struct storage *val = x86_expression(stmt->switch_expression);
	struct symbol *sym;
	printf("\tswitch v%d\n", val->pseudo);

	/*
	 * Debugging only: Check that the case list is correct
	 * by printing it out.
	 *
	 * This is where a _real_ back-end would go through the
	 * cases to decide whether to use a lookup table or a
	 * series of comparisons etc
	 */
	printf("# case table:\n");
	FOR_EACH_PTR(stmt->switch_case->symbol_list, sym) {
		struct statement *case_stmt = sym->stmt;
		struct expression *expr = case_stmt->case_expression;
		struct expression *to = case_stmt->case_to;

		if (!expr) {
			printf("    default");
		} else {
			if (expr->type == EXPR_VALUE) {
				printf("    case %lld", expr->value);
				if (to) {
					if (to->type == EXPR_VALUE) {
						printf(" .. %lld", to->value);
					} else {
						printf(" .. what?");
					}
				}
			} else
				printf("    what?");
		}
		printf(": .L%p\n", sym);
	} END_FOR_EACH_PTR;
	printf("# end case table\n");

	x86_statement(stmt->switch_statement);

	if (stmt->switch_break->used)
		printf(".L%p:\n", stmt->switch_break);
}

static void x86_symbol_decl(struct symbol_list *syms)
{
	struct symbol *sym;
	FOR_EACH_PTR(syms, sym) {
		x86_symbol_init(sym);
	} END_FOR_EACH_PTR;
}

static void x86_return_stmt(struct statement *stmt);

/*
 * Print out a statement
 */
static struct storage *x86_statement(struct statement *stmt)
{
	if (!stmt)
		return 0;
	switch (stmt->type) {
	case STMT_RETURN:
		x86_return_stmt(stmt);
		return NULL;
	case STMT_COMPOUND: {
		struct statement *s;
		struct storage *last = NULL;

		x86_symbol_decl(stmt->syms);
		FOR_EACH_PTR(stmt->stmts, s) {
			last = x86_statement(s);
		} END_FOR_EACH_PTR;
		if (stmt->ret) {
			struct storage *addr;
			int bits;
			printf(".L%p:\n", stmt->ret);
			addr = x86_symbol_expr(stmt->ret);
			bits = stmt->ret->bit_size;
			last = new_pseudo();
			printf("\tld.%d\t\tv%d,[v%d]\n", bits, last->pseudo, addr->pseudo);
		}
		return last;
	}

	case STMT_EXPRESSION:
		return x86_expression(stmt->expression);
	case STMT_IF:
		emit_if_conditional(stmt);
		return NULL;
	case STMT_SWITCH:
		x86_switch_statement(stmt);
		break;

	case STMT_CASE:
		printf(".L%p:\n", stmt->case_label);
		x86_statement(stmt->case_statement);
		break;

	case STMT_ITERATOR: {
		struct statement  *pre_statement = stmt->iterator_pre_statement;
		struct expression *pre_condition = stmt->iterator_pre_condition;
		struct statement  *statement = stmt->iterator_statement;
		struct statement  *post_statement = stmt->iterator_post_statement;
		struct expression *post_condition = stmt->iterator_post_condition;
		int loop_top = 0, loop_bottom = 0;
		struct storage *val;

		x86_symbol_decl(stmt->iterator_syms);
		x86_statement(pre_statement);
		if (pre_condition) {
			if (pre_condition->type == EXPR_VALUE) {
				if (!pre_condition->value) {
					loop_bottom = new_label();
					printf("\tjmp\t\t.L%d\n", loop_bottom);
				}
			} else {
				loop_bottom = new_label();
				val = x86_expression(pre_condition);
				printf("\tje\t\tv%d, .L%d\n", val->pseudo, loop_bottom);
			}
		}
		if (!post_condition || post_condition->type != EXPR_VALUE || post_condition->value) {
			loop_top = new_label();
			printf(".L%d:\n", loop_top);
		}
		x86_statement(statement);
		if (stmt->iterator_continue->used)
			printf(".L%p:\n", stmt->iterator_continue);
		x86_statement(post_statement);
		if (!post_condition) {
			printf("\tjmp\t\t.L%d\n", loop_top);
		} else if (post_condition->type == EXPR_VALUE) {
			if (post_condition->value)
				printf("\tjmp\t\t.L%d\n", loop_top);
		} else {
			val = x86_expression(post_condition);
			printf("\tjne\t\tv%d, .L%d\n", val->pseudo, loop_top);
		}
		if (stmt->iterator_break->used)
			printf(".L%p:\n", stmt->iterator_break);
		if (loop_bottom)
			printf(".L%d:\n", loop_bottom);
		break;
	}
	case STMT_NONE:
		break;

	case STMT_LABEL:
		printf(".L%p:\n", stmt->label_identifier);
		x86_statement(stmt->label_statement);
		break;

	case STMT_GOTO:
		if (stmt->goto_expression) {
			struct storage *val = x86_expression(stmt->goto_expression);
			printf("\tgoto *v%d\n", val->pseudo);
		} else {
			printf("\tgoto .L%p\n", stmt->goto_label);
		}
		break;
	case STMT_ASM:
		printf("\tasm( .... )\n");
		break;

	}
	return 0;
}

static struct storage *x86_call_expression(struct expression *expr)
{
	struct symbol *direct;
	struct expression *arg, *fn;
	struct storage *retval, *fncall;
	int framesize;
	char s[64];

	if (!expr->ctype) {
		warn(expr->pos, "\tcall with no type!");
		return NULL;
	}

	framesize = 0;
	FOR_EACH_PTR_REVERSE(expr->args, arg) {
		struct storage *new = x86_expression(arg);
		int size = arg->ctype->bit_size;

		/* FIXME: pay attention to 'size' */
		insn("pushl", stor_op_name(new), NULL,
		     stor_arg_warning(new) ? stor_arg_warning(new) :
		     !framesize ? "begin function call" : NULL);

		framesize += size >> 3;
	} END_FOR_EACH_PTR_REVERSE;

	fn = expr->fn;

	/* Remove dereference, if any */
	direct = NULL;
	if (fn->type == EXPR_PREOP) {
		if (fn->unop->type == EXPR_SYMBOL) {
			struct symbol *sym = fn->unop->symbol;
			if (sym->ctype.base_type->type == SYM_FN)
				direct = sym;
		}
	}
	if (direct)
		insn("call", show_ident(direct->ident), NULL, NULL);
	else {
		fncall = x86_expression(fn);
		printf("\tcall\t*v%d\n", fncall->pseudo);
	}

	/* FIXME: pay attention to BITS_IN_POINTER */
	if (framesize) {
		sprintf(s, "$%d", framesize);
		insn("addl", s, "%esp", "end function call");
	}

	retval = new_pseudo();
	printf("\tmov.%d\t\tv%d,retval\n", expr->ctype->bit_size, retval->pseudo);
	return retval;
}

static struct storage *x86_regular_preop(struct expression *expr)
{
	struct storage *target = x86_expression(expr->unop);
	struct storage *new = new_pseudo();
	static const char *name[] = {
		['!'] = "nonzero", ['-'] = "neg",
		['~'] = "not",
	};
	unsigned int op = expr->op;
	const char *opname;

	opname = show_special(op);
	if (op < sizeof(name)/sizeof(*name))
		opname = name[op];
	printf("\t%s.%d\t\tv%d,v%d\n", opname, expr->ctype->bit_size, new->pseudo, target->pseudo);
	return new;
}

/*
 * FIXME! Not all accesses are memory loads. We should
 * check what kind of symbol is behind the dereference.
 */
static struct storage *x86_address_gen(struct expression *expr)
{
	if (expr->type == EXPR_PREOP)
		return x86_expression(expr->unop);
	return x86_expression(expr->address);
}

static struct storage *x86_assignment(struct expression *expr)
{
	struct expression *target = expr->left;
	struct storage *val, *addr;
	int bits;

	if (!expr->ctype)
		return NULL;

	bits = expr->ctype->bit_size;
	val = x86_expression(expr->right);
	addr = x86_address_gen(target);
	emit_move(target, addr, val, bits);
	return val;
}

static void x86_return_stmt(struct statement *stmt)
{
	struct expression *expr = stmt->ret_value;
	struct symbol *target = stmt->ret_target;

	if (expr && expr->ctype) {
		struct storage *val = x86_expression(expr);
		int bits = expr->ctype->bit_size;
		struct storage *addr = x86_symbol_expr(target);
		emit_store(NULL, addr, val, bits);
	}
	printf("\tgoto .L%p\n", target);
}

static int x86_initialization(struct symbol *sym, struct expression *expr)
{
	struct storage *val, *addr;
	int bits;

	if (!expr->ctype)
		return 0;

	bits = expr->ctype->bit_size;
	val = x86_expression(expr);
	addr = x86_symbol_expr(sym);
	// FIXME! The "target" expression is for bitfield store information.
	// Leave it NULL, which works fine.
	emit_store(NULL, addr, val, bits);
	return 0;
}

static struct storage *x86_access(struct expression *expr)
{
	return x86_address_gen(expr);
}

static struct storage *x86_preop(struct expression *expr)
{
	/*
	 * '*' is an lvalue access, and is fundamentally different
	 * from an arithmetic operation. Maybe it should have an
	 * expression type of its own..
	 */
	if (expr->op == '*')
		return x86_access(expr);
	if (expr->op == SPECIAL_INCREMENT || expr->op == SPECIAL_DECREMENT)
		return emit_inc_dec(expr, 0);
	return x86_regular_preop(expr);
}

static struct storage *x86_symbol_expr(struct symbol *sym)
{
	struct storage *new = new_pseudo();

	if (sym->ctype.modifiers & (MOD_TOPLEVEL | MOD_EXTERN | MOD_STATIC)) {
		printf("\tmovi.%d\t\tv%d,$%s\n", BITS_IN_POINTER, new->pseudo, show_ident(sym->ident));
		return new;
	}
	if (sym->ctype.modifiers & MOD_ADDRESSABLE) {
		printf("\taddi.%d\t\tv%d,vFP,$%lld\n", BITS_IN_POINTER, new->pseudo, sym->value);
		return new;
	}
	printf("\taddi.%d\t\tv%d,vFP,$offsetof(%s:%p)\n", BITS_IN_POINTER, new->pseudo, show_ident(sym->ident), sym);
	return new;
}

static void x86_symbol_init(struct symbol *sym)
{
	struct symbol_private *priv = sym->aux;
	struct expression *expr = sym->initializer;
	struct storage *new;

	if (expr)
		new = x86_expression(expr);
	else
		new = new_pseudo();

	if (!priv) {
		priv = calloc(1, sizeof(*priv));
		sym->aux = priv;
		/* FIXME: leak! we don't free... */
		/* (well, we don't free symbols either) */
	}

	priv->addr = new;
}

static int type_is_signed(struct symbol *sym)
{
	if (sym->type == SYM_NODE)
		sym = sym->ctype.base_type;
	if (sym->type == SYM_PTR)
		return 0;
	return !(sym->ctype.modifiers & MOD_UNSIGNED);
}

static struct storage *x86_cast_expr(struct expression *expr)
{
	struct symbol *old_type, *new_type;
	struct storage *op = x86_expression(expr->cast_expression);
	int oldbits, newbits;
	int is_signed;
	struct storage *new;

	old_type = expr->cast_expression->ctype;
	new_type = expr->cast_type;

	oldbits = old_type->bit_size;
	newbits = new_type->bit_size;
	if (oldbits >= newbits)
		return op;
	new = new_pseudo();
	is_signed = type_is_signed(old_type);
	if (is_signed) {
		printf("\tsext%d.%d\tv%d,v%d\n", oldbits, newbits, new->pseudo, op->pseudo);
	} else {
		printf("\tandl.%d\t\tv%d,v%d,$%lu\n", newbits, new->pseudo, op->pseudo, (1UL << oldbits)-1);
	}
	return new;
}

static struct storage *show_string_expr(struct expression *expr)
{
	struct storage *new = new_pseudo();

	printf("\tmovi.%d\t\tv%d,&%s\n", BITS_IN_POINTER, new->pseudo,
		show_string(expr->string));
	return new;
}

static struct storage *x86_bitfield_expr(struct expression *expr)
{
	return x86_access(expr);
}

static struct storage *x86_label_expr(struct expression *expr)
{
	struct storage *new = new_pseudo();
	printf("\tmovi.%d\t\tv%d,.L%p\n",BITS_IN_POINTER, new->pseudo, expr->label_symbol);
	return new;
}

static struct storage *x86_conditional_expr(struct expression *expr)
{
	struct storage *cond = x86_expression(expr->conditional);
	struct storage *true = x86_expression(expr->cond_true);
	struct storage *false = x86_expression(expr->cond_false);
	struct storage *new = new_pseudo();

	if (!true)
		true = cond;
	printf("[v%d]\tcmov.%d\t\tv%d,v%d,v%d\n",
		cond->pseudo, expr->ctype->bit_size, new->pseudo,
		true->pseudo, false->pseudo);
	return new;
}

static struct storage *x86_statement_expr(struct expression *expr)
{
	return x86_statement(expr->statement);
}

static int x86_position_expr(struct expression *expr, struct symbol *base)
{
	struct storage *new = x86_expression(expr->init_expr);
	struct symbol *ctype = expr->init_sym;

	printf("\tinsert v%d at [%d:%d] of %s\n", new->pseudo,
		expr->init_offset, ctype->bit_offset,
		show_ident(base->ident));
	return 0;
}

static void x86_initializer_expr(struct expression *expr, struct symbol *ctype)
{
	struct expression *entry;

	FOR_EACH_PTR(expr->expr_list, entry) {
		// Nested initializers have their positions already
		// recursively calculated - just output them too
		if (entry->type == EXPR_INITIALIZER) {
			x86_initializer_expr(entry, ctype);
			continue;
		}

		// Ignore initializer indexes and identifiers - the
		// evaluator has taken them into account
		if (entry->type == EXPR_IDENTIFIER || entry->type == EXPR_INDEX)
			continue;
		if (entry->type == EXPR_POS) {
			x86_position_expr(entry, ctype);
			continue;
		}
		x86_initialization(ctype, entry);
	} END_FOR_EACH_PTR;
}

static struct storage *x86_symbol_expr_init(struct symbol *sym)
{
	struct expression *expr = sym->initializer;
	struct symbol_private *priv = sym->aux;

	if (expr)
		x86_initializer_expr(expr, expr->ctype);

	if (priv == NULL) {
		fprintf(stderr, "WARNING! priv == NULL\n");
		priv = calloc(1, sizeof(*priv));
		sym->aux = priv;
		priv->addr = new_pseudo(); /* this is wrong! */
	}

	return priv->addr;
}

/*
 * Print out an expression. Return the pseudo that contains the
 * variable.
 */
static struct storage *x86_expression(struct expression *expr)
{
	if (!expr)
		return 0;

	if (!expr->ctype) {
		struct position *pos = &expr->pos;
		printf("\tno type at %s:%d:%d\n",
			input_streams[pos->stream].name,
			pos->line, pos->pos);
		return 0;
	}

	switch (expr->type) {
	case EXPR_CALL:
		return x86_call_expression(expr);

	case EXPR_ASSIGNMENT:
		return x86_assignment(expr);

	case EXPR_COMPARE:
		return emit_compare(expr);
	case EXPR_BINOP:
	case EXPR_COMMA:
	case EXPR_LOGICAL:
		return emit_binop(expr);
	case EXPR_PREOP:
		return x86_preop(expr);
	case EXPR_POSTOP:
		return emit_postop(expr);
	case EXPR_SYMBOL:
		return x86_symbol_expr_init(expr->symbol);
	case EXPR_DEREF:
	case EXPR_SIZEOF:
		warn(expr->pos, "invalid expression after evaluation");
		return 0;
	case EXPR_CAST:
		return x86_cast_expr(expr);
	case EXPR_VALUE:
		return emit_value(expr);
	case EXPR_STRING:
		return show_string_expr(expr);
	case EXPR_BITFIELD:
		return x86_bitfield_expr(expr);
	case EXPR_INITIALIZER:
		x86_initializer_expr(expr, expr->ctype);
		return NULL;
	case EXPR_CONDITIONAL:
		return x86_conditional_expr(expr);
	case EXPR_STATEMENT:
		return x86_statement_expr(expr);
	case EXPR_LABEL:
		return x86_label_expr(expr);

	// None of these should exist as direct expressions: they are only
	// valid as sub-expressions of initializers.
	case EXPR_POS:
		warn(expr->pos, "unable to show plain initializer position expression");
		return 0;
	case EXPR_IDENTIFIER:
		warn(expr->pos, "unable to show identifier expression");
		return 0;
	case EXPR_INDEX:
		warn(expr->pos, "unable to show index expression");
		return 0;
	}
	return 0;
}