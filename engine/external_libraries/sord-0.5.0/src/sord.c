/*
  Copyright 2011 David Robillard <http://drobilla.net>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

// C99
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zix/hash.h"
#include "zix/tree.h"

#include "sord-config.h"
#include "sord_internal.h"

#define SORD_LOG(prefix, ...) fprintf(stderr, "[Sord::" prefix "] " __VA_ARGS__)

#ifdef SORD_DEBUG_ITER
#    define SORD_ITER_LOG(...) SORD_LOG("iter", __VA_ARGS__)
#else
#    define SORD_ITER_LOG(...)
#endif
#ifdef SORD_DEBUG_SEARCH
#    define SORD_FIND_LOG(...) SORD_LOG("search", __VA_ARGS__)
#else
#    define SORD_FIND_LOG(...)
#endif
#ifdef SORD_DEBUG_WRITE
#    define SORD_WRITE_LOG(...) SORD_LOG("write", __VA_ARGS__)
#else
#    define SORD_WRITE_LOG(...)
#endif

#define NUM_ORDERS          12
#define STATEMENT_LEN       3
#define TUP_LEN             STATEMENT_LEN + 1
#define DEFAULT_ORDER       SPO
#define DEFAULT_GRAPH_ORDER GSPO

#define TUP_FMT         "(%s %s %s %s)"
#define TUP_FMT_ELEM(e) ((e) ? sord_node_get_string(e) : (const uint8_t*)"*")
#define TUP_FMT_ARGS(t) \
	TUP_FMT_ELEM((t)[0]), \
	TUP_FMT_ELEM((t)[1]), \
	TUP_FMT_ELEM((t)[2]), \
	TUP_FMT_ELEM((t)[3])

#define TUP_S 0
#define TUP_P 1
#define TUP_O 2
#define TUP_G 3

/** Triple ordering */
typedef enum {
	SPO,   ///<         Subject,   Predicate, Object
	SOP,   ///<         Subject,   Object,    Predicate
	OPS,   ///<         Object,    Predicate, Subject
	OSP,   ///<         Object,    Subject,   Predicate
	PSO,   ///<         Predicate, Subject,   Object
	POS,   ///<         Predicate, Object,    Subject
	GSPO,  ///< Graph,  Subject,   Predicate, Object
	GSOP,  ///< Graph,  Subject,   Object,    Predicate
	GOPS,  ///< Graph,  Object,    Predicate, Subject
	GOSP,  ///< Graph,  Object,    Subject,   Predicate
	GPSO,  ///< Graph,  Predicate, Subject,   Object
	GPOS,  ///< Graph,  Predicate, Object,    Subject
} SordOrder;

/** String name of each ordering (array indexed by SordOrder) */
static const char* const order_names[NUM_ORDERS] = {
	"spo",  "sop",  "ops",  "osp",  "pso",  "pos",
	"gspo", "gsop", "gops", "gosp", "gpso", "gpos"
};

/**
   Quads of indices for each order, from most to least significant
   (array indexed by SordOrder)
*/
static const int orderings[NUM_ORDERS][TUP_LEN] = {
	{  0,1,2,3}, {  0,2,1,3}, {  2,1,0,3}, {  2,0,1,3}, {  1,0,2,3}, {  1,2,0,3},
	{3,0,1,2  }, {3,0,2,1  }, {3,2,1,0  }, {3,2,0,1  }, {3,1,0,2  }, {3,1,2,0  }
};

/** World */
struct SordWorldImpl {
	ZixHash* names;     ///< URI or blank node identifier string => ID
	ZixHash* langs;     ///< Language tag => Interned language tag
	ZixHash* literals;  ///< Literal => ID
	size_t   n_nodes;   ///< Number of nodes
};

/** Store */
struct SordModelImpl {
	SordWorld* world;

	/** Index for each possible triple ordering (may or may not exist).
	 * If an index for e.g. SPO exists, it is a dictionary with
	 * (S P O) keys (as a SordTuplrID), and ignored values.
	 */
	ZixTree* indices[NUM_ORDERS];

	size_t n_quads;
};

/** Mode for searching or iteration */
typedef enum {
	ALL,           ///< Iterate to end of store, returning all results, no filtering
	SINGLE,        ///< Iteration over a single element (exact search)
	RANGE,         ///< Iterate over range with equal prefix
	FILTER_RANGE,  ///< Iterate over range with equal prefix, filtering
	FILTER_ALL     ///< Iterate to end of store, filtering
} SearchMode;

/** Iterator over some range of a store */
struct SordIterImpl {
	const SordModel* sord;               ///< Store this is an iterator for
	ZixTreeIter*     cur;                ///< Current DB cursor
	SordQuad         pat;                ///< Iteration pattern (in ordering order)
	int              ordering[TUP_LEN];  ///< Store ordering
	SearchMode       mode;               ///< Iteration mode
	int              n_prefix;           ///< Length of range prefix (RANGE, FILTER_RANGE)
	bool             end;                ///< True iff reached end
	bool             skip_graphs;        ///< True iff iteration should ignore graphs
};

static unsigned
sord_literal_hash(const void* n)
{
	SordNode* node = (SordNode*)n;
	return zix_string_hash(node->node.buf)
		+ (node->lang ? zix_string_hash(node->lang) : 0);
}

static bool
sord_literal_equal(const void* a, const void* b)
{
	SordNode* a_node = (SordNode*)a;
	SordNode* b_node = (SordNode*)b;
	return (a_node == b_node)
		|| (zix_string_equal(sord_node_get_string(a_node),
		                     sord_node_get_string(b_node))
		    && (a_node->lang == b_node->lang)
		    && (a_node->datatype == b_node->datatype));
}

SordWorld*
sord_world_new(void)
{
	SordWorld* world = malloc(sizeof(struct SordWorldImpl));
	world->names    = zix_hash_new(zix_string_hash, zix_string_equal);
	world->langs    = zix_hash_new(zix_string_hash, zix_string_equal);
	world->literals = zix_hash_new(sord_literal_hash, sord_literal_equal);
	world->n_nodes  = 0;
	return world;
}

static void
free_node_entry(const void* key, void* value, void* user_data)
{
	SordNode* node = (SordNode*)value;
	if (node->node.type == SERD_LITERAL) {
		sord_node_free((SordWorld*)user_data, node->datatype);
	}
	free((uint8_t*)node->node.buf);
	free(node);
}

static void
free_lang_entry(const void* key, void* value, void* user_data)
{
	free(value);
}

void
sord_world_free(SordWorld* world)
{
	zix_hash_foreach(world->literals, free_node_entry, world);
	zix_hash_foreach(world->names, free_node_entry, world);
	zix_hash_foreach(world->langs, free_lang_entry, world);
	zix_hash_free(world->names);
	zix_hash_free(world->langs);
	zix_hash_free(world->literals);
	free(world);
}

static inline int
sord_node_compare(const SordNode* a, const SordNode* b)
{
	if (a == b || !a || !b) {
		return 0;
	} else if (a->node.type != b->node.type) {
		return a->node.type - b->node.type;
	}

	int cmp;
	switch (a->node.type) {
	case SERD_URI:
	case SERD_BLANK:
		return strcmp((const char*)a->node.buf, (const char*)b->node.buf);
	case SERD_LITERAL:
		cmp = strcmp((const char*)sord_node_get_string(a),
		             (const char*)sord_node_get_string(b));
		if (cmp == 0) {
			cmp = sord_node_compare(a->datatype, b->datatype);
		}
		if (cmp == 0) {
			if (!a->lang || !b->lang) {
				cmp = a->lang - b->lang;
			} else {
				cmp = strcmp(a->lang, b->lang);
			}
		}
		return cmp;
	default:
		break;  // never reached
	}
	assert(false);
	return 0;
}

bool
sord_node_equals(const SordNode* a, const SordNode* b)
{
	if (!a || !b) {
		return (a == b);
	} else {
		// FIXME: nodes are interned, this can be much faster
		return (a == b) || (sord_node_compare(a, b) == 0);
	}
}

/** Return true iff IDs are equivalent, or one is a wildcard */
static inline bool
sord_id_match(const SordNode* a, const SordNode* b)
{
	return !a || !b || (a == b);
}

static inline bool
sord_quad_match_inline(const SordQuad x, const SordQuad y)
{
	return sord_id_match(x[0], y[0])
		&& sord_id_match(x[1], y[1])
		&& sord_id_match(x[2], y[2])
		&& sord_id_match(x[3], y[3]);
}

bool
sord_quad_match(const SordQuad x, const SordQuad y)
{
	return sord_quad_match_inline(x, y);
}

/**
   Compare two quad IDs lexicographically.
   NULL IDs (equal to 0) are treated as wildcards, always less than every
   other possible ID, except itself.
*/
static int
sord_quad_compare(const void* x_ptr, const void* y_ptr, void* user_data)
{
	const int* const ordering = (const int*)user_data;
	SordNode** const x        = (SordNode**)x_ptr;
	SordNode** const y        = (SordNode**)y_ptr;

	for (int i = 0; i < TUP_LEN; ++i) {
		const int idx = ordering[i];
		const int cmp = sord_node_compare(x[idx], y[idx]);
		if (cmp) {
			return cmp;
		}
	}

	return 0;
}

static inline bool
sord_iter_forward(SordIter* iter)
{
	if (!iter->skip_graphs) {
		iter->cur = sord_zix_tree_iter_next(iter->cur);
		return sord_zix_tree_iter_is_end(iter->cur);
	}

	SordNode** key = (SordNode**)sord_zix_tree_get(iter->cur);
	const SordQuad initial = { key[0], key[1], key[2], key[3] };
	while (true) {
		iter->cur = sord_zix_tree_iter_next(iter->cur);
		if (sord_zix_tree_iter_is_end(iter->cur))
			return true;

		key = (SordNode**)sord_zix_tree_get(iter->cur);
		for (int i = 0; i < 3; ++i)
			if (key[i] != initial[i])
				return false;
	}
	assert(false);
}

/**
   Seek forward as necessary until @a iter points at a match.
   @return true iff iterator reached end of valid range.
*/
static inline bool
sord_iter_seek_match(SordIter* iter)
{
	for (iter->end = true;
	     !sord_zix_tree_iter_is_end(iter->cur);
	     sord_iter_forward(iter)) {
		const SordNode** const key = (const SordNode**)sord_zix_tree_get(iter->cur);
		if (sord_quad_match_inline(key, iter->pat))
			return (iter->end = false);
	}
	return true;
}

/**
   Seek forward as necessary until @a iter points at a match, or the prefix
   no longer matches iter->pat.
   @return true iff iterator reached end of valid range.
*/
static inline bool
sord_iter_seek_match_range(SordIter* iter)
{
	if (iter->end)
		return true;

	do {
		const SordNode** key = (const SordNode**)sord_zix_tree_get(iter->cur);

		if (sord_quad_match_inline(key, iter->pat))
			return false;  // Found match

		for (int i = 0; i < iter->n_prefix; ++i) {
			const int idx = iter->ordering[i];
			if (!sord_id_match(key[idx], iter->pat[idx])) {
				iter->end = true;  // Reached end of valid range
				return true;
			}
		}
	} while (!sord_iter_forward(iter));

	return (iter->end = true);  // Reached end
}

static SordIter*
sord_iter_new(const SordModel* sord, ZixTreeIter* cur, const SordQuad pat,
              SordOrder order, SearchMode mode, int n_prefix)
{
	const int* ordering = orderings[order];

	SordIter* iter = malloc(sizeof(struct SordIterImpl));
	iter->sord        = sord;
	iter->cur         = cur;
	iter->mode        = mode;
	iter->n_prefix    = n_prefix;
	iter->end         = false;
	iter->skip_graphs = order < GSPO;
	for (int i = 0; i < TUP_LEN; ++i) {
		iter->pat[i]      = pat[i];
		iter->ordering[i] = ordering[i];
	}

	switch (iter->mode) {
	case ALL:
	case SINGLE:
	case RANGE:
		assert(
			sord_quad_match_inline((const SordNode**)sord_zix_tree_get(iter->cur),
			                       iter->pat));
		break;
	case FILTER_RANGE:
		sord_iter_seek_match_range(iter);
		break;
	case FILTER_ALL:
		sord_iter_seek_match(iter);
		break;
	}

#ifdef SORD_DEBUG_ITER
	SordQuad value;
	sord_iter_get(iter, value);
	SORD_ITER_LOG("New %p pat=" TUP_FMT " cur=" TUP_FMT " end=%d skip=%d\n",
	              (void*)iter, TUP_FMT_ARGS(pat), TUP_FMT_ARGS(value),
	              iter->end, iter->skip_graphs);
#endif
	return iter;
}

const SordModel*
sord_iter_get_model(SordIter* iter)
{
	return iter->sord;
}

void
sord_iter_get(const SordIter* iter, SordQuad id)
{
	SordNode** key = (SordNode**)sord_zix_tree_get(iter->cur);
	for (int i = 0; i < TUP_LEN; ++i) {
		id[i] = key[i];
	}
}

bool
sord_iter_next(SordIter* iter)
{
	if (iter->end)
		return true;

	const SordNode** key;
	iter->end = sord_iter_forward(iter);
	if (!iter->end) {
		switch (iter->mode) {
		case ALL:
			// At the end if the cursor is (assigned above)
			break;
		case SINGLE:
			iter->end = true;
			SORD_ITER_LOG("%p reached single end\n", (void*)iter);
			break;
		case RANGE:
			SORD_ITER_LOG("%p range next\n", (void*)iter);
			// At the end if the MSNs no longer match
			key = (const SordNode**)sord_zix_tree_get(iter->cur);
			assert(key);
			for (int i = 0; i < iter->n_prefix; ++i) {
				const int idx = iter->ordering[i];
				if (!sord_id_match(key[idx], iter->pat[idx])) {
					iter->end = true;
					SORD_ITER_LOG("%p reached non-match end\n", (void*)iter);
					break;
				}
			}
			break;
		case FILTER_RANGE:
			// Seek forward to next match, stopping if prefix changes
			sord_iter_seek_match_range(iter);
			break;
		case FILTER_ALL:
			// Seek forward to next match
			sord_iter_seek_match(iter);
			break;
		}
	} else {
		SORD_ITER_LOG("%p reached index end\n", (void*)iter);
	}

	if (iter->end) {
		SORD_ITER_LOG("%p Reached end\n", (void*)iter);
		return true;
	} else {
#ifdef SORD_DEBUG_ITER
		SordQuad tup;
		sord_iter_get(iter, tup);
		SORD_ITER_LOG("%p Increment to " TUP_FMT "\n",
		              (void*)iter, TUP_FMT_ARGS(tup));
#endif
		return false;
	}
}

bool
sord_iter_end(const SordIter* iter)
{
	return !iter || iter->end;
}

void
sord_iter_free(SordIter* iter)
{
	SORD_ITER_LOG("%p Free\n", (void*)iter);
	if (iter) {
		free(iter);
	}
}

/**
   Return true iff @a sord has an index for @a order.
   If @a graph_search is true, @a order will be modified to be the
   corresponding order with a G prepended (so G will be the MSN).
*/
static inline bool
sord_has_index(SordModel* sord, SordOrder* order, int* n_prefix, bool graph_search)
{
	if (graph_search) {
		*order    += GSPO;
		*n_prefix += 1;
	}

	return sord->indices[*order];
}

/**
   Return the best available index for a pattern.
   @param pat Pattern in standard (S P O G) order
   @param mode Set to the (best) iteration mode for iterating over results
   @param n_prefix Set to the length of the range prefix
   (for @a mode == RANGE and @a mode == FILTER_RANGE)
*/
static inline SordOrder
sord_best_index(SordModel* sord, const SordQuad pat, SearchMode* mode, int* n_prefix)
{
	const bool graph_search = (pat[TUP_G] != 0);

	const unsigned sig
		= (pat[0] ? 1 : 0) * 0x100
		+ (pat[1] ? 1 : 0) * 0x010
		+ (pat[2] ? 1 : 0) * 0x001;

	SordOrder good[2] = { (SordOrder)-1, (SordOrder)-1 };

	// Good orderings that don't require filtering
	*mode     = RANGE;
	*n_prefix = 0;
	switch (sig) {
	case 0x000: *mode = ALL; return graph_search ? DEFAULT_GRAPH_ORDER : DEFAULT_ORDER;
	case 0x001: *mode = RANGE; good[0] = OPS; good[1] = OSP; *n_prefix = 1; break;
	case 0x010: *mode = RANGE; good[0] = POS; good[1] = PSO; *n_prefix = 1; break;
	case 0x011: *mode = RANGE; good[0] = OPS; good[1] = POS; *n_prefix = 2; break;
	case 0x100: *mode = RANGE; good[0] = SPO; good[1] = SOP; *n_prefix = 1; break;
	case 0x101: *mode = RANGE; good[0] = SOP; good[1] = OSP; *n_prefix = 2; break;
	case 0x110: *mode = RANGE; good[0] = SPO; good[1] = PSO; *n_prefix = 2; break;
	case 0x111: *mode = SINGLE; return graph_search ? DEFAULT_GRAPH_ORDER : DEFAULT_ORDER;
	}

	if (*mode == RANGE) {
		if (sord_has_index(sord, &good[0], n_prefix, graph_search)) {
			return good[0];
		} else if (sord_has_index(sord, &good[1], n_prefix, graph_search)) {
			return good[1];
		}
	}

	// Not so good orderings that require filtering, but can
	// still be constrained to a range
	switch (sig) {
	case 0x011: *mode = FILTER_RANGE; good[0] = OSP; good[1] = PSO; *n_prefix = 1; break;
	case 0x101: *mode = FILTER_RANGE; good[0] = SPO; good[1] = OPS; *n_prefix = 1; break;
	case 0x110: *mode = FILTER_RANGE; good[0] = SOP; good[1] = POS; *n_prefix = 1; break;
	default: break;
	}

	if (*mode == FILTER_RANGE) {
		if (sord_has_index(sord, &good[0], n_prefix, graph_search)) {
			return good[0];
		} else if (sord_has_index(sord, &good[1], n_prefix, graph_search)) {
			return good[1];
		}
	}

	if (graph_search) {
		*mode = FILTER_RANGE;
		*n_prefix = 1;
		return DEFAULT_GRAPH_ORDER;
	} else {
		*mode = FILTER_ALL;
		return DEFAULT_ORDER;
	}
}

SordModel*
sord_new(SordWorld* world, unsigned indices, bool graphs)
{
	SordModel* sord = (SordModel*)malloc(sizeof(struct SordModelImpl));
	sord->world   = world;
	sord->n_quads = 0;

	for (unsigned i = 0; i < (NUM_ORDERS / 2); ++i) {
		const int* const ordering   = orderings[i];
		const int* const g_ordering = orderings[i + (NUM_ORDERS / 2)];

		if (indices & (1 << i)) {
			sord->indices[i] = sord_zix_tree_new(
				false, sord_quad_compare, (void*)ordering);
			if (graphs) {
				sord->indices[i + (NUM_ORDERS / 2)] = sord_zix_tree_new(
					false, sord_quad_compare, (void*)g_ordering);
			} else {
				sord->indices[i + (NUM_ORDERS / 2)] = NULL;
			}
		} else {
			sord->indices[i] = NULL;
			sord->indices[i + (NUM_ORDERS / 2)] = NULL;
		}
	}

	if (!sord->indices[DEFAULT_ORDER]) {
		sord->indices[DEFAULT_ORDER] = sord_zix_tree_new(
			false, sord_quad_compare, (void*)orderings[DEFAULT_ORDER]);
	}

	return sord;
}

static void
sord_node_free_internal(SordWorld* world, SordNode* node)
{
	assert(node->refs == 0);
	if (node->node.type == SERD_LITERAL) {
		if (zix_hash_remove(world->literals, node)) {
			fprintf(stderr, "Failed to remove literal from hash.\n");
			return;
		}
		sord_node_free(world, node->datatype);
	} else {
		if (zix_hash_remove(world->names, node->node.buf)) {
			fprintf(stderr, "Failed to remove resource from hash.\n");
			return;
		}
	}
	free((uint8_t*)node->node.buf);
	free(node);
}

static void
sord_add_quad_ref(SordModel* sord, const SordNode* node, SordQuadIndex i)
{
	if (node) {
		assert(node->refs > 0);
		++((SordNode*)node)->refs;
		if (i == SORD_OBJECT) {
			++((SordNode*)node)->refs_as_obj;
		}
	}
}

static void
sord_drop_quad_ref(SordModel* sord, const SordNode* node, SordQuadIndex i)
{
	if (!node) {
		return;
	}

	assert(node->refs > 0);
	if (i == SORD_OBJECT) {
		assert(node->refs_as_obj > 0);
		--((SordNode*)node)->refs_as_obj;
	}
	if (--((SordNode*)node)->refs == 0) {
		sord_node_free_internal(sord_get_world(sord), (SordNode*)node);
	}
}

void
sord_free(SordModel* sord)
{
	if (!sord)
		return;

	// Free nodes
	SordQuad tup;
	SordIter* i = sord_begin(sord);
	for (; !sord_iter_end(i); sord_iter_next(i)) {
		sord_iter_get(i, tup);
		for (int i = 0; i < TUP_LEN; ++i) {
			sord_drop_quad_ref(sord, (SordNode*)tup[i], i);
		}
	}
	sord_iter_free(i);

	// Free quads
	for (ZixTreeIter* i = sord_zix_tree_begin(sord->indices[DEFAULT_ORDER]);
	     !sord_zix_tree_iter_is_end(i);
	     i = sord_zix_tree_iter_next(i)) {
		free(sord_zix_tree_get(i));
	}

	// Free indices
	for (unsigned i = 0; i < NUM_ORDERS; ++i)
		if (sord->indices[i])
			sord_zix_tree_free(sord->indices[i]);

	free(sord);
}

SordWorld*
sord_get_world(SordModel* sord)
{
	return sord->world;
}

size_t
sord_num_quads(const SordModel* sord)
{
	return sord->n_quads;
}

size_t
sord_num_nodes(const SordWorld* world)
{
	return world->n_nodes;
}

SordIter*
sord_begin(const SordModel* sord)
{
	if (sord_num_quads(sord) == 0) {
		return NULL;
	} else {
		ZixTreeIter* cur = sord_zix_tree_begin(sord->indices[DEFAULT_ORDER]);
		SordQuad pat = { 0, 0, 0, 0 };
		return sord_iter_new(sord, cur, pat, DEFAULT_ORDER, ALL, 0);
	}
}

static inline ZixTreeIter*
index_search(ZixTree* db, const SordQuad search_key)
{
	ZixTreeIter* iter = NULL;
	sord_zix_tree_find(db, (void*)search_key, &iter);
	return iter;
}

static inline ZixTreeIter*
index_lower_bound(ZixTree* db, const SordQuad search_key)
{
	ZixTreeIter* iter = NULL;
	sord_zix_tree_find(db, (void*)search_key, &iter);
	if (!iter) {
		return NULL;
	}

	ZixTreeIter* prev = NULL;
	while ((prev = sord_zix_tree_iter_prev(iter))) {
		if (!prev) {
			return iter;
		}

		const SordNode** const key = (const SordNode**)sord_zix_tree_get(prev);
		if (!sord_quad_match_inline(key, search_key)) {
			return iter;
		}

		iter = prev;
	}

	return iter;
}

SordIter*
sord_find(SordModel* sord, const SordQuad pat)
{
	if (!pat[0] && !pat[1] && !pat[2] && !pat[3])
		return sord_begin(sord);

	SearchMode      mode;
	int             prefix_len;
	const SordOrder index_order = sord_best_index(sord, pat, &mode, &prefix_len);

	SORD_FIND_LOG("Find " TUP_FMT "  index=%s  mode=%d  prefix_len=%d ordering=%d%d%d%d\n",
	              TUP_FMT_ARGS(pat), order_names[index_order], mode, prefix_len,
	              ordering[0], ordering[1], ordering[2], ordering[3]);

	if (pat[0] && pat[1] && pat[2] && pat[3])
		mode = SINGLE;  // No duplicate quads (Sord is a set)

	ZixTree* const     db  = sord->indices[index_order];
	ZixTreeIter* const cur = index_lower_bound(db, pat);
	if (sord_zix_tree_iter_is_end(cur)) {
		SORD_FIND_LOG("No match found\n");
		return NULL;
	}
	const SordNode** const key = (const SordNode**)sord_zix_tree_get(cur);
	if (!key || ( (mode == RANGE || mode == SINGLE)
	              && !sord_quad_match_inline(pat, key) )) {
		SORD_FIND_LOG("No match found\n");
		return NULL;
	}

	return sord_iter_new(sord, cur, pat, index_order, mode, prefix_len);
}

bool
sord_contains(SordModel* sord, const SordQuad pat)
{
	SordIter* iter = sord_find(sord, pat);
	bool      ret  = (iter != NULL);
	sord_iter_free(iter);
	return ret;
}

static SordNode*
sord_lookup_name(SordWorld* world, const uint8_t* str)
{
	return zix_hash_find(world->names, str);
}

char*
sord_strndup(const char* str, size_t len)
{
	char* dup = malloc(len + 1);
	memcpy(dup, str, len + 1);
	return dup;
}

static SordNode*
sord_new_node(SerdType type, const uint8_t* data,
              size_t n_bytes, size_t n_chars, SerdNodeFlags flags,
              SordNode* datatype, const char* lang)
{
	SordNode* node = malloc(sizeof(struct SordNodeImpl));
	node->lang         = lang;
	node->datatype     = datatype;
	node->refs         = 1;
	node->refs_as_obj  = 0;
	node->node.buf     = (uint8_t*)sord_strndup((const char*)data, n_bytes);
	node->node.n_bytes = n_bytes;
	node->node.n_chars = n_chars;
	node->node.flags   = flags;
	node->node.type    = type;

	return node;
}

const char*
sord_intern_lang(SordWorld* world, const char* lang)
{
	if (lang) {
		char* ilang = zix_hash_find(world->langs, lang);
		if (!ilang) {
			ilang = sord_strndup(lang, strlen(lang));
			zix_hash_insert(world->langs, ilang, ilang);
		}
		lang = ilang;
	}
	return lang;
}

static SordNode*
sord_lookup_literal(SordWorld* world, SordNode* type,
                    const uint8_t* str, size_t n_bytes, size_t n_chars,
                    const char*    lang)
{
	struct SordNodeImpl key;
	key.lang         = lang;
	key.datatype     = type;
	key.refs         = 1;
	key.refs_as_obj  = 1;
	key.node.buf     = (uint8_t*)str;
	key.node.n_bytes = n_bytes;
	key.node.n_chars = n_chars;
	key.node.flags   = 0;
	key.node.type    = SERD_LITERAL;

	return zix_hash_find(world->literals, &key);
}

SordNodeType
sord_node_get_type(const SordNode* node)
{
	switch (node->node.type) {
	case SERD_BLANK:
		return SORD_BLANK;
	case SERD_LITERAL:
		return SORD_LITERAL;
	case SERD_URI:
		return SORD_URI;
	default:
		fprintf(stderr, "sord: error: Illegal node type.\n");
		return (SordNodeType)0;
	}
}

const uint8_t*
sord_node_get_string(const SordNode* ref)
{
	return ref->node.buf;
}

const uint8_t*
sord_node_get_string_counted(const SordNode* ref, size_t* len)
{
	*len = ref->node.n_chars;
	return ref->node.buf;
}

const char*
sord_node_get_language(const SordNode* ref)
{
	return ref->lang;
}

SordNode*
sord_node_get_datatype(const SordNode* ref)
{
	return ref->datatype;
}

SerdNodeFlags
sord_node_get_flags(const SordNode* node)
{
	return node->node.flags;
}

bool
sord_node_is_inline_object(const SordNode* node)
{
	return (node->node.type == SERD_BLANK) && (node->refs_as_obj == 1);
}

static void
sord_add_node(SordWorld* world, SordNode* node)
{
	++world->n_nodes;
}

static SordNode*
sord_new_uri_counted(SordWorld* world, const uint8_t* str,
                     size_t n_bytes, size_t n_chars)
{
	SordNode* node = sord_lookup_name(world, str);
	if (node) {
		++node->refs;
		return node;
	}

	node = sord_new_node(SERD_URI, str, n_bytes, n_chars, 0, 0, 0);
	assert(!zix_hash_find(world->names, node->node.buf));
	zix_hash_insert(world->names, (char*)node->node.buf, node);
	sord_add_node(world, node);
	return node;
}

SordNode*
sord_new_uri(SordWorld* world, const uint8_t* str)
{
	const SerdNode node = serd_node_from_string(SERD_URI, str);
	return sord_new_uri_counted(world, str, node.n_bytes, node.n_chars);
}

static SordNode*
sord_new_blank_counted(SordWorld* world, const uint8_t* str,
                       size_t n_bytes, size_t n_chars)
{
	SordNode* node = sord_lookup_name(world, str);
	if (node) {
		++node->refs;
		return node;
	}

	node = sord_new_node(SERD_BLANK, str, n_bytes, n_chars, 0, 0, 0);
	zix_hash_insert(world->names, (char*)node->node.buf, node);
	sord_add_node(world, node);
	return node;
}

SordNode*
sord_new_blank(SordWorld* world, const uint8_t* str)
{
	const SerdNode node = serd_node_from_string(SERD_URI, str);
	return sord_new_blank_counted(world, str, node.n_bytes, node.n_chars);
}

static SordNode*
sord_new_literal_counted(SordWorld* world, SordNode* datatype,
                         const uint8_t* str, size_t n_bytes, size_t n_chars,
                         SerdNodeFlags flags,
                         const char* lang)
{
	lang = sord_intern_lang(world, lang);
	SordNode* node = sord_lookup_literal(world, datatype, str, n_bytes, n_chars, lang);
	if (node) {
		++node->refs;
		return node;
	}

	node = sord_new_node(SERD_LITERAL,
	                     str, n_bytes, n_chars, flags,
	                     sord_node_copy(datatype), lang);
	zix_hash_insert(world->literals, node, node);  // FIXME: correct?
	sord_add_node(world, node);
	assert(node->refs == 1);
	return node;
}

SordNode*
sord_new_literal(SordWorld* world, SordNode* datatype,
                 const uint8_t* str, const char* lang)
{
	SerdNodeFlags flags   = 0;
	size_t        n_bytes = 0;
	size_t        n_chars = serd_strlen(str, &n_bytes, &flags);
	return sord_new_literal_counted(world, datatype,
	                                str, n_bytes, n_chars, flags,
	                                lang);
}

SordNode*
sord_node_from_serd_node(SordWorld*      world,
                         SerdEnv*        env,
                         const SerdNode* sn,
                         const SerdNode* datatype,
                         const SerdNode* lang)
{
	SordNode* datatype_node = NULL;
	SordNode* ret           = NULL;
	switch (sn->type) {
	case SERD_NOTHING:
		return NULL;
	case SERD_LITERAL:
		datatype_node = sord_node_from_serd_node(world, env, datatype, NULL, NULL),
		ret = sord_new_literal_counted(
			world,
			datatype_node,
			sn->buf,
			sn->n_bytes,
			sn->n_chars,
			sn->flags,
			(const char*)lang->buf);
		sord_node_free(world, datatype_node);
		return ret;
	case SERD_URI:
		if (serd_uri_string_has_scheme(sn->buf)) {
			return sord_new_uri_counted(world,
			                            sn->buf, sn->n_bytes, sn->n_chars);
		} else {
			SerdURI base_uri;
			serd_env_get_base_uri(env, &base_uri);
			SerdURI  abs_uri;
			SerdNode abs_uri_node = serd_node_new_uri_from_node(
				sn, &base_uri, &abs_uri);
			SordNode* ret = sord_new_uri_counted(world,
			                                     abs_uri_node.buf,
			                                     abs_uri_node.n_bytes,
			                                     abs_uri_node.n_chars);
			serd_node_free(&abs_uri_node);
			return ret;
		}
	case SERD_CURIE: {
		SerdChunk uri_prefix;
		SerdChunk uri_suffix;
		if (serd_env_expand(env, sn, &uri_prefix, &uri_suffix)) {
			fprintf(stderr, "Failed to expand qname `%s'\n", sn->buf);
			return NULL;
		}
		const size_t uri_len = uri_prefix.len + uri_suffix.len;
		uint8_t      buf[uri_len + 1];
		memcpy(buf,                  uri_prefix.buf, uri_prefix.len);
		memcpy(buf + uri_prefix.len, uri_suffix.buf, uri_suffix.len);
		buf[uri_len] = '\0';
		SordNode* ret = sord_new_uri_counted(
			world, buf, uri_prefix.len + uri_suffix.len,
			uri_prefix.len + uri_suffix.len);  // FIXME: UTF-8
		return ret;
	}
	case SERD_BLANK:
		return sord_new_blank_counted(world, sn->buf, sn->n_bytes, sn->n_chars);
	}
	return NULL;
}

const SerdNode*
sord_node_to_serd_node(const SordNode* node)
{
	return node ? &node->node : &SERD_NODE_NULL;
}

void
sord_node_free(SordWorld* world, SordNode* node)
{
	if (!node) {
		return;
	}

	assert(node->refs > 0);
	if (--node->refs == 0) {
		sord_node_free_internal(world, node);
	}
}

SordNode*
sord_node_copy(const SordNode* node)
{
	SordNode* copy = (SordNode*)node;
	if (copy) {
		++copy->refs;
	}
	return copy;
}

static inline bool
sord_add_to_index(SordModel* sord, const SordNode** tup, SordOrder order)
{
	return !sord_zix_tree_insert(sord->indices[order], tup, NULL);
}

bool
sord_add(SordModel* sord, const SordQuad tup)
{
	SORD_WRITE_LOG("Add " TUP_FMT "\n", TUP_FMT_ARGS(tup));
	if (!tup[0] || !tup[1] || !tup[2]) {
		fprintf(stderr, "Attempt to add quad with NULL field.\n");
		return false;
	}

	const SordNode** quad = malloc(sizeof(SordQuad));
	memcpy(quad, tup, sizeof(SordQuad));

	for (unsigned i = 0; i < NUM_ORDERS; ++i) {
		if (sord->indices[i]) {
			if (!sord_add_to_index(sord, quad, i)) {
				assert(i == 0);  // Assuming index coherency
				free(quad);
				return false;  // Quad already stored, do nothing
			}
		}
	}

	for (SordQuadIndex i = 0; i < TUP_LEN; ++i)
		sord_add_quad_ref(sord, tup[i], i);

	++sord->n_quads;
	//assert(sord->n_quads == (size_t)sord_zix_tree_get_length(sord->indices[SPO]));
	return true;
}

void
sord_remove(SordModel* sord, const SordQuad tup)
{
	SORD_WRITE_LOG("Remove " TUP_FMT "\n", TUP_FMT_ARGS(tup));

	SordNode** quad = NULL;
	for (unsigned i = 0; i < NUM_ORDERS; ++i) {
		if (sord->indices[i]) {
			ZixTreeIter* const cur = index_search(sord->indices[i], tup);
			if (!sord_zix_tree_iter_is_end(cur)) {
				if (!quad) {
					quad = sord_zix_tree_get(cur);
				}
				sord_zix_tree_remove(sord->indices[i], cur);
			} else {
				assert(i == 0);  // Assuming index coherency
				return;  // Quad not found, do nothing
			}
		}
	}

	free(quad);

	for (SordQuadIndex i = 0; i < TUP_LEN; ++i)
		sord_drop_quad_ref(sord, tup[i], i);

	--sord->n_quads;
}
