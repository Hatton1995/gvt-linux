/*
 * Copyright (C) 2014 Red Hat
 * Copyright (C) 2014 Intel Corp.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 * Rob Clark <robdclark@gmail.com>
 * Daniel Vetter <daniel.vetter@ffwll.ch>
 */

#ifndef DRM_ATOMIC_H_
#define DRM_ATOMIC_H_

#include <drm/drm_crtc.h>

/**
 * struct drm_crtc_commit - track modeset commits on a CRTC
 *
 * This structure is used to track pending modeset changes and atomic commit on
 * a per-CRTC basis. Since updating the list should never block this structure
 * is reference counted to allow waiters to safely wait on an event to complete,
 * without holding any locks.
 *
 * It has 3 different events in total to allow a fine-grained synchronization
 * between outstanding updates::
 *
 *	atomic commit thread			hardware
 *
 * 	write new state into hardware	---->	...
 * 	signal hw_done
 * 						switch to new state on next
 * 	...					v/hblank
 *
 *	wait for buffers to show up		...
 *
 *	...					send completion irq
 *						irq handler signals flip_done
 *	cleanup old buffers
 *
 * 	signal cleanup_done
 *
 * 	wait for flip_done		<----
 * 	clean up atomic state
 *
 * The important bit to know is that cleanup_done is the terminal event, but the
 * ordering between flip_done and hw_done is entirely up to the specific driver
 * and modeset state change.
 *
 * For an implementation of how to use this look at
 * drm_atomic_helper_setup_commit() from the atomic helper library.
 */
struct drm_crtc_commit {
	/**
	 * @crtc:
	 *
	 * DRM CRTC for this commit.
	 */
	struct drm_crtc *crtc;

	/**
	 * @ref:
	 *
	 * Reference count for this structure. Needed to allow blocking on
	 * completions without the risk of the completion disappearing
	 * meanwhile.
	 */
	struct kref ref;

	/**
	 * @flip_done:
	 *
	 * Will be signaled when the hardware has flipped to the new set of
	 * buffers. Signals at the same time as when the drm event for this
	 * commit is sent to userspace, or when an out-fence is singalled. Note
	 * that for most hardware, in most cases this happens after @hw_done is
	 * signalled.
	 */
	struct completion flip_done;

	/**
	 * @hw_done:
	 *
	 * Will be signalled when all hw register changes for this commit have
	 * been written out. Especially when disabling a pipe this can be much
	 * later than than @flip_done, since that can signal already when the
	 * screen goes black, whereas to fully shut down a pipe more register
	 * I/O is required.
	 *
	 * Note that this does not need to include separately reference-counted
	 * resources like backing storage buffer pinning, or runtime pm
	 * management.
	 */
	struct completion hw_done;

	/**
	 * @cleanup_done:
	 *
	 * Will be signalled after old buffers have been cleaned up by calling
	 * drm_atomic_helper_cleanup_planes(). Since this can only happen after
	 * a vblank wait completed it might be a bit later. This completion is
	 * useful to throttle updates and avoid hardware updates getting ahead
	 * of the buffer cleanup too much.
	 */
	struct completion cleanup_done;

	/**
	 * @commit_entry:
	 *
	 * Entry on the per-CRTC &drm_crtc.commit_list. Protected by
	 * $drm_crtc.commit_lock.
	 */
	struct list_head commit_entry;

	/**
	 * @event:
	 *
	 * &drm_pending_vblank_event pointer to clean up private events.
	 */
	struct drm_pending_vblank_event *event;
};

struct __drm_planes_state {
	struct drm_plane *ptr;
	struct drm_plane_state *state, *old_state, *new_state;
};

struct __drm_crtcs_state {
	struct drm_crtc *ptr;
	struct drm_crtc_state *state, *old_state, *new_state;
	struct drm_crtc_commit *commit;
	s32 __user *out_fence_ptr;
	unsigned last_vblank_count;
};

struct __drm_connnectors_state {
	struct drm_connector *ptr;
	struct drm_connector_state *state, *old_state, *new_state;
};

/**
 * struct drm_atomic_state - the global state object for atomic updates
 * @ref: count of all references to this state (will not be freed until zero)
 * @dev: parent DRM device
 * @allow_modeset: allow full modeset
 * @legacy_cursor_update: hint to enforce legacy cursor IOCTL semantics
 * @legacy_set_config: Disable conflicting encoders instead of failing with -EINVAL.
 * @planes: pointer to array of structures with per-plane data
 * @crtcs: pointer to array of CRTC pointers
 * @num_connector: size of the @connectors and @connector_states arrays
 * @connectors: pointer to array of structures with per-connector data
 * @acquire_ctx: acquire context for this atomic modeset state update
 */
struct drm_atomic_state {
	struct kref ref;

	struct drm_device *dev;
	bool allow_modeset : 1;
	bool legacy_cursor_update : 1;
	bool legacy_set_config : 1;
	struct __drm_planes_state *planes;
	struct __drm_crtcs_state *crtcs;
	int num_connector;
	struct __drm_connnectors_state *connectors;

	struct drm_modeset_acquire_ctx *acquire_ctx;

	/**
	 * @commit_work:
	 *
	 * Work item which can be used by the driver or helpers to execute the
	 * commit without blocking.
	 */
	struct work_struct commit_work;
};

void __drm_crtc_commit_free(struct kref *kref);

/**
 * drm_crtc_commit_get - acquire a reference to the CRTC commit
 * @commit: CRTC commit
 *
 * Increases the reference of @commit.
 */
static inline void drm_crtc_commit_get(struct drm_crtc_commit *commit)
{
	kref_get(&commit->ref);
}

/**
 * drm_crtc_commit_put - release a reference to the CRTC commmit
 * @commit: CRTC commit
 *
 * This releases a reference to @commit which is freed after removing the
 * final reference. No locking required and callable from any context.
 */
static inline void drm_crtc_commit_put(struct drm_crtc_commit *commit)
{
	kref_put(&commit->ref, __drm_crtc_commit_free);
}

struct drm_atomic_state * __must_check
drm_atomic_state_alloc(struct drm_device *dev);
void drm_atomic_state_clear(struct drm_atomic_state *state);

/**
 * drm_atomic_state_get - acquire a reference to the atomic state
 * @state: The atomic state
 *
 * Returns a new reference to the @state
 */
static inline struct drm_atomic_state *
drm_atomic_state_get(struct drm_atomic_state *state)
{
	kref_get(&state->ref);
	return state;
}

void __drm_atomic_state_free(struct kref *ref);

/**
 * drm_atomic_state_put - release a reference to the atomic state
 * @state: The atomic state
 *
 * This releases a reference to @state which is freed after removing the
 * final reference. No locking required and callable from any context.
 */
static inline void drm_atomic_state_put(struct drm_atomic_state *state)
{
	kref_put(&state->ref, __drm_atomic_state_free);
}

int  __must_check
drm_atomic_state_init(struct drm_device *dev, struct drm_atomic_state *state);
void drm_atomic_state_default_clear(struct drm_atomic_state *state);
void drm_atomic_state_default_release(struct drm_atomic_state *state);

struct drm_crtc_state * __must_check
drm_atomic_get_crtc_state(struct drm_atomic_state *state,
			  struct drm_crtc *crtc);
int drm_atomic_crtc_set_property(struct drm_crtc *crtc,
		struct drm_crtc_state *state, struct drm_property *property,
		uint64_t val);
struct drm_plane_state * __must_check
drm_atomic_get_plane_state(struct drm_atomic_state *state,
			   struct drm_plane *plane);
int drm_atomic_plane_set_property(struct drm_plane *plane,
		struct drm_plane_state *state, struct drm_property *property,
		uint64_t val);
struct drm_connector_state * __must_check
drm_atomic_get_connector_state(struct drm_atomic_state *state,
			       struct drm_connector *connector);
int drm_atomic_connector_set_property(struct drm_connector *connector,
		struct drm_connector_state *state, struct drm_property *property,
		uint64_t val);

/**
 * drm_atomic_get_existing_crtc_state - get crtc state, if it exists
 * @state: global atomic state object
 * @crtc: crtc to grab
 *
 * This function returns the crtc state for the given crtc, or NULL
 * if the crtc is not part of the global atomic state.
 *
 * This function is deprecated, @drm_atomic_get_old_crtc_state or
 * @drm_atomic_get_new_crtc_state should be used instead.
 */
static inline struct drm_crtc_state *
drm_atomic_get_existing_crtc_state(struct drm_atomic_state *state,
				   struct drm_crtc *crtc)
{
	return state->crtcs[drm_crtc_index(crtc)].state;
}

/**
 * drm_atomic_get_old_crtc_state - get old crtc state, if it exists
 * @state: global atomic state object
 * @crtc: crtc to grab
 *
 * This function returns the old crtc state for the given crtc, or
 * NULL if the crtc is not part of the global atomic state.
 */
static inline struct drm_crtc_state *
drm_atomic_get_old_crtc_state(struct drm_atomic_state *state,
			      struct drm_crtc *crtc)
{
	return state->crtcs[drm_crtc_index(crtc)].old_state;
}
/**
 * drm_atomic_get_new_crtc_state - get new crtc state, if it exists
 * @state: global atomic state object
 * @crtc: crtc to grab
 *
 * This function returns the new crtc state for the given crtc, or
 * NULL if the crtc is not part of the global atomic state.
 */
static inline struct drm_crtc_state *
drm_atomic_get_new_crtc_state(struct drm_atomic_state *state,
			      struct drm_crtc *crtc)
{
	return state->crtcs[drm_crtc_index(crtc)].new_state;
}

/**
 * drm_atomic_get_existing_plane_state - get plane state, if it exists
 * @state: global atomic state object
 * @plane: plane to grab
 *
 * This function returns the plane state for the given plane, or NULL
 * if the plane is not part of the global atomic state.
 *
 * This function is deprecated, @drm_atomic_get_old_plane_state or
 * @drm_atomic_get_new_plane_state should be used instead.
 */
static inline struct drm_plane_state *
drm_atomic_get_existing_plane_state(struct drm_atomic_state *state,
				    struct drm_plane *plane)
{
	return state->planes[drm_plane_index(plane)].state;
}

/**
 * drm_atomic_get_old_plane_state - get plane state, if it exists
 * @state: global atomic state object
 * @plane: plane to grab
 *
 * This function returns the old plane state for the given plane, or
 * NULL if the plane is not part of the global atomic state.
 */
static inline struct drm_plane_state *
drm_atomic_get_old_plane_state(struct drm_atomic_state *state,
			       struct drm_plane *plane)
{
	return state->planes[drm_plane_index(plane)].old_state;
}

/**
 * drm_atomic_get_new_plane_state - get plane state, if it exists
 * @state: global atomic state object
 * @plane: plane to grab
 *
 * This function returns the new plane state for the given plane, or
 * NULL if the plane is not part of the global atomic state.
 */
static inline struct drm_plane_state *
drm_atomic_get_new_plane_state(struct drm_atomic_state *state,
			       struct drm_plane *plane)
{
	return state->planes[drm_plane_index(plane)].new_state;
}

/**
 * drm_atomic_get_existing_connector_state - get connector state, if it exists
 * @state: global atomic state object
 * @connector: connector to grab
 *
 * This function returns the connector state for the given connector,
 * or NULL if the connector is not part of the global atomic state.
 *
 * This function is deprecated, @drm_atomic_get_old_connector_state or
 * @drm_atomic_get_new_connector_state should be used instead.
 */
static inline struct drm_connector_state *
drm_atomic_get_existing_connector_state(struct drm_atomic_state *state,
					struct drm_connector *connector)
{
	int index = drm_connector_index(connector);

	if (index >= state->num_connector)
		return NULL;

	return state->connectors[index].state;
}

/**
 * drm_atomic_get_old_connector_state - get connector state, if it exists
 * @state: global atomic state object
 * @connector: connector to grab
 *
 * This function returns the old connector state for the given connector,
 * or NULL if the connector is not part of the global atomic state.
 */
static inline struct drm_connector_state *
drm_atomic_get_old_connector_state(struct drm_atomic_state *state,
				   struct drm_connector *connector)
{
	int index = drm_connector_index(connector);

	if (index >= state->num_connector)
		return NULL;

	return state->connectors[index].old_state;
}

/**
 * drm_atomic_get_new_connector_state - get connector state, if it exists
 * @state: global atomic state object
 * @connector: connector to grab
 *
 * This function returns the new connector state for the given connector,
 * or NULL if the connector is not part of the global atomic state.
 */
static inline struct drm_connector_state *
drm_atomic_get_new_connector_state(struct drm_atomic_state *state,
				   struct drm_connector *connector)
{
	int index = drm_connector_index(connector);

	if (index >= state->num_connector)
		return NULL;

	return state->connectors[index].new_state;
}

/**
 * __drm_atomic_get_current_plane_state - get current plane state
 * @state: global atomic state object
 * @plane: plane to grab
 *
 * This function returns the plane state for the given plane, either from
 * @state, or if the plane isn't part of the atomic state update, from @plane.
 * This is useful in atomic check callbacks, when drivers need to peek at, but
 * not change, state of other planes, since it avoids threading an error code
 * back up the call chain.
 *
 * WARNING:
 *
 * Note that this function is in general unsafe since it doesn't check for the
 * required locking for access state structures. Drivers must ensure that it is
 * safe to access the returned state structure through other means. One common
 * example is when planes are fixed to a single CRTC, and the driver knows that
 * the CRTC lock is held already. In that case holding the CRTC lock gives a
 * read-lock on all planes connected to that CRTC. But if planes can be
 * reassigned things get more tricky. In that case it's better to use
 * drm_atomic_get_plane_state and wire up full error handling.
 *
 * Returns:
 *
 * Read-only pointer to the current plane state.
 */
static inline const struct drm_plane_state *
__drm_atomic_get_current_plane_state(struct drm_atomic_state *state,
				     struct drm_plane *plane)
{
	if (state->planes[drm_plane_index(plane)].state)
		return state->planes[drm_plane_index(plane)].state;

	return plane->state;
}

int __must_check
drm_atomic_set_mode_for_crtc(struct drm_crtc_state *state,
			     struct drm_display_mode *mode);
int __must_check
drm_atomic_set_mode_prop_for_crtc(struct drm_crtc_state *state,
				  struct drm_property_blob *blob);
int __must_check
drm_atomic_set_crtc_for_plane(struct drm_plane_state *plane_state,
			      struct drm_crtc *crtc);
void drm_atomic_set_fb_for_plane(struct drm_plane_state *plane_state,
				 struct drm_framebuffer *fb);
void drm_atomic_set_fence_for_plane(struct drm_plane_state *plane_state,
				    struct dma_fence *fence);
int __must_check
drm_atomic_set_crtc_for_connector(struct drm_connector_state *conn_state,
				  struct drm_crtc *crtc);
int __must_check
drm_atomic_add_affected_connectors(struct drm_atomic_state *state,
				   struct drm_crtc *crtc);
int __must_check
drm_atomic_add_affected_planes(struct drm_atomic_state *state,
			       struct drm_crtc *crtc);

void drm_atomic_legacy_backoff(struct drm_atomic_state *state);

void
drm_atomic_clean_old_fb(struct drm_device *dev, unsigned plane_mask, int ret);

int __must_check drm_atomic_check_only(struct drm_atomic_state *state);
int __must_check drm_atomic_commit(struct drm_atomic_state *state);
int __must_check drm_atomic_nonblocking_commit(struct drm_atomic_state *state);

void drm_state_dump(struct drm_device *dev, struct drm_printer *p);

/**
 * for_each_connector_in_state - iterate over all connectors in an atomic update
 * @__state: &struct drm_atomic_state pointer
 * @connector: &struct drm_connector iteration cursor
 * @connector_state: &struct drm_connector_state iteration cursor
 * @__i: int iteration cursor, for macro-internal use
 *
 * This iterates over all connectors in an atomic update. Note that before the
 * software state is committed (by calling drm_atomic_helper_swap_state(), this
 * points to the new state, while afterwards it points to the old state. Due to
 * this tricky confusion this macro is deprecated.
 *
 * FIXME:
 *
 * Replace all usage of this with one of the explicit iterators below and then
 * remove this macro.
 */
#define for_each_connector_in_state(__state, connector, connector_state, __i) \
	for ((__i) = 0;							\
	     (__i) < (__state)->num_connector &&				\
	     ((connector) = (__state)->connectors[__i].ptr,			\
	     (connector_state) = (__state)->connectors[__i].state, 1); 	\
	     (__i)++)							\
		for_each_if (connector)

/**
 * for_each_oldnew_connector_in_state - iterate over all connectors in an atomic update
 * @__state: &struct drm_atomic_state pointer
 * @connector: &struct drm_connector iteration cursor
 * @old_connector_state: &struct drm_connector_state iteration cursor for the
 * 	old state
 * @new_connector_state: &struct drm_connector_state iteration cursor for the
 * 	new state
 * @__i: int iteration cursor, for macro-internal use
 *
 * This iterates over all connectors in an atomic update, tracking both old and
 * new state. This is useful in places where the state delta needs to be
 * considered, for example in atomic check functions.
 */
#define for_each_oldnew_connector_in_state(__state, connector, old_connector_state, new_connector_state, __i) \
	for ((__i) = 0;								\
	     (__i) < (__state)->num_connector &&				\
	     ((connector) = (__state)->connectors[__i].ptr,			\
	     (old_connector_state) = (__state)->connectors[__i].old_state,	\
	     (new_connector_state) = (__state)->connectors[__i].new_state, 1); 	\
	     (__i)++)							\
		for_each_if (connector)

/**
 * for_each_old_connector_in_state - iterate over all connectors in an atomic update
 * @__state: &struct drm_atomic_state pointer
 * @connector: &struct drm_connector iteration cursor
 * @old_connector_state: &struct drm_connector_state iteration cursor for the
 * 	old state
 * @__i: int iteration cursor, for macro-internal use
 *
 * This iterates over all connectors in an atomic update, tracking only the old
 * state. This is useful in disable functions, where we need the old state the
 * hardware is still in.
 */
#define for_each_old_connector_in_state(__state, connector, old_connector_state, __i) \
	for ((__i) = 0;								\
	     (__i) < (__state)->num_connector &&				\
	     ((connector) = (__state)->connectors[__i].ptr,			\
	     (old_connector_state) = (__state)->connectors[__i].old_state, 1); 	\
	     (__i)++)							\
		for_each_if (connector)

/**
 * for_each_new_connector_in_state - iterate over all connectors in an atomic update
 * @__state: &struct drm_atomic_state pointer
 * @connector: &struct drm_connector iteration cursor
 * @new_connector_state: &struct drm_connector_state iteration cursor for the
 * 	new state
 * @__i: int iteration cursor, for macro-internal use
 *
 * This iterates over all connectors in an atomic update, tracking only the new
 * state. This is useful in enable functions, where we need the new state the
 * hardware should be in when the atomic commit operation has completed.
 */
#define for_each_new_connector_in_state(__state, connector, new_connector_state, __i) \
	for ((__i) = 0;								\
	     (__i) < (__state)->num_connector &&				\
	     ((connector) = (__state)->connectors[__i].ptr,			\
	     (new_connector_state) = (__state)->connectors[__i].new_state, 1); 	\
	     (__i)++)							\
		for_each_if (connector)

/**
 * for_each_crtc_in_state - iterate over all connectors in an atomic update
 * @__state: &struct drm_atomic_state pointer
 * @crtc: &struct drm_crtc iteration cursor
 * @crtc_state: &struct drm_crtc_state iteration cursor
 * @__i: int iteration cursor, for macro-internal use
 *
 * This iterates over all CRTCs in an atomic update. Note that before the
 * software state is committed (by calling drm_atomic_helper_swap_state(), this
 * points to the new state, while afterwards it points to the old state. Due to
 * this tricky confusion this macro is deprecated.
 *
 * FIXME:
 *
 * Replace all usage of this with one of the explicit iterators below and then
 * remove this macro.
 */
#define for_each_crtc_in_state(__state, crtc, crtc_state, __i)	\
	for ((__i) = 0;						\
	     (__i) < (__state)->dev->mode_config.num_crtc &&	\
	     ((crtc) = (__state)->crtcs[__i].ptr,			\
	     (crtc_state) = (__state)->crtcs[__i].state, 1);	\
	     (__i)++)						\
		for_each_if (crtc_state)

/**
 * for_each_oldnew_crtc_in_state - iterate over all CRTCs in an atomic update
 * @__state: &struct drm_atomic_state pointer
 * @crtc: &struct drm_crtc iteration cursor
 * @old_crtc_state: &struct drm_crtc_state iteration cursor for the old state
 * @new_crtc_state: &struct drm_crtc_state iteration cursor for the new state
 * @__i: int iteration cursor, for macro-internal use
 *
 * This iterates over all CRTCs in an atomic update, tracking both old and
 * new state. This is useful in places where the state delta needs to be
 * considered, for example in atomic check functions.
 */
#define for_each_oldnew_crtc_in_state(__state, crtc, old_crtc_state, new_crtc_state, __i) \
	for ((__i) = 0;							\
	     (__i) < (__state)->dev->mode_config.num_crtc &&		\
	     ((crtc) = (__state)->crtcs[__i].ptr,			\
	     (old_crtc_state) = (__state)->crtcs[__i].old_state,	\
	     (new_crtc_state) = (__state)->crtcs[__i].new_state, 1);	\
	     (__i)++)							\
		for_each_if (crtc)

/**
 * for_each_old_crtc_in_state - iterate over all CRTCs in an atomic update
 * @__state: &struct drm_atomic_state pointer
 * @crtc: &struct drm_crtc iteration cursor
 * @old_crtc_state: &struct drm_crtc_state iteration cursor for the old state
 * @__i: int iteration cursor, for macro-internal use
 *
 * This iterates over all CRTCs in an atomic update, tracking only the old
 * state. This is useful in disable functions, where we need the old state the
 * hardware is still in.
 */
#define for_each_old_crtc_in_state(__state, crtc, old_crtc_state, __i)	\
	for ((__i) = 0;							\
	     (__i) < (__state)->dev->mode_config.num_crtc &&		\
	     ((crtc) = (__state)->crtcs[__i].ptr,			\
	     (old_crtc_state) = (__state)->crtcs[__i].old_state, 1);	\
	     (__i)++)							\
		for_each_if (crtc)

/**
 * for_each_new_crtc_in_state - iterate over all CRTCs in an atomic update
 * @__state: &struct drm_atomic_state pointer
 * @crtc: &struct drm_crtc iteration cursor
 * @new_crtc_state: &struct drm_crtc_state iteration cursor for the new state
 * @__i: int iteration cursor, for macro-internal use
 *
 * This iterates over all CRTCs in an atomic update, tracking only the new
 * state. This is useful in enable functions, where we need the new state the
 * hardware should be in when the atomic commit operation has completed.
 */
#define for_each_new_crtc_in_state(__state, crtc, new_crtc_state, __i)	\
	for ((__i) = 0;							\
	     (__i) < (__state)->dev->mode_config.num_crtc &&		\
	     ((crtc) = (__state)->crtcs[__i].ptr,			\
	     (new_crtc_state) = (__state)->crtcs[__i].new_state, 1);	\
	     (__i)++)							\
		for_each_if (crtc)

/**
 * for_each_plane_in_state - iterate over all planes in an atomic update
 * @__state: &struct drm_atomic_state pointer
 * @plane: &struct drm_plane iteration cursor
 * @plane_state: &struct drm_plane_state iteration cursor
 * @__i: int iteration cursor, for macro-internal use
 *
 * This iterates over all planes in an atomic update. Note that before the
 * software state is committed (by calling drm_atomic_helper_swap_state(), this
 * points to the new state, while afterwards it points to the old state. Due to
 * this tricky confusion this macro is deprecated.
 *
 * FIXME:
 *
 * Replace all usage of this with one of the explicit iterators below and then
 * remove this macro.
 */
#define for_each_plane_in_state(__state, plane, plane_state, __i)		\
	for ((__i) = 0;							\
	     (__i) < (__state)->dev->mode_config.num_total_plane &&	\
	     ((plane) = (__state)->planes[__i].ptr,				\
	     (plane_state) = (__state)->planes[__i].state, 1);		\
	     (__i)++)							\
		for_each_if (plane_state)

/**
 * for_each_oldnew_plane_in_state - iterate over all planes in an atomic update
 * @__state: &struct drm_atomic_state pointer
 * @plane: &struct drm_plane iteration cursor
 * @old_plane_state: &struct drm_plane_state iteration cursor for the old state
 * @new_plane_state: &struct drm_plane_state iteration cursor for the new state
 * @__i: int iteration cursor, for macro-internal use
 *
 * This iterates over all planes in an atomic update, tracking both old and
 * new state. This is useful in places where the state delta needs to be
 * considered, for example in atomic check functions.
 */
#define for_each_oldnew_plane_in_state(__state, plane, old_plane_state, new_plane_state, __i) \
	for ((__i) = 0;							\
	     (__i) < (__state)->dev->mode_config.num_total_plane &&	\
	     ((plane) = (__state)->planes[__i].ptr,			\
	     (old_plane_state) = (__state)->planes[__i].old_state,	\
	     (new_plane_state) = (__state)->planes[__i].new_state, 1);	\
	     (__i)++)							\
		for_each_if (plane)

/**
 * for_each_old_plane_in_state - iterate over all planes in an atomic update
 * @__state: &struct drm_atomic_state pointer
 * @plane: &struct drm_plane iteration cursor
 * @old_plane_state: &struct drm_plane_state iteration cursor for the old state
 * @__i: int iteration cursor, for macro-internal use
 *
 * This iterates over all planes in an atomic update, tracking only the old
 * state. This is useful in disable functions, where we need the old state the
 * hardware is still in.
 */
#define for_each_old_plane_in_state(__state, plane, old_plane_state, __i) \
	for ((__i) = 0;							\
	     (__i) < (__state)->dev->mode_config.num_total_plane &&	\
	     ((plane) = (__state)->planes[__i].ptr,			\
	     (old_plane_state) = (__state)->planes[__i].old_state, 1);	\
	     (__i)++)							\
		for_each_if (plane)

/**
 * for_each_new_plane_in_state - iterate over all planes in an atomic update
 * @__state: &struct drm_atomic_state pointer
 * @plane: &struct drm_plane iteration cursor
 * @new_plane_state: &struct drm_plane_state iteration cursor for the new state
 * @__i: int iteration cursor, for macro-internal use
 *
 * This iterates over all planes in an atomic update, tracking only the new
 * state. This is useful in enable functions, where we need the new state the
 * hardware should be in when the atomic commit operation has completed.
 */
#define for_each_new_plane_in_state(__state, plane, new_plane_state, __i) \
	for ((__i) = 0;							\
	     (__i) < (__state)->dev->mode_config.num_total_plane &&	\
	     ((plane) = (__state)->planes[__i].ptr,			\
	     (new_plane_state) = (__state)->planes[__i].new_state, 1);	\
	     (__i)++)							\
		for_each_if (plane)

/**
 * drm_atomic_crtc_needs_modeset - compute combined modeset need
 * @state: &drm_crtc_state for the CRTC
 *
 * To give drivers flexibility &struct drm_crtc_state has 3 booleans to track
 * whether the state CRTC changed enough to need a full modeset cycle:
 * planes_changed, mode_changed and active_changed. This helper simply
 * combines these three to compute the overall need for a modeset for @state.
 *
 * The atomic helper code sets these booleans, but drivers can and should
 * change them appropriately to accurately represent whether a modeset is
 * really needed. In general, drivers should avoid full modesets whenever
 * possible.
 *
 * For example if the CRTC mode has changed, and the hardware is able to enact
 * the requested mode change without going through a full modeset, the driver
 * should clear mode_changed in its &drm_mode_config_funcs.atomic_check
 * implementation.
 */
static inline bool
drm_atomic_crtc_needs_modeset(const struct drm_crtc_state *state)
{
	return state->mode_changed || state->active_changed ||
	       state->connectors_changed;
}

#endif /* DRM_ATOMIC_H_ */
