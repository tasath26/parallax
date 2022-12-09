#include "parallax_callbacks.h"
#include "../btree/btree.h"

struct parallax_callbacks {
	struct parallax_callback_funcs parallax_callback_functions;
	void *context;
};

void parallax_init_callbacks(par_handle dbhandle, struct parallax_callback_funcs *parallax_callbacks, void *context)
{
	struct db_handle *handle = (struct db_handle *)dbhandle;
	struct parallax_callbacks *parallax_cb = (struct parallax_callbacks *)handle->db_desc->parallax_callbacks;
	parallax_cb = (struct parallax_callbacks *)calloc(1, sizeof(struct parallax_callbacks));

	// set the callbacks
	if (parallax_callbacks->segment_is_full_cb)
		parallax_cb->parallax_callback_functions.segment_is_full_cb = parallax_callbacks->segment_is_full_cb;

	if (parallax_callbacks->compaction_started_cb)
		parallax_cb->parallax_callback_functions.compaction_started_cb =
			parallax_callbacks->compaction_started_cb;

	if (parallax_callbacks->compaction_ended_cb)
		parallax_cb->parallax_callback_functions.compaction_ended_cb = parallax_callbacks->compaction_ended_cb;

	if (parallax_callbacks->swap_levels_cb)
		parallax_cb->parallax_callback_functions.swap_levels_cb = parallax_callbacks->swap_levels_cb;

	if (parallax_callbacks->comp_write_cursor_flush_segment_cb)
		parallax_cb->parallax_callback_functions.comp_write_cursor_flush_segment_cb =
			parallax_callbacks->comp_write_cursor_flush_segment_cb;

	// set context
	parallax_cb->context = context;
}

struct parallax_callback_funcs parallax_get_callbacks(parallax_callbacks_t parallax_cb)
{
	struct parallax_callbacks *parallax_obj = (struct parallax_callbacks *)parallax_cb;
	return parallax_obj->parallax_callback_functions;
}

void *parallax_get_context(parallax_callbacks_t parallax_cb)
{
	struct parallax_callbacks *parallax_obj = (struct parallax_callbacks *)parallax_cb;
	return parallax_obj->context;
}

int8_t is_parallax_set(parallax_callbacks_t parallax_cb)
{
	struct parallax_callbacks *parallax_obj = (struct parallax_callbacks *)parallax_cb;
	if (parallax_obj)
		return 1;
	return 0;
}