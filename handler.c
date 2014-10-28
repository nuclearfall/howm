#include <stdbool.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <X11/keysym.h>

#include "handler.h"
#include "command.h"
#include "client.h"
#include "op.h"
#include "howm.h"
#include "helper.h"
#include "workspace.h"
#include "xcb_help.h"
#include "layout.h"

int cur_cnt = 1;

void (*handler[XCB_NO_OPERATION])(xcb_generic_event_t *) = {
	[XCB_BUTTON_PRESS] = button_press_event,
	[XCB_KEY_PRESS] = key_press_event,
	[XCB_MAP_REQUEST] = map_event,
	[XCB_DESTROY_NOTIFY] = destroy_event,
	[XCB_ENTER_NOTIFY] = enter_event,
	[XCB_CONFIGURE_NOTIFY] = configure_event,
	[XCB_UNMAP_NOTIFY] = unmap_event,
	[XCB_CLIENT_MESSAGE] = client_message_event
};

/**
 * @brief Process a button press.
 *
 * @param ev The button press event.
 */
void button_press_event(xcb_generic_event_t *ev)
{
	/* FIXME: be->event doesn't seem to match with any windows managed by howm.*/
	xcb_button_press_event_t *be = (xcb_button_press_event_t *)ev;

	log_info("Button %d pressed at (%d, %d)", be->detail, be->event_x, be->event_y);
	if (FOCUS_MOUSE_CLICK && be->detail == XCB_BUTTON_INDEX_1)
		focus_window(be->event);

	if (FOCUS_MOUSE_CLICK) {
		xcb_allow_events(dpy, XCB_ALLOW_REPLAY_POINTER, be->time);
		xcb_flush(dpy);
	}
}

/**
 * @brief Process a key press.
 *
 * This function implements an FSA that determines which command to run, as
 * well as with what targets and how many times.
 *
 * An keyboard input of the form qc (Assuming the correct mod keys have been
 * pressed) will lead to one client being killed- howm assumes no count means
 * perform the operation once. This is the behaviour that vim uses.
 *
 * Only counts as high as 9 are acceptable- I feel that any higher would just
 * be pointless.
 *
 * @param ev A keypress event.
 */
void key_press_event(xcb_generic_event_t *ev)
{
	unsigned int i = 0;
	xcb_keysym_t keysym;
	xcb_key_press_event_t *ke = (xcb_key_press_event_t *)ev;

	log_info("Keypress with code: %d mod: %d", ke->detail, ke->state);
	keysym = keycode_to_keysym(ke->detail);
	switch (cur_state) {
	case OPERATOR_STATE:
		for (i = 0; i < LENGTH(operators); i++) {
			if (keysym == operators[i].sym && EQUALMODS(operators[i].mod, ke->state)
			    && operators[i].mode == cur_mode) {
				operator_func = operators[i].func;
				cur_state = COUNT_STATE;
				break;
			}
		}
		break;
	case COUNT_STATE:
		if (EQUALMODS(COUNT_MOD, ke->state) && XK_1 <= keysym
				&& keysym <= XK_9) {
			/* Get a value between 1 and 9 inclusive.  */
			cur_cnt = keysym - XK_0;
			cur_state = MOTION_STATE;
			break;
		}
	case MOTION_STATE:
		for (i = 0; i < LENGTH(motions); i++) {
			if (keysym == motions[i].sym && EQUALMODS(motions[i].mod, ke->state)) {
				operator_func(motions[i].type, cur_cnt);
				save_last_ocm(operator_func, motions[i].type, cur_cnt);
				cur_state = OPERATOR_STATE;
				/* Reset so that qc is equivalent to q1c. */
				cur_cnt = 1;
			}
		}
	}
	for (i = 0; i < LENGTH(keys); i++)
		if (keysym == keys[i].sym && EQUALMODS(keys[i].mod, ke->state)
		    && keys[i].func && keys[i].mode == cur_mode) {
			keys[i].func(&keys[i].arg);
			if (keys[i].func != replay)
				save_last_cmd(keys[i].func, &keys[i].arg);
		}
}

/**
 * @brief Handles mapping requests.
 *
 * When an X window wishes to be displayed, it send a mapping request. This
 * function processes that mapping request and inserts the new client (created
 * from the map requesting window) into the list of clients for the current
 * workspace.
 *
 * @param ev A mapping request event.
 */
void map_event(xcb_generic_event_t *ev)
{
	xcb_window_t transient = 0;
	xcb_get_geometry_reply_t *geom;
	xcb_get_window_attributes_reply_t *wa;
	xcb_map_request_event_t *me = (xcb_map_request_event_t *)ev;
	xcb_ewmh_get_atoms_reply_t type;
	unsigned int i;
	Client *c;

	wa = xcb_get_window_attributes_reply(dpy, xcb_get_window_attributes(dpy, me->window), NULL);
	if (!wa || wa->override_redirect || find_client_by_win(me->window)) {
		free(wa);
		return;
	}
	free(wa);

	log_info("Mapping request for window <0x%x>", me->window);

	c = create_client(me->window);

	if (xcb_ewmh_get_wm_window_type_reply(ewmh,
				xcb_ewmh_get_wm_window_type(ewmh, me->window),
				&type, NULL) == 1) {
		for (i = 0; i < type.atoms_len; i++) {
			xcb_atom_t a = type.atoms[i];

			if (a == ewmh->_NET_WM_WINDOW_TYPE_DOCK
				|| a == ewmh->_NET_WM_WINDOW_TYPE_TOOLBAR) {
				return;
			} else if (a == ewmh->_NET_WM_WINDOW_TYPE_NOTIFICATION
				|| a == ewmh->_NET_WM_WINDOW_TYPE_DROPDOWN_MENU
				|| a == ewmh->_NET_WM_WINDOW_TYPE_SPLASH
				|| a == ewmh->_NET_WM_WINDOW_TYPE_POPUP_MENU
				|| a == ewmh->_NET_WM_WINDOW_TYPE_TOOLTIP
				|| a == ewmh->_NET_WM_WINDOW_TYPE_DIALOG) {
				c->is_floating = true;
			}
		}
	}


	/* Assume that transient windows MUST float. */
	xcb_icccm_get_wm_transient_for_reply(dpy, xcb_icccm_get_wm_transient_for_unchecked(dpy, me->window), &transient, NULL);
	c->is_transient = transient ? true : false;
	if (c->is_transient)
		c->is_floating = true;

	geom = xcb_get_geometry_reply(dpy, xcb_get_geometry_unchecked(dpy, me->window), NULL);
	if (geom) {
		log_info("Mapped client's initial geom is %ux%u+%d+%d", geom->width, geom->height, geom->x, geom->y);
		if (c->is_floating) {
			c->w = geom->width > 1 ? geom->width : FLOAT_SPAWN_WIDTH;
			c->h = geom->height > 1 ? geom->height : FLOAT_SPAWN_HEIGHT;
			c->x = CENTER_FLOATING ? (screen_width / 2) - (c->w / 2) : geom->x;
			c->y = CENTER_FLOATING ? (screen_height - wss[cw].bar_height - c->h) / 2 : geom->y;
		}
		free(geom);
	}

	apply_rules(c);
	arrange_windows();
	xcb_map_window(dpy, c->win);
	update_focused_client(c);
	grab_buttons(c);
}

/**
 * @brief The handler for destroy events.
 *
 * Used when a window sends a destroy event, signalling that it wants to be
 * unmapped. The client that the window belongs to is then removed from the
 * client list for its repective workspace.
 *
 * @param ev The destroy event.
 */
void destroy_event(xcb_generic_event_t *ev)
{
	xcb_destroy_notify_event_t *de = (xcb_destroy_notify_event_t *)ev;
	Client *c = find_client_by_win(de->window);

	if (!c)
		return;
	log_info("Client <%p> wants to be destroyed", c);
	remove_client(c, true);
	arrange_windows();
}

/**
 * @brief The event that occurs when the mouse pointer enters a window.
 *
 * @param ev The enter event.
 */
void enter_event(xcb_generic_event_t *ev)
{
	xcb_enter_notify_event_t *ee = (xcb_enter_notify_event_t *)ev;

	log_debug("Enter event for window <0x%x>", ee->event);
	if (FOCUS_MOUSE && wss[cw].layout != ZOOM)
		focus_window(ee->event);
}

/**
 * @brief Deal with a window's request to change its geometry.
 *
 * @param ev The event sent from the window.
 */
void configure_event(xcb_generic_event_t *ev)
{
	xcb_configure_request_event_t *ce = (xcb_configure_request_event_t *)ev;
	uint32_t vals[7] = {0}, i = 0;

	log_info("Received configure request for window <0x%x>", ce->window);

	/* TODO: Need to test whether gaps etc need to be taken into account
	 * here. */
	if (XCB_CONFIG_WINDOW_X & ce->value_mask)
		vals[i++] = ce->x;
	if (XCB_CONFIG_WINDOW_Y & ce->value_mask)
		vals[i++] = ce->y + (BAR_BOTTOM ? 0 : wss[cw].bar_height);
	if (XCB_CONFIG_WINDOW_WIDTH & ce->value_mask)
		vals[i++] = (ce->width < screen_width - BORDER_PX) ? ce->width : screen_width - BORDER_PX;
	if (XCB_CONFIG_WINDOW_HEIGHT & ce->value_mask)
		vals[i++] = (ce->height < screen_height - BORDER_PX) ? ce->height : screen_height - BORDER_PX;
	if (XCB_CONFIG_WINDOW_BORDER_WIDTH & ce->value_mask)
		vals[i++] = ce->border_width;
	if (XCB_CONFIG_WINDOW_SIBLING & ce->value_mask)
		vals[i++] = ce->sibling;
	if (XCB_CONFIG_WINDOW_STACK_MODE & ce->value_mask)
		vals[i++] = ce->stack_mode;
	xcb_configure_window(dpy, ce->window, ce->value_mask, vals);
	arrange_windows();
}

/**
 * @brief Remove clients that wish to be unmapped.
 *
 * @param ev An event letting us know which client should be unmapped.
 */
void unmap_event(xcb_generic_event_t *ev)
{
	xcb_unmap_notify_event_t *ue = (xcb_unmap_notify_event_t *)ev;
	Client *c = find_client_by_win(ue->window);

	if (!c)
		return;
	log_info("Received unmap request for client <%p>", c);

	if (!ue->event == screen->root) {
		remove_client(c, true);
		arrange_windows();
	}
	howm_info();
}

/**
 * @brief Handle messages sent by the client to alter its state.
 *
 * @param ev The client message as a generic event.
 */
void client_message_event(xcb_generic_event_t *ev)
{
	xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
	Client *c = find_client_by_win(cm->window);

	if (c && cm->type == ewmh->_NET_WM_STATE) {
		ewmh_process_wm_state(c, (xcb_atom_t) cm->data.data32[1], cm->data.data32[0]);
		if (cm->data.data32[2])
			ewmh_process_wm_state(c, (xcb_atom_t) cm->data.data32[2], cm->data.data32[0]);
	} else if (c && cm->type == ewmh->_NET_CLOSE_WINDOW) {
		log_info("_NET_CLOSE_WINDOW: Removing client <%p>", c);
		remove_client(c, true);
		arrange_windows();
	} else if (c && cm->type == ewmh->_NET_ACTIVE_WINDOW) {
		log_info("_NET_ACTIVE_WINDOW: Focusing client <%p>", c);
		update_focused_client(find_client_by_win(cm->window));
	} else if (c && cm->type == ewmh->_NET_CURRENT_DESKTOP
			&& cm->data.data32[0] < WORKSPACES) {
		log_info("_NET_CURRENT_DESKTOP: Changing to workspace <%d>", cm->data.data32[0]);
		change_ws(&(Arg){ .i = cm->data.data32[0] });
	} else {
		log_debug("Unhandled client message: %d", cm->type);
	}
}

void handle_event(xcb_generic_event_t *ev)
{
	if (handler[ev->response_type & ~0x80])
		handler[ev->response_type & ~0x80](ev);
}
