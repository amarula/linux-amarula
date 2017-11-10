// SPDX-License-Identifier: GPL-2.0+
/*
 * XArray implementation
 * Copyright (c) 2017 Microsoft Corporation
 * Author: Matthew Wilcox <willy@infradead.org>
 */

#include <linux/bitmap.h>
#include <linux/export.h>
#include <linux/xarray.h>

/*
 * Coding conventions in this file:
 *
 * @xa is used to refer to the entire xarray.
 * @xas is the 'xarray operation state'.  It may be either a pointer to
 * an xa_state, or an xa_state stored on the stack.  This is an unfortunate
 * ambiguity.
 * @index is the index of the entry being operated on
 * @tag is an xa_tag_t; a small number indicating one of the tag bits.
 * @node refers to an xa_node; usually the primary one being operated on by
 * this function.
 * @offset is the index into the slots array inside an xa_node.
 * @parent refers to the @xa_node closer to the head than @node.
 * @entry refers to something stored in a slot in the xarray
 */

static inline void xa_tag_set(struct xarray *xa, xa_tag_t tag)
{
	if (!(xa->xa_flags & XA_FLAGS_TAG(tag)))
		xa->xa_flags |= XA_FLAGS_TAG(tag);
}

static inline void xa_tag_clear(struct xarray *xa, xa_tag_t tag)
{
	if (xa->xa_flags & XA_FLAGS_TAG(tag))
		xa->xa_flags &= ~(XA_FLAGS_TAG(tag));
}

static inline bool node_get_tag(const struct xa_node *node, unsigned int offset,
				xa_tag_t tag)
{
	return test_bit(offset, node->tags[(__force unsigned)tag]);
}

/* returns true if the bit was set */
static inline bool node_set_tag(struct xa_node *node, unsigned int offset,
				xa_tag_t tag)
{
	return __test_and_set_bit(offset, node->tags[(__force unsigned)tag]);
}

/* returns true if the bit was set */
static inline bool node_clear_tag(struct xa_node *node, unsigned int offset,
				xa_tag_t tag)
{
	return __test_and_clear_bit(offset, node->tags[(__force unsigned)tag]);
}

static inline bool node_any_tag(struct xa_node *node, xa_tag_t tag)
{
	return !bitmap_empty(node->tags[(__force unsigned)tag], XA_CHUNK_SIZE);
}

/* extracts the offset within this node from the index */
static unsigned int get_offset(unsigned long index, struct xa_node *node)
{
	return (index >> node->shift) & XA_CHUNK_MASK;
}

/* move the index either forwards (find) or backwards (sibling slot) */
static void xas_move_index(struct xa_state *xas, unsigned long offset)
{
	unsigned int shift = xas->xa_node->shift;
	xas->xa_index &= ~XA_CHUNK_MASK << shift;
	xas->xa_index += offset << shift;
}

static void *set_bounds(struct xa_state *xas)
{
	xas->xa_node = XAS_BOUNDS;
	return NULL;
}

/*
 * Starts a walk.  If the @xas is already valid, we assume that it's on
 * the right path and just return where we've got to.  If we're in an
 * error state, return NULL.  If the index is outside the current scope
 * of the xarray, return NULL without changing @xas->xa_node.  Otherwise
 * set @xas->xa_node to NULL and return the current head of the array.
 */
static void *xas_start(struct xa_state *xas)
{
	void *entry;

	if (xas_valid(xas))
		return xas_reload(xas);
	if (xas_error(xas))
		return NULL;

	entry = xa_head(xas->xa);
	if (!xa_is_node(entry)) {
		if (xas->xa_index)
			return set_bounds(xas);
	} else {
		if ((xas->xa_index >> xa_to_node(entry)->shift) > XA_CHUNK_MASK)
			return set_bounds(xas);
	}

	xas->xa_node = NULL;
	return entry;
}

static void *xas_descend(struct xa_state *xas, struct xa_node *node)
{
	unsigned int offset = get_offset(xas->xa_index, node);
	void *entry = xa_entry(xas->xa, node, offset);

	xas->xa_node = node;
	if (xa_is_sibling(entry)) {
		offset = xa_to_sibling(entry);
		entry = xa_entry(xas->xa, node, offset);
		xas_move_index(xas, offset);
	}

	xas->xa_offset = offset;
	return entry;
}

/**
 * xas_load() - Load an entry from the XArray (advanced).
 * @xas: XArray operation state.
 *
 * Usually walks the @xas to the appropriate state to load the entry
 * stored at xa_index.  However, it will do nothing and return %NULL if
 * @xas is in an error state.  xas_load() will never expand the tree.
 *
 * If the xa_state is set up to operate on a multi-index entry, xas_load()
 * may return %NULL or an internal entry, even if there are entries
 * present within the range specified by @xas.
 *
 * Context: Any context.  The caller should hold the xa_lock or the RCU lock.
 * Return: Usually an entry in the XArray, but see description for exceptions.
 */
void *xas_load(struct xa_state *xas)
{
	void *entry = xas_start(xas);

	while (xa_is_node(entry)) {
		struct xa_node *node = xa_to_node(entry);

		if (xas->xa_shift > node->shift)
			break;
		entry = xas_descend(xas, node);
	}
	return entry;
}
EXPORT_SYMBOL_GPL(xas_load);

/**
 * xas_get_tag() - Returns the state of this tag.
 * @xas: XArray operation state.
 * @tag: Tag number.
 *
 * Return: true if the tag is set, false if the tag is clear or @xas
 * is in an error state.
 */
bool xas_get_tag(const struct xa_state *xas, xa_tag_t tag)
{
	if (xas_invalid(xas))
		return false;
	if (!xas->xa_node)
		return xa_tagged(xas->xa, tag);
	return node_get_tag(xas->xa_node, xas->xa_offset, tag);
}
EXPORT_SYMBOL_GPL(xas_get_tag);

/**
 * xas_set_tag() - Sets the tag on this entry and its parents.
 * @xas: XArray operation state.
 * @tag: Tag number.
 *
 * Sets the specified tag on this entry, and walks up the tree setting it
 * on all the ancestor entries.  Does nothing if @xas has not been walked to
 * an entry, or is in an error state.
 */
void xas_set_tag(const struct xa_state *xas, xa_tag_t tag)
{
	struct xa_node *node = xas->xa_node;
	unsigned int offset = xas->xa_offset;

	if (xas_invalid(xas))
		return;

	while (node) {
		if (node_set_tag(node, offset, tag))
			return;
		offset = node->offset;
		node = xa_parent_locked(xas->xa, node);
	}

	if (!xa_tagged(xas->xa, tag))
		xa_tag_set(xas->xa, tag);
}
EXPORT_SYMBOL_GPL(xas_set_tag);

/**
 * xas_clear_tag() - Clears the tag on this entry and its parents.
 * @xas: XArray operation state.
 * @tag: Tag number.
 *
 * Clears the specified tag on this entry, and walks back to the head
 * attempting to clear it on all the ancestor entries.  Does nothing if
 * @xas has not been walked to an entry, or is in an error state.
 */
void xas_clear_tag(const struct xa_state *xas, xa_tag_t tag)
{
	struct xa_node *node = xas->xa_node;
	unsigned int offset = xas->xa_offset;

	if (xas_invalid(xas))
		return;

	while (node) {
		if (!node_clear_tag(node, offset, tag))
			return;
		if (node_any_tag(node, tag))
			return;

		offset = node->offset;
		node = xa_parent_locked(xas->xa, node);
	}

	if (xa_tagged(xas->xa, tag))
		xa_tag_clear(xas->xa, tag);
}
EXPORT_SYMBOL_GPL(xas_clear_tag);

/**
 * xa_init_flags() - Initialise an empty XArray with flags.
 * @xa: XArray.
 * @flags: XA_FLAG values.
 *
 * If you need to initialise an XArray with special flags (eg you need
 * to take the lock from interrupt context), use this function instead
 * of xa_init().
 *
 * Context: Any context.
 */
void xa_init_flags(struct xarray *xa, gfp_t flags)
{
	spin_lock_init(&xa->xa_lock);
	xa->xa_flags = flags;
	xa->xa_head = NULL;
}
EXPORT_SYMBOL(xa_init_flags);

/**
 * xa_load() - Load an entry from an XArray.
 * @xa: XArray.
 * @index: index into array.
 *
 * Context: Any context.  Takes and releases the RCU lock.
 * Return: The entry at @index in @xa.
 */
void *xa_load(struct xarray *xa, unsigned long index)
{
	XA_STATE(xas, xa, index);
	void *entry;

	rcu_read_lock();
	do {
		entry = xas_load(&xas);
	} while (xas_retry(&xas, entry));
	rcu_read_unlock();

	return entry;
}
EXPORT_SYMBOL(xa_load);

/**
 * __xa_set_tag() - Set this tag on this entry while locked.
 * @xa: XArray.
 * @index: Index of entry.
 * @tag: Tag number.
 *
 * Attempting to set a tag on a NULL entry does not succeed.
 *
 * Context: Any context.  Expects xa_lock to be held on entry.
 */
void __xa_set_tag(struct xarray *xa, unsigned long index, xa_tag_t tag)
{
	XA_STATE(xas, xa, index);
	void *entry = xas_load(&xas);

	if (entry)
		xas_set_tag(&xas, tag);
}
EXPORT_SYMBOL_GPL(__xa_set_tag);

/**
 * __xa_clear_tag() - Clear this tag on this entry while locked.
 * @xa: XArray.
 * @index: Index of entry.
 * @tag: Tag number.
 *
 * Context: Any context.  Expects xa_lock to be held on entry.
 */
void __xa_clear_tag(struct xarray *xa, unsigned long index, xa_tag_t tag)
{
	XA_STATE(xas, xa, index);
	void *entry = xas_load(&xas);

	if (entry)
		xas_clear_tag(&xas, tag);
}
EXPORT_SYMBOL_GPL(__xa_clear_tag);

/**
 * xa_get_tag() - Inquire whether this tag is set on this entry.
 * @xa: XArray.
 * @index: Index of entry.
 * @tag: Tag number.
 *
 * This function uses the RCU read lock, so the result may be out of date
 * by the time it returns.  If you need the result to be stable, use a lock.
 *
 * Context: Any context.  Takes and releases the RCU lock.
 * Return: True if the entry at @index has this tag set, false if it doesn't.
 */
bool xa_get_tag(struct xarray *xa, unsigned long index, xa_tag_t tag)
{
	XA_STATE(xas, xa, index);
	void *entry;

	rcu_read_lock();
	entry = xas_start(&xas);
	while (xas_get_tag(&xas, tag)) {
		if (!xa_is_node(entry))
			goto found;
		entry = xas_descend(&xas, xa_to_node(entry));
	}
	rcu_read_unlock();
	return false;
 found:
	rcu_read_unlock();
	return true;
}
EXPORT_SYMBOL(xa_get_tag);

/**
 * xa_set_tag() - Set this tag on this entry.
 * @xa: XArray.
 * @index: Index of entry.
 * @tag: Tag number.
 *
 * Attempting to set a tag on a NULL entry does not succeed.
 *
 * Context: Process context.  Takes and releases the xa_lock.
 */
void xa_set_tag(struct xarray *xa, unsigned long index, xa_tag_t tag)
{
	xa_lock(xa);
	__xa_set_tag(xa, index, tag);
	xa_unlock(xa);
}
EXPORT_SYMBOL(xa_set_tag);

/**
 * xa_clear_tag() - Clear this tag on this entry.
 * @xa: XArray.
 * @index: Index of entry.
 * @tag: Tag number.
 *
 * Clearing a tag always succeeds.
 *
 * Context: Process context.  Takes and releases the xa_lock.
 */
void xa_clear_tag(struct xarray *xa, unsigned long index, xa_tag_t tag)
{
	xa_lock(xa);
	__xa_clear_tag(xa, index, tag);
	xa_unlock(xa);
}
EXPORT_SYMBOL(xa_clear_tag);

#ifdef XA_DEBUG
void xa_dump_node(const struct xa_node *node)
{
	unsigned i, j;

	if (!node)
		return;
	if ((unsigned long)node & 3) {
		pr_cont("node %px\n", node);
		return;
	}

	pr_cont("node %px %s %d parent %px shift %d count %d values %d "
		"array %px list %px %px tags",
		node, node->parent ? "offset" : "max", node->offset,
		node->parent, node->shift, node->count, node->nr_values,
		node->array, node->private_list.prev, node->private_list.next);
	for (i = 0; i < XA_MAX_TAGS; i++)
		for (j = 0; j < XA_TAG_LONGS; j++)
			pr_cont(" %lx", node->tags[i][j]);
	pr_cont("\n");
}

void xa_dump_index(unsigned long index, unsigned int shift)
{
	if (!shift)
		pr_info("%lu: ", index);
	else if (shift >= BITS_PER_LONG)
		pr_info("0-%lu: ", ~0UL);
	else
		pr_info("%lu-%lu: ", index, index | ((1UL << shift) - 1));
}

void xa_dump_entry(const void *entry, unsigned long index, unsigned long shift)
{
	if (!entry)
		return;

	xa_dump_index(index, shift);

	if (xa_is_node(entry)) {
		if (shift == 0) {
			pr_cont("%px\n", entry);
		} else {
			unsigned long i;
			struct xa_node *node = xa_to_node(entry);
			xa_dump_node(node);
			for (i = 0; i < XA_CHUNK_SIZE; i++)
				xa_dump_entry(node->slots[i],
				      index + (i << node->shift), node->shift);
		}
	} else if (xa_is_value(entry))
		pr_cont("value %ld (0x%lx) [%px]\n", xa_to_value(entry),
						xa_to_value(entry), entry);
	else if (!xa_is_internal(entry))
		pr_cont("%px\n", entry);
	else if (xa_is_retry(entry))
		pr_cont("retry (%ld)\n", xa_to_internal(entry));
	else if (xa_is_sibling(entry))
		pr_cont("sibling (slot %ld)\n", xa_to_sibling(entry));
	else
		pr_cont("UNKNOWN ENTRY (%px)\n", entry);
}

void xa_dump(const struct xarray *xa)
{
	void *entry = xa->xa_head;
	unsigned int shift = 0;

	pr_info("xarray: %px head %px flags %x tags %d %d %d\n", xa, entry,
			xa->xa_flags, xa_tagged(xa, XA_TAG_0),
			xa_tagged(xa, XA_TAG_1), xa_tagged(xa, XA_TAG_2));
	if (xa_is_node(entry))
		shift = xa_to_node(entry)->shift + XA_CHUNK_SHIFT;
	xa_dump_entry(entry, 0, shift);
}
#endif
