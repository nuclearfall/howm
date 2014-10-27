#include <err.h>
#include <errno.h>
#include <sys/select.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <X11/keysym.h>
#include <X11/X.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_ewmh.h>

/**
 * @file howm.c
 *
 * @author Harvey Hunt
 *
 * @date 2014
 *
 * @brief howm
 */

/*
 *┌────────────┐
 *│╻ ╻┏━┓╻ ╻┏┳┓│
 *│┣━┫┃ ┃┃╻┃┃┃┃│
 *│╹ ╹┗━┛┗┻┛╹ ╹│
 *└────────────┘
*/

/** Calculates a mask that can be applied to a window in order to reconfigure a
 * window. */
#define MOVE_RESIZE_MASK (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | \
			  XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT)
/** Ensures that the number lock doesn't intefere with checking the equality
 * of two modifier masks.*/
#define CLEANMASK(mask) (mask & ~(numlockmask | XCB_MOD_MASK_LOCK))
/** Wraps up the comparison of modifier masks into a neat package. */
#define EQUALMODS(mask, omask) (CLEANMASK(mask) == CLEANMASK(omask))
/** Calculates the length of an array. */
#define LENGTH(x) (unsigned int)(sizeof(x) / sizeof(*x))
/** Checks to see if a client is floating, fullscreen or transient. */
#define FFT(c) (c->is_transient || c->is_floating || c->is_fullscreen)
/** Supresses the unused variable compiler warnings. */
#define UNUSED(x) (void)(x)
/** Determine which file descriptor is the largest and add one to it. */
#define MAX_FD(x, y) ((x) > (y) ? (x + 1) : (y + 1))

/** The remove action for a WM_STATE request. */
#define _NET_WM_STATE_REMOVE 0
/** The add action for a WM_STATE request. */
#define _NET_WM_STATE_ADD 1
/** The toggle action for a WM_STATE request. */
#define _NET_WM_STATE_TOGGLE 2

/**
 * @brief Represents an argument.
 *
 * Used to hold data that is sent as a parameter to a function when called as a
 * result of a keypress.
 */
typedef union {
	const char * const * const cmd; /**< Represents a command that will be called by a shell.  */
	int i; /**< Usually used for specifying workspaces or clients. */
} Arg;

/**
 * @brief Represents a key.
 *
 * Holds information relative to a key, such as keysym and the mode during
 * which the keypress can be seen as valid.
 */
typedef struct {
	int mod; /**< The mask of the modifiers pressed. */
	unsigned int mode; /**< The mode within which this keypress is valid. */
	xcb_keysym_t sym;  /**< The keysym of the pressed key. */
	void (*func)(const Arg *); /**< The function to be called when this key is pressed. */
	const Arg arg; /**< The argument passed to the above function. */
} Key;

/**
 * @brief Represents a rule that is applied to a client upon it starting.
 */
typedef struct {
	const char *class; /**<	The class or name of the client. */
	int ws; /**<  The workspace that the client should be spawned
				on (0 means current workspace). */
	bool follow; /**< If the client is spawned on another ws, shall we follow? */
	bool is_floating; /**< Spawn the client in a floating state? */
	bool is_fullscreen; /**< Spawn the client in a fullscreen state? */
} Rule;

/**
 * @brief Represents an operator.
 *
 * Operators perform an action upon one or more targets (identified by
 * motions).
 */
typedef struct {
	int mod; /**< The mask of the modifiers pressed. */
	xcb_keysym_t sym; /**< The keysym of the pressed key. */
	unsigned int mode; /**< The mode within which this keypress is valid. */
	void (*func)(const unsigned int type, const int cnt); /**< The function to be
								 * called when the key is pressed. */
} Operator;

/**
 * @brief Represents a motion.
 *
 * A motion can be used to target an operation at something specific- such as a
 * client or workspace.
 *
 * For example:
 *
 * q4c (Kill, 4, Clients).
 */
typedef struct {
	int mod; /**< The mask of the modifiers pressed. */
	xcb_keysym_t sym; /**< The keysym of the pressed key. */
	unsigned int type; /**< Represents whether the motion is for clients, WS etc. */
} Motion;

/**
 * @brief Represents a button.
 *
 * Allows the mapping of a button to a function, as is done with the Key struct
 * for keys.
 */
typedef struct {
	int mod; /**< The mask of the modifiers pressed.  */
	short int button; /**< The button that was pressed. */
	void (*func)(const Arg *); /**< The function to be called when the
					* button is pressed. */
	const Arg arg; /**< The argument passed to the above function. */
} Button;

/**
 * @brief Represents a client that is being handled by howm.
 *
 * All the attributes that are needed by howm for a client are stored here.
 */
typedef struct Client {
	struct Client *next; /**< Clients are stored in a linked list-
					* this represents the client after this one. */
	bool is_fullscreen; /**< Is the client fullscreen? */
	bool is_floating; /**< Is the client floating? */
	bool is_transient; /**< Is the client transient?
					* Defined at: http://standards.freedesktop.org/wm-spec/wm-spec-latest.html*/
	bool is_urgent; /**< This is set by a client that wants focus for some reason. */
	xcb_window_t win; /**< The window that this client represents. */
	uint16_t x; /**< The x coordinate of the client. */
	uint16_t y; /**< The y coordinate of the client. */
	uint16_t w; /**< The width of the client. */
	uint16_t h; /**< The height of the client. */
	uint16_t gap; /**< The size of the useless gap between this client and
			the others. */
} Client;

/**
 * @brief Represents a workspace, which stores clients.
 *
 * Clients are stored as a linked list. Changing to a different workspace will
 * cause different clients to be rendered on the screen.
 */
typedef struct {
	int layout; /**< The current layout of the WS, as defined in the
				* layout enum. */
	int client_cnt; /**< The amount of clients on this workspace. */
	uint16_t gap; /**< The size of the useless gap between windows for this workspace. */
	float master_ratio; /**< The ratio of the size of the master window
				 compared to the screen's size. */
	uint16_t bar_height; /**< The height of the space left for a bar. Stored
			      here so it can be toggled per ws. */
	Client *head; /**< The start of the linked list. */
	Client *prev_foc; /**< The last focused client. This is seperate to
				* the linked list structure. */
	Client *current; /**< The client that is currently in focus. */
} Workspace;

typedef struct {
	char *name; /**< The function's name. */
	void (*func)(const Arg *); /**< The function to be called when a command
				     comes in from the socket. */
	void (*operator)(const unsigned int type, const int cnt); /**< The
			operator to be called when a command comes in from
			the socket. */
	int argc; /**< The amount of args this command expects. */
	int arg_type; /**< The argument's type for commands that use the union Arg. */
} Command;

/**
 * @brief Represents the last command (and its arguments) or the last
 * combination of operator, count and motion (ocm).
 */
struct replay_state {
	void (*last_op)(const unsigned int type, int cnt); /**< The last operator to be called. */
	void (*last_cmd)(const Arg *arg); /**< The last command to be called. */
	const Arg *last_arg; /**< The last argument, passed to the last command. */
	unsigned int last_type; /**< The value determine by the last motion
				(workspace, client etc).*/
	int last_cnt; /**< The last count passed to the last operator function. */
};

/**
 * @brief Represents a stack. This stack is going to hold linked lists of
 * clients. An example of the stack is below:
 *
 * TOP
 * ==========
 * c1->c2->c3->NULL
 * ==========
 * c1->NULL
 * ==========
 * c1->c2->c3->NULL
 * ==========
 * BOTTOM
 *
 */
struct stack {
	int size; /**< The amount of items in the stack. */
	Client **contents; /**< The contents is an array of linked lists. Storage
			is malloced later as we don't know the size yet.*/
};


/* Operators */
static void op_kill(const unsigned int type, int cnt);
static void op_move_up(const unsigned int type, int cnt);
static void op_move_down(const unsigned int type, int cnt);
static void op_focus_down(const unsigned int type, int cnt);
static void op_focus_up(const unsigned int type, int cnt);
static void op_shrink_gaps(const unsigned int type, int cnt);
static void op_grow_gaps(const unsigned int type, int cnt);
static void op_cut(const unsigned int type, int cnt);

/* Clients */
static void teleport_client(const Arg *arg);
static void change_client_gaps(Client *c, int size);
static void change_gaps(const unsigned int type, int cnt, int size);
static void move_current_down(const Arg *arg);
static void move_current_up(const Arg *arg);
static void kill_client(const int ws, bool arrange);
static void move_down(Client *c);
static void move_up(Client *c);
static Client *next_client(Client *c);
static void focus_next_client(const Arg *arg);
static void focus_prev_client(const Arg *arg);
static void update_focused_client(Client *c);
static Client *prev_client(Client *c, int ws);
static Client *create_client(xcb_window_t w);
static void remove_client(Client *c, bool refocus);
static Client *find_client_by_win(xcb_window_t w);
static void client_to_ws(Client *c, const int ws, bool follow);
static void current_to_ws(const Arg *arg);
static void draw_clients(void);
static void change_client_geom(Client *c, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
static void toggle_float(const Arg *arg);
static void resize_float_width(const Arg *arg);
static void resize_float_height(const Arg *arg);
static void move_float_y(const Arg *arg);
static void move_float_x(const Arg *arg);
static void make_master(const Arg *arg);
static void grab_buttons(Client *c);
static void set_fullscreen(Client *c, bool fscr);
static void set_urgent(Client *c, bool urg);
static void toggle_fullscreen(const Arg *arg);
static void focus_urgent(const Arg *arg);
static void send_to_scratchpad(const Arg *arg);
static void get_from_scratchpad(const Arg *arg);

/* Workspaces */
static void kill_ws(const int ws);
static void toggle_bar(const Arg *arg);
static void resize_master(const Arg *arg);
static void focus_next_ws(const Arg *arg);
static void focus_prev_ws(const Arg *arg);
static void focus_last_ws(const Arg *arg);
static void change_ws(const Arg *arg);
static int correct_ws(int ws);

/* Layouts */
static void change_layout(const Arg *arg);
static void next_layout(const Arg *arg);
static void previous_layout(const Arg *arg);
static void last_layout(const Arg *arg);
static void stack(void);
static void grid(void);
static void zoom(void);
static void arrange_windows(void);

/* Modes */
static void change_mode(const Arg *arg);

/* Stack */
static void stack_push(struct stack *s, Client *c);
static Client *stack_pop(struct stack *s);
static void stack_init(struct stack *s);
static void stack_free(struct stack *s);

/* Events */
static void enter_event(xcb_generic_event_t *ev);
static void destroy_event(xcb_generic_event_t *ev);
static void button_press_event(xcb_generic_event_t *ev);
static void key_press_event(xcb_generic_event_t *ev);
static void map_event(xcb_generic_event_t *ev);
static void configure_event(xcb_generic_event_t *ev);
static void unmap_event(xcb_generic_event_t *ev);
static void client_message_event(xcb_generic_event_t *ev);

/* XCB */
static void grab_keys(void);
static xcb_keycode_t *keysym_to_keycode(xcb_keysym_t sym);
static void grab_keycode(xcb_keycode_t *keycode, const int mod);
static void elevate_window(xcb_window_t win);
static void move_resize(xcb_window_t win, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
static void set_border_width(xcb_window_t win, uint16_t w);
static void get_atoms(char **names, xcb_atom_t *atoms);
static void check_other_wm(void);
static xcb_keysym_t keycode_to_keysym(xcb_keycode_t keycode);
static void ewmh_process_wm_state(Client *c, xcb_atom_t a, int action);

/* Misc */
static int ipc_init(void);
static int ipc_process_cmd(char *msg, int len);
static char **ipc_process_args(char *msg, int len, int *err);
static int ipc_arg_to_int(char *arg, int *err);
static void apply_rules(Client *c);
static void howm_info(void);
static void save_last_ocm(void (*op) (const unsigned int, int), const unsigned int type, int cnt);
static void save_last_cmd(void (*cmd)(const Arg *), const Arg *arg);
static void replay(const Arg *arg);
static void paste(const Arg *arg);
static int get_non_tff_count(void);
static Client *get_first_non_tff(void);
static uint32_t get_colour(char *colour);
static void spawn(const Arg *arg);
static void setup(void);
static void move_client(int cnt, bool up);
static void focus_window(xcb_window_t win);
static void quit_howm(const Arg *arg);
static void restart_howm(const Arg *arg);
static void cleanup(void);
static void delete_win(xcb_window_t win);
static void setup_ewmh(void);

enum layouts { ZOOM, GRID, HSTACK, VSTACK, END_LAYOUT };
enum states { OPERATOR_STATE, COUNT_STATE, MOTION_STATE, END_STATE };
enum modes { NORMAL, FOCUS, FLOATING, END_MODES };
enum motions { CLIENT, WORKSPACE };
enum net_atom_enum { NET_WM_STATE_FULLSCREEN, NET_SUPPORTED, NET_WM_STATE,
	NET_ACTIVE_WINDOW };
enum wm_atom_enum { WM_DELETE_WINDOW, WM_PROTOCOLS };
enum teleport_locations { TOP_LEFT, TOP_CENTER, TOP_RIGHT, CENTER, BOTTOM_LEFT, BOTTOM_CENTER, BOTTOM_RIGHT };
enum ipc_errs { IPC_ERR_NONE, IPC_ERR_SYNTAX, IPC_ERR_ALLOC, IPC_ERR_NO_CMD, IPC_ERR_TOO_MANY_ARGS,
	IPC_ERR_TOO_FEW_ARGS, IPC_ERR_ARG_NOT_INT, IPC_ERR_ARG_TOO_LARGE };
enum arg_types {TYPE_IGNORE, TYPE_INT, TYPE_CMD};

/* Handlers */
static void(*handler[XCB_NO_OPERATION]) (xcb_generic_event_t *) = {
	[XCB_BUTTON_PRESS] = button_press_event,
	[XCB_KEY_PRESS] = key_press_event,
	[XCB_MAP_REQUEST] = map_event,
	[XCB_DESTROY_NOTIFY] = destroy_event,
	[XCB_ENTER_NOTIFY] = enter_event,
	[XCB_CONFIGURE_NOTIFY] = configure_event,
	[XCB_UNMAP_NOTIFY] = unmap_event,
	[XCB_CLIENT_MESSAGE] = client_message_event
};

static void(*layout_handler[]) (void) = {
	[GRID] = grid,
	[ZOOM] = zoom,
	[HSTACK] = stack,
	[VSTACK] = stack
};

#include "config.h"

static Command commands[] = {
	{"resize_master", resize_master, NULL, 1, TYPE_INT},
	{"change_layout", change_layout, NULL, 1, TYPE_INT},
	{"next_layout", next_layout, NULL, 0, TYPE_INT},
	{"previous_layout", previous_layout, NULL, 0, TYPE_INT},
	{"last_layout", last_layout, NULL, 0, TYPE_INT},
	{"change_mode", change_mode, NULL, 1, TYPE_INT},
	{"toggle_float", toggle_float, NULL, 0, TYPE_INT},
	{"toggle_fullscreen", toggle_fullscreen, NULL, 0, TYPE_INT},
	{"quit_howm", quit_howm, NULL, 1, TYPE_INT},
	{"restart_howm", restart_howm, NULL, 1, TYPE_INT},
	{"resize_master", resize_master, NULL, 1, TYPE_INT},
	{"toggle_bar", toggle_bar, NULL, 0, TYPE_INT},
	{"replay", replay, NULL, 0, TYPE_INT},
	{"paste", paste, NULL, 0, TYPE_INT},
	{"send_to_scratchpad", send_to_scratchpad, NULL, 0, TYPE_INT},
	{"get_from_scratchpad", get_from_scratchpad, NULL, 0, TYPE_INT},
	{"resize_float_height", resize_float_height, NULL, 1, TYPE_INT},
	{"resize_float_width", resize_float_width, NULL, 1, TYPE_INT},
	{"move_float_x", move_float_x, NULL, 1, TYPE_INT},
	{"move_float_y", move_float_y, NULL, 1, TYPE_INT},
	{"teleport_client", teleport_client, NULL, 1, TYPE_INT},
	{"focus_urgent", focus_urgent, NULL, 0, TYPE_INT},
	{"focus_prev_client", focus_prev_client, NULL, 0, TYPE_INT},
	{"focus_next_client", focus_next_client, NULL, 0, TYPE_INT},
	{"move_current_up", move_current_up, NULL, 0, TYPE_INT},
	{"move_current_down", move_current_down, NULL, 0, TYPE_INT},
	{"focus_last_ws", focus_last_ws, NULL, 0, TYPE_INT},
	{"focus_next_ws", focus_next_ws, NULL, 0, TYPE_INT},
	{"focus_prev_ws", focus_prev_ws, NULL, 0, TYPE_INT},
	{"make_master", make_master, NULL, 0, TYPE_INT},
	{"change_ws", change_ws, NULL, 1, TYPE_INT},
	{"current_to_ws", current_to_ws, NULL, 1, TYPE_INT},
	{"spawn", spawn, NULL, 1, TYPE_CMD},

	{"op_kill", NULL, op_kill, 2, TYPE_IGNORE},
	{"op_move_up", NULL, op_move_up, 2, TYPE_IGNORE},
	{"op_move_down", NULL, op_move_down, 2, TYPE_IGNORE},
	{"op_shrink_gaps", NULL, op_shrink_gaps, 2, TYPE_IGNORE},
	{"op_grow_gaps", NULL, op_grow_gaps, 2, TYPE_IGNORE},
	{"op_cut", NULL, op_cut, 2, TYPE_IGNORE},
	{"op_focus_down", NULL, op_focus_down, 2, TYPE_IGNORE},
	{"op_focus_up", NULL, op_focus_up, 2, TYPE_IGNORE}
};

static void (*operator_func)(const unsigned int type, int cnt);

static Client *scratchpad;
static struct stack del_reg;
static xcb_connection_t *dpy;
static char *WM_ATOM_NAMES[] = { "WM_DELETE_WINDOW", "WM_PROTOCOLS" };
static xcb_atom_t wm_atoms[LENGTH(WM_ATOM_NAMES)];
static xcb_screen_t *screen;
static xcb_ewmh_connection_t *ewmh;
static int numlockmask, retval, last_ws, prev_layout, cw = DEFAULT_WORKSPACE;
static uint32_t border_focus, border_unfocus, border_prev_focus, border_urgent;
static unsigned int cur_mode, cur_state = OPERATOR_STATE, cur_cnt = 1;
static uint16_t screen_height, screen_width;
static bool running = true, restart;

static struct replay_state rep_state;

/* Add comments so that splint ignores this as it doesn't support variadic
 * macros.
 */
/*@ignore@*/
#ifdef DEBUG_ENABLE
/** Output debugging information using puts. */
#       define DEBUG(x) puts(x)
/** Output debugging information using printf to allow for formatting. */
#	define DEBUGP(M, ...) fprintf(stderr, "[DBG] %s:%d: " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#       define DEBUG(x) do {} while (0)
#       define DEBUGP(x, ...) do {} while (0)
#endif

#define LOG_DEBUG 1
#define LOG_INFO 2
#define LOG_WARN 3
#define LOG_ERR 4
#define LOG_NONE 5

#if LOG_LEVEL == LOG_DEBUG
#define log_debug(M, ...) fprintf(stderr, "[DEBUG] (%s:%d) " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define log_debug(x, ...) do {} while (0)
#endif


#if LOG_LEVEL <= LOG_INFO
#define log_info(M, ...) fprintf(stderr, "[INFO] (%s:%d) " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define log_info(x, ...) do {} while (0)
#endif

#if LOG_LEVEL <= LOG_WARN
#define log_warn(M, ...) fprintf(stderr, "[WARN] (%s:%d) " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define log_warn(x, ...) do {} while (0)
#endif

#if LOG_LEVEL <= LOG_ERR
#define log_err(M, ...) fprintf(stderr, "[ERROR] (%s:%d) " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define log_err(x, ...) do {} while (0)
#endif

/*@end@*/

/**
 * @brief Occurs when howm first starts.
 *
 * A connection to the X11 server is attempted and keys are then grabbed.
 *
 * Atoms are gathered.
 */
void setup(void)
{
	screen = xcb_setup_roots_iterator(xcb_get_setup(dpy)).data;
	if (!screen)
		log_err("Can't acquire the default screen.");
	screen_height = screen->height_in_pixels;
	screen_width = screen->width_in_pixels;

	log_info("Screen's height is: %d", screen_height);
	log_info("Screen's width is: %d", screen_width);

	grab_keys();

	get_atoms(WM_ATOM_NAMES, wm_atoms);

	setup_ewmh();

	border_focus = get_colour(BORDER_FOCUS);
	border_unfocus = get_colour(BORDER_UNFOCUS);
	border_prev_focus = get_colour(BORDER_PREV_FOCUS);
	border_urgent = get_colour(BORDER_URGENT);
	stack_init(&del_reg);

	howm_info();
}

/**
 * @brief Converts a hexcode colour into an X11 colourmap pixel.
 *
 * @param colour A string of the format "#RRGGBB", that will be interpreted as
 * a colour code.
 *
 * @return An X11 colourmap pixel.
 */
uint32_t get_colour(char *colour)
{
	uint32_t pixel;
	uint16_t r, g, b;
	xcb_alloc_color_reply_t *rep;
	xcb_colormap_t map = screen->default_colormap;

	long int rgb = strtol(++colour, NULL, 16);

	r = ((rgb >> 16) & 0xFF) * 257;
	g = ((rgb >> 8) & 0xFF) * 257;
	b = (rgb & 0xFF) * 257;
	rep = xcb_alloc_color_reply(dpy, xcb_alloc_color(dpy, map,
				    r, g, b), NULL);
	if (!rep) {
		log_err("ERROR: Can't allocate the colour %s", colour);
		return 0;
	}
	pixel = rep->pixel;
	free(rep);
	return pixel;
}

/**
 * @brief The code that glues howm together...
 */
int main(int argc, char *argv[])
{
	UNUSED(argc);
	UNUSED(argv);
	fd_set descs;
	int sock_fd, dpy_fd, cmd_fd, ret;
	ssize_t n;
	xcb_generic_event_t *ev;
	char *data = calloc(IPC_BUF_SIZE, sizeof(char));

	if (!data) {
		log_err("Can't allocate memory for socket buffer.");
		exit(EXIT_FAILURE);
	}

	dpy = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(dpy)) {
		log_err("Can't open X connection");
		exit(EXIT_FAILURE);
	}
	sock_fd = ipc_init();
	setup();
	check_other_wm();
	dpy_fd = xcb_get_file_descriptor(dpy);
	while (running) {
		if (!xcb_flush(dpy))
			log_err("Failed to flush X connection");

		FD_ZERO(&descs);
		FD_SET(dpy_fd, &descs);
		FD_SET(sock_fd, &descs);

		if (select(MAX_FD(dpy_fd, sock_fd), &descs, NULL, NULL, NULL) > 0) {
			if (FD_ISSET(sock_fd, &descs)) {
				cmd_fd = accept(sock_fd, NULL, 0);
				if (cmd_fd == -1) {
					log_err("Failed to accept connection");
					continue;
				}
				n = read(cmd_fd, data, IPC_BUF_SIZE - 1);
				if (n > 0) {
					data[n] = '\0';
					ret = ipc_process_cmd(data, n);
					if (write(cmd_fd, &ret, sizeof(int)) == -1)
						log_err("Unable to send response. errno: %d", errno);
					close(cmd_fd);
				}
			}
			if (FD_ISSET(dpy_fd, &descs)) {
				while ((ev = xcb_poll_for_event(dpy)) != NULL) {
					if (ev && handler[ev->response_type & ~0x80])
						handler[ev->response_type & ~0x80](ev);
					else
						log_debug("Unimplemented event: %d", ev->response_type & ~0x80);
					free(ev);
				}
			}
			if (xcb_connection_has_error(dpy)) {
				log_err("XCB connection encountered an error.");
				running = false;
			}
		}
	}

	cleanup();
	xcb_disconnect(dpy);
	close(sock_fd);
	free(data);

	if (!running && !restart) {
		return retval;
	} else if (!running && restart) {
		char *const argv[] = {HOWM_PATH, NULL};

		execv(argv[0], argv);
		return EXIT_SUCCESS;
	}
	return EXIT_FAILURE;
}

/**
 * @brief Try to detect if another WM exists.
 *
 * If another WM exists (this can be seen by whether it has registered itself
 * with the X11 server) then howm will exit.
 */
void check_other_wm(void)
{
	xcb_generic_error_t *e;
	uint32_t values[1] = { XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
			       XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
			       XCB_EVENT_MASK_BUTTON_PRESS |
			       XCB_EVENT_MASK_KEY_PRESS |
			       XCB_EVENT_MASK_PROPERTY_CHANGE
			     };

	e = xcb_request_check(dpy, xcb_change_window_attributes_checked(dpy,
			      screen->root, XCB_CW_EVENT_MASK, values));
	if (e != NULL) {
		xcb_disconnect(dpy);
		log_err("Couldn't register as WM. Perhaps another WM is running? XCB returned error_code: %d", e->error_code);
		exit(EXIT_FAILURE);
	}
	free(e);
}

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
 * @brief Spawns a command.
 */
void spawn(const Arg *arg)
{
	if (fork())
		return;
	if (dpy)
		close(screen->root);
	setsid();
	log_info("Spawning command: %s", (char *)arg->cmd[0]);
	execvp((char *)arg->cmd[0], (char **)arg->cmd);
	log_err("execvp of command: %s failed.", (char *)arg->cmd[0]);
	exit(EXIT_FAILURE);
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
 * @brief Search workspaces for a window, returning the client that it belongs
 * to.
 *
 * @param win A valid XCB window that is used when searching all clients across
 * all desktops.
 *
 * @return The found client.
 */
Client *find_client_by_win(xcb_window_t win)
{
	bool found;
	int w = 1;
	Client *c = NULL;

	for (found = false; w <= WORKSPACES && !found; w++)
		for (c = wss[w].head; c && !(found = (win == c->win)); c = c->next)
			;
	return c;
}

/**
 * @brief Convert a keycode to a keysym.
 *
 * @param code An XCB keycode.
 *
 * @return The keysym corresponding to the given keycode.
 */
xcb_keysym_t keycode_to_keysym(xcb_keycode_t code)
{
	xcb_keysym_t sym;
	xcb_key_symbols_t *syms = xcb_key_symbols_alloc(dpy);

	if (!syms)
		return 0;
	sym = xcb_key_symbols_get_keysym(syms, code, 0);
	xcb_key_symbols_free(syms);
	return sym;
}

/**
 * @brief Convert a keysym to a keycode.
 *
 * @param sym An XCB keysym.
 *
 * @return The keycode corresponding to the given keysym.
 */
xcb_keycode_t *keysym_to_keycode(xcb_keysym_t sym)
{
	xcb_keycode_t *code;
	xcb_key_symbols_t *syms = xcb_key_symbols_alloc(dpy);

	if (!syms)
		return NULL;
	code = xcb_key_symbols_get_keycode(syms, sym);
	xcb_key_symbols_free(syms);
	return code;
}

/**
 * @brief Find the client before the given client.
 *
 * @param c The client which needs to have its previous found.
 *
 * @param ws The workspace that the client is on.
 *
 * @return The previous client, so long as the given client isn't NULL and
 * there is more than one client. Else, NULL.
 */
Client *prev_client(Client *c, int ws)
{
	Client *p = NULL;

	if (!c || !wss[ws].head || !wss[ws].head->next)
		return NULL;
	for (p = wss[ws].head; p->next && p->next != c; p = p->next)
		;
	return p;
}

/**
 * @brief Find the next client.
 *
 * Note: This function wraps around the end of the list of clients. If c is the
 * last item in the list of clients, then the head of the list is returned.
 *
 * @param c The client which needs to have its next found.
 *
 * @return The next client, if c is the last client in the list then this will
 * be head. If c is NULL or there is only one client in the client list, NULL
 * will be returned.
 */
Client *next_client(Client *c)
{
	if (!c || !wss[cw].head	|| !wss[cw].head->next)
		return NULL;
	if (c->next)
		return c->next;
	return wss[cw].head;
}

/**
 * @brief Call the appropriate layout handler for each layout.
 */
void arrange_windows(void)
{
	if (!wss[cw].head)
		return;
	log_debug("Arranging windows");
	layout_handler[wss[cw].head->next ? wss[cw].layout : ZOOM]();
	howm_info();
}

/**
 * @brief Arrange the windows into a grid layout.
 */
void grid(void)
{
	int n = get_non_tff_count();
	Client *c = NULL;
	int cols, rows, i = -1, col_cnt = 0, row_cnt = 0;
	uint16_t col_w;
	uint16_t client_y = BAR_BOTTOM ? 0 : wss[cw].bar_height;
	uint16_t col_h = screen_height - wss[cw].bar_height;

	if (n <= 1) {
		zoom();
		return;
	}

	log_info("Arranging %d clients in grid layout", n);

	for (cols = 0; cols <= n / 2; cols++)
		if (cols * cols >= n)
			break;
	rows = n / cols;
	col_w = screen_width / cols;
	for (c = wss[cw].head; c; c = c->next) {
		if (FFT(c))
			continue;
		else
			i++;

		if (cols - (n % cols) < (i / rows) + 1)
			rows = n / cols + 1;
		change_client_geom(c, col_cnt * col_w, client_y + (row_cnt * col_h / rows),
				col_w, (col_h / rows));
		if (++row_cnt >= rows) {
			row_cnt = 0;
			col_cnt++;
		}
	}
	draw_clients();
}

/**
 * @brief Have one window at a time taking up the entire screen.
 *
 * Sets the geometry of each window in order for the windows to be rendered to
 * take up the entire screen.
 */
void zoom(void)
{
	Client *c;

	log_info("Arranging clients in zoom format");
	/* When zoom is called because there aren't enough clients for other
	 * layouts to work, draw a border to be consistent with other layouts.
	 * */
	if (wss[cw].layout != ZOOM && !wss[cw].head->is_fullscreen)
		set_border_width(wss[cw].head->win, BORDER_PX);

	for (c = wss[cw].head; c; c = c->next)
		if (!FFT(c))
			change_client_geom(c, 0, BAR_BOTTOM ? 0 : wss[cw].bar_height,
					screen_width, screen_height - wss[cw].bar_height);
	draw_clients();
}

/**
 * @brief Change the dimensions and location of a window (win).
 *
 * @param win The window upon which the operations should be performed.
 * @param x The new x location of the top left corner.
 * @param y The new y location of the top left corner.
 * @param w The new width of the window.
 * @param h The new height of the window.
 */
void move_resize(xcb_window_t win,
		 uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	uint32_t position[] = { x, y, w, h };

	xcb_configure_window(dpy, win, MOVE_RESIZE_MASK, position);
}

/**
 * @brief Sets c to the active window and gives it input focus. Sorts out
 * border colours as well.
 *
 * WARNING: Do NOT use this to focus a client on another workspace. Instead,
 * set wss[ws].current to the client that you want focused.
 *
 * @param c The client that is currently in focus.
 */
void update_focused_client(Client *c)
{
	unsigned int all = 0, fullscreen = 0, float_trans = 0;

	if (!c)
		return;

	if (!wss[cw].head) {
		wss[cw].prev_foc = wss[cw].current = NULL;
		xcb_ewmh_set_active_window(ewmh, 0, XCB_NONE);
		return;
	} else if (c == wss[cw].prev_foc) {
		wss[cw].prev_foc = prev_client(wss[cw].current = wss[cw].prev_foc, cw);
	} else if (c != wss[cw].current) {
		wss[cw].prev_foc = wss[cw].current;
		wss[cw].current = c;
	}

	log_info("Focusing client <%p>", c);
	for (c = wss[cw].head; c; c = c->next, ++all) {
		if (FFT(c)) {
			fullscreen++;
			if (!c->is_fullscreen)
				float_trans++;
		}
	}
	xcb_window_t windows[all];

	windows[(wss[cw].current->is_floating || wss[cw].current->is_transient) ? 0 : float_trans] = wss[cw].current->win;
	c = wss[cw].head;
	for (fullscreen += FFT(wss[cw].current) ? 1 : 0; c; c = c->next) {
		set_border_width(c->win, c->is_fullscreen ? 0 : BORDER_PX);
		xcb_change_window_attributes(dpy, c->win, XCB_CW_BORDER_PIXEL,
					     (c == wss[cw].current ? &border_focus :
					      c == wss[cw].prev_foc ? &border_prev_focus
					      : &border_unfocus));
		if (c != wss[cw].current)
			windows[c->is_fullscreen ? --fullscreen : FFT(c) ?
				--float_trans : --all] = c->win;
	}

	for (float_trans = 0; float_trans <= all; ++float_trans)
		elevate_window(windows[all - float_trans]);

	xcb_ewmh_set_active_window(ewmh, 0, wss[cw].current->win);

	xcb_set_input_focus(dpy, XCB_INPUT_FOCUS_POINTER_ROOT, wss[cw].current->win,
			    XCB_CURRENT_TIME);
	arrange_windows();
}

/**
 * @brief Let the X11 server know which keys howm is interested in so that howm
 * can be alerted when any of them are pressed.
 *
 * All keys are ungrabbed and then each key in keys, operators and motions are
 * grabbed.
 */
void grab_keys(void)
{
	/* TODO: optimise this so that it doesn't call xcb_grab_key for all
	 * keys, as some are repeated due to modes. Perhaps XCB does this
	 * already? */
	xcb_keycode_t *keycode;
	unsigned int i;

	log_debug("Grabbing keys");
	xcb_ungrab_key(dpy, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);
	for (i = 0; i < LENGTH(keys); i++) {
		keycode = keysym_to_keycode(keys[i].sym);
		grab_keycode(keycode, keys[i].mod);
	}

	for (i = 0; i < LENGTH(operators); i++) {
		keycode = keysym_to_keycode(operators[i].sym);
		grab_keycode(keycode, operators[i].mod);
	}

	for (i = 0; i < LENGTH(motions); i++) {
		keycode = keysym_to_keycode(motions[i].sym);
		grab_keycode(keycode, motions[i].mod);
	}

	for (i = 0; i < 8; i++) {
		keycode = keysym_to_keycode(XK_1 + i);
		grab_keycode(keycode, COUNT_MOD);
	}
}

/**
 * @brief Make a client listen for button press events.
 *
 * @param c The client that needs to listen for button presses.
 */
void grab_buttons(Client *c)
{
	xcb_ungrab_button(dpy, XCB_BUTTON_INDEX_ANY, c->win, XCB_GRAB_ANY);
	xcb_grab_button(dpy, 1, c->win, XCB_EVENT_MASK_BUTTON_PRESS,
			XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
			XCB_WINDOW_NONE, XCB_CURSOR_NONE,
			XCB_BUTTON_INDEX_ANY, XCB_BUTTON_MASK_ANY);
}

/**
 * @brief Grab a keycode, therefore telling the X11 server howm wants to
 * receive events when the key is pressed.
 *
 * @param keycode The keycode to be grabbed.
 * @param mod The modifier that should be pressed down in order for an event
 * for the keypress to be sent to howm.
 */
void grab_keycode(xcb_keycode_t *keycode, const int mod)
{
	unsigned int j, k;
	uint16_t mods[] = { 0, XCB_MOD_MASK_LOCK };

	for (j = 0; keycode[j] != XCB_NO_SYMBOL; j++)
		for (k = 0; k < LENGTH(mods); k++)
			xcb_grab_key(dpy, 1, screen->root, mod |
				     mods[k], keycode[j], XCB_GRAB_MODE_ASYNC,
				     XCB_GRAB_MODE_ASYNC);
	free(keycode);
}

/**
 * @brief Sets the width of the borders around a window (win).
 *
 * @param win The window that will have its border width changed.
 * @param w The new width of the window's border.
 */
void set_border_width(xcb_window_t win, uint16_t w)
{
	uint32_t width[1] = { w };

	xcb_configure_window(dpy, win, XCB_CONFIG_WINDOW_BORDER_WIDTH, width);
}

/**
 * @brief Move a window to the front of all the other windows.
 *
 * @param win The window to be moved.
 */
void elevate_window(xcb_window_t win)
{
	uint32_t stack_mode[1] = { XCB_STACK_MODE_ABOVE };

	log_info("Moving window <0x%x> to the front", win);
	xcb_configure_window(dpy, win, XCB_CONFIG_WINDOW_STACK_MODE, stack_mode);
}

/**
 * @brief Request all of the atoms that howm supports.
 *
 * @param names The names of the atoms to be fetched.
 * @param atoms Where the returned atoms will be stored.
 */
void get_atoms(char **names, xcb_atom_t *atoms)
{
	xcb_intern_atom_reply_t *reply;
	unsigned int i;
	xcb_intern_atom_cookie_t cookies[LENGTH(names)];

	for (i = 0; i <= LENGTH(names); i++) {
		cookies[i] = xcb_intern_atom(dpy, 0, strlen(names[i]), names[i]);
		log_debug("Requesting atom %s", names[i]);
	}
	for (i = 0; i <= LENGTH(names); i++) {
		reply = xcb_intern_atom_reply(dpy, cookies[i], NULL);
		if (reply) {
			atoms[i] = reply->atom;
			log_debug("Got reply for atom %s", names[i]);
			free(reply);
		} else {
			log_warn("The atom %s has not been registered by howm.", names[i]);
		}
	}
}

/**
 * @brief Arrange the windows in a stack, whether that be horizontal or
 * vertical is decided by the current_layout.
 */
void stack(void)
{
	Client *c = get_first_non_tff();
	bool vert = (wss[cw].layout == VSTACK);
	uint16_t h = screen_height - wss[cw].bar_height;
	uint16_t w = screen_width;
	int n = get_non_tff_count();
	uint16_t client_x = 0, client_span = 0;
	uint16_t client_y = BAR_BOTTOM ? 0 : wss[cw].bar_height;
	uint16_t ms = (vert ? w : h) * wss[cw].master_ratio;
	/* The size of the direction the clients will be stacked in. e.g.
	 *
	 *+---------------------------+--------------+   +
	 *|                           |              |   |
	 *|                           |              |   |
	 *|                           |              |   |
	 *|                           |              |   |
	 *|                           |              |   | Span for vert stack
	 *|                           +--------------+   |
	 *|                           |              |   |
	 *|                           |              |   |
	 *|                           |              |   |
	 *|                           |              |   |
	 *|                           +--------------+   |
	 *|                           |              |   |
	 *|                           |              |   |
	 *|                           |              |   |
	 *|                           |              |   |
	 *+---------------------------+--------------+   v
	 */
	uint16_t span = vert ? h : w;

	if (n <= 1) {
		zoom();
		return;
	}

	/* TODO: Need to take into account when this has remainders. */
	client_span = (span / (n - 1));

	log_info("Arranging %d clients in %sstack layout", n, vert ? "v" : "h");
	if (vert) {
		change_client_geom(c, 0, client_y,
			    ms, span);
	} else {
		change_client_geom(c, 0, BAR_BOTTOM ? 0 : wss[cw].bar_height,
			span, ms);
	}

	for (c = c->next; c; c = c->next) {
		if (FFT(c))
			continue;
		if (vert) {
			change_client_geom(c, ms, client_y,
				    screen_width - ms,
				    client_span);
			client_y += client_span;
		} else {
			change_client_geom(c, client_x, ms,
				    client_span,
				    screen_height - wss[cw].bar_height - ms);
			client_x += client_span;
		}
	}
	draw_clients();
}

/**
 * @brief Count how many clients aren't Transient, Floating or Fullscreen.
 *
 * @return The amount of clients in the current workspace that aren't TFF.
 */
int get_non_tff_count(void)
{
	int n = 0;
	Client *c = NULL;

	for (c = wss[cw].head; c; c = c->next)
		if (!FFT(c))
			n++;
	return n;
}

/**
 * @brief Returns the first client that isn't transient, floating or
 * fullscreen.
 *
 * @return The first client that isn't TFF. NULL if none.
 */
static Client *get_first_non_tff(void)
{
	Client *c = NULL;

	for (c = wss[cw].head; c && FFT(c); c = c->next)
		;
	return c;
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
 * @brief Remove a client from its workspace client list.
 *
 * @param c The client to be removed.
 *
 * @param refocus Whether the clients should be rearranged and focus be
 * updated.
 */
void remove_client(Client *c, bool refocus)
{
	Client **temp = NULL;
	int w = 1;

	for (; w <= WORKSPACES; w++)
		for (temp = &wss[w].head; *temp; temp = &(*temp)->next)
			if (*temp == c)
				goto found;
	return;

found:
	*temp = c->next;
	log_info("Removing client <%p>", c);
	if (c == wss[w].prev_foc)
		wss[w].prev_foc = prev_client(wss[w].current, w);
	if (c == wss[w].current || !wss[w].head->next) {
		wss[w].current = wss[w].prev_foc ? wss[w].prev_foc : wss[w].head;
		if (refocus)
			update_focused_client(wss[w].current);
	}
	free(c);
	c = NULL;
	wss[w].client_cnt--;
}

/**
 * @brief Print debug information about the current state of howm.
 *
 * This can be parsed by programs such as scripts that will pipe their input
 * into a status bar.
 */
void howm_info(void)
{
	unsigned int w = 0;
#if DEBUG_ENABLE
	for (w = 1; w <= WORKSPACES; w++) {
		fprintf(stdout, "%u:%d:%u:%u:%u\n", cur_mode,
		       wss[w].layout, w, cur_state, wss[w].client_cnt);
	}
	fflush(stdout);
#else
	UNUSED(w);
	fprintf(stdout, "%u:%d:%u:%u:%u\n", cur_mode,
		wss[cw].layout, cw, cur_state, wss[cw].client_cnt);
	fflush(stdout);
#endif
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
 * @brief Move a client down in its client list.
 *
 * @param c The client to be moved.
 */
void move_down(Client *c)
{
	Client *prev = prev_client(c, cw);
	Client *n = (c->next) ? c->next : wss[cw].head;

	if (!c)
		return;
	if (!prev)
		return;
	if (wss[cw].head == c)
		wss[cw].head = n;
	else
		prev->next = c->next;
	c->next = (c->next) ? n->next : n;
	if (n->next == c->next)
		n->next = c;
	else
		wss[cw].head = c;
	log_info("Moved client <%p> on workspace <%d> down", c, cw);
	arrange_windows();
}

/**
 * @brief Move a client up in its client list.
 *
 * @param c The client to be moved down.
 */
void move_up(Client *c)
{
	Client *p = prev_client(c, cw);
	Client *pp = NULL;

	if (!c)
		return;
	if (!p)
		return;
	if (p->next)
		for (pp = wss[cw].head; pp && pp->next != p; pp = pp->next)
			;
	if (pp)
		pp->next = c;
	else
		wss[cw].head = (wss[cw].head == c) ? c->next : c;
	p->next = (c->next == wss[cw].head) ? c : c->next;
	c->next = (c->next == wss[cw].head) ? NULL : p;
	log_info("Moved client <%p> on workspace <%d> down", c, cw);
	arrange_windows();
}

/**
 * @brief brief Move focus onto the client next in the client list.
 *
 * @param arg The argument passed from the config file. Note: The argument goes
 * unused.
 */
void focus_next_client(const Arg *arg)
{
	UNUSED(arg);
	if (!wss[cw].current || !wss[cw].head->next)
		return;
	log_info("Focusing next client");
	update_focused_client(wss[cw].current->next ? wss[cw].current->next : wss[cw].head);
}

/**
 * @brief brief Move focus onto the client previous in the client list.
 *
 * @param arg The argument passed from the config file. Note: The argument goes
 * unused.
 */
void focus_prev_client(const Arg *arg)
{
	UNUSED(arg);
	if (!wss[cw].current || !wss[cw].head->next)
		return;
	log_info("Focusing previous client");
	wss[cw].prev_foc = wss[cw].current;
	update_focused_client(prev_client(wss[cw].prev_foc, cw));
}

/**
 * @brief Change to a different workspace and map the correct windows.
 *
 * @param arg arg->i indicates which workspace howm should change to.
 */
void change_ws(const Arg *arg)
{
	Client *c = wss[arg->i].head;

	if (arg->i > WORKSPACES || arg->i <= 0 || arg->i == cw)
		return;
	last_ws = cw;
	log_info("Changing from workspace <%d> to <%d>.", last_ws, arg->i);
	for (; c; c = c->next)
		xcb_map_window(dpy, c->win);
	for (c = wss[last_ws].head; c; c = c->next)
		xcb_unmap_window(dpy, c->win);
	cw = arg->i;
	update_focused_client(wss[cw].current);

	xcb_ewmh_set_current_desktop(ewmh, 0, cw - 1);
	xcb_ewmh_geometry_t workarea[] = { { 0, BAR_BOTTOM ? 0 : wss[cw].bar_height,
				screen_width, screen_height - wss[cw].bar_height } };
	xcb_ewmh_set_workarea(ewmh, 0, LENGTH(workarea), workarea);

	howm_info();
}

/**
 * @brief Focus the previous workspace.
 *
 * @param arg Unused.
 */
void focus_prev_ws(const Arg *arg)
{
	UNUSED(arg);

	log_info("Focusing previous workspace");
	change_ws(&(Arg){ .i = correct_ws(cw - 1) });
}

/**
 * @brief Focus the last focused workspace.
 *
 * @param arg Unused.
 */
void focus_last_ws(const Arg *arg)
{
	UNUSED(arg);

	log_info("Focusing last workspace");
	change_ws(&(Arg){ .i = last_ws });

}

/**
 * @brief Focus the next workspace.
 *
 * @param arg Unused.
 */
void focus_next_ws(const Arg *arg)
{
	UNUSED(arg);

	log_info("Focusing previous workspace");
	change_ws(&(Arg){ .i = correct_ws(cw + 1) });
}

/**
 * @brief Change the layout of the current workspace.
 *
 * @param arg A numerical value (arg->i) representing the layout that should be
 * used.
 */
void change_layout(const Arg *arg)
{
	if (arg->i == wss[cw].layout || arg->i >= END_LAYOUT || arg->i < 0)
		return;
	prev_layout = wss[cw].layout;
	wss[cw].layout = arg->i;
	update_focused_client(wss[cw].current);
	log_info("Changed layout from %d to %d", prev_layout,  wss[cw].layout);
}

/**
 * @brief Change to the previous layout.
 *
 * @param arg Unused.
 */
void previous_layout(const Arg *arg)
{
	UNUSED(arg);
	const Arg a = { .i = wss[cw].layout < 1 ? END_LAYOUT - 1 : wss[cw].layout - 1 };

	log_info("Changing to previous layout (%d)", a.i);
	change_layout(&a);
}

/**
 * @brief Change to the next layout.
 *
 * @param arg Unused.
 */
void next_layout(const Arg *arg)
{
	UNUSED(arg);
	const Arg a = { .i = (wss[cw].layout + 1) % END_LAYOUT };

	log_info("Changing to layout (%d)", a.i);
	change_layout(&a);
}

/**
 * @brief Change to the last used layout.
 *
 * @param arg Unused.
 */
void last_layout(const Arg *arg)
{
	UNUSED(arg);

	log_info("Changing to last layout (%d)", prev_layout);
	change_layout(&(Arg){ .i = prev_layout });
}

/**
 * @brief Change the mode of howm.
 *
 * Modes should be thought of in the same way as they are in vi. Different
 * modes mean keypresses cause different actions.
 *
 * @param arg arg->i defines which mode to be selected.
 */
void change_mode(const Arg *arg)
{
	if (arg->i >= (int)END_MODES || arg->i == (int)cur_mode)
		return;
	cur_mode = arg->i;
	log_info("Changing to mode %d", cur_mode);
	howm_info();
}

/**
 * @brief An operator that kills an arbitrary amount of clients or workspaces.
 *
 * @param type Whether to kill workspaces or clients.
 * @param cnt How many workspaces or clients to kill.
 */
void op_kill(const unsigned int type, int cnt)
{
	if (type == WORKSPACE) {
		log_info("Killing %d workspaces", cnt);
		while (cnt > 0) {
			kill_ws(correct_ws(cw + cnt - 1));
			cnt--;
		}
	} else if (type == CLIENT) {
		log_info("Killing %d clients", cnt);
		while (cnt > 0) {
			kill_client(cw, cnt == 1);
			cnt--;
		}
	}
}

/**
 * @brief Kills the current client on the workspace ws.
 *
 * @param ws The workspace that the current client to be killed is on.
 *
 * @param arrange Whether the windows should be rearranged.
 */
void kill_client(const int ws, bool arrange)
{
	xcb_icccm_get_wm_protocols_reply_t rep;
	unsigned int i;
	bool found = false;

	if (!wss[ws].current)
		return;

	if (xcb_icccm_get_wm_protocols_reply(dpy,
				xcb_icccm_get_wm_protocols(dpy,
					wss[ws].current->win,
					wm_atoms[WM_PROTOCOLS]), &rep, NULL)) {
		for (i = 0; i < rep.atoms_len; ++i)
			if (rep.atoms[i] == wm_atoms[WM_DELETE_WINDOW]) {
				delete_win(wss[ws].current->win);
				found = true;
				break;
			}
		xcb_icccm_get_wm_protocols_reply_wipe(&rep);
	}
	if (!found)
		xcb_kill_client(dpy, wss[ws].current->win);
	log_info("Killing Client <%p>", wss[ws].current);
	remove_client(wss[ws].current, arrange);
}

/**
 * @brief Kills the given workspace.
 *
 * @param ws The workspace to be killed.
 */
void kill_ws(const int ws)
{
	log_info("Killing off workspace <%d>", ws);
	while (wss[ws].head)
		kill_client(ws, wss[ws].client_cnt == 1
				&& cw == ws);
}

/**
 * @brief Move client/s down.
 *
 * @param type We don't support moving workspaces, so this should only be
 * client.
 * @param cnt How many clients to move.
 */
void op_move_down(const unsigned int type, int cnt)
{
	if (type == WORKSPACE)
		return;
	move_client(cnt, false);
}

/**
 * @brief Move client/s up.
 *
 * @param type We don't support moving workspaces, so this should only be
 * client.
 * @param cnt How many clients to move.
 */
void op_move_up(const unsigned int type, int cnt)
{
	if (type == WORKSPACE)
		return;
	move_client(cnt, true);
}

/**
 * @brief Moves a client either upwards or down.
 *
 * Moves a single client or multiple clients either up or
 * down. The op_move_* functions serves as simple wrappers to this.
 *
 * @param cnt How many clients to move.
 * @param up Whether to move the clients up or down. True is up.
 */
void move_client(int cnt, bool up)
{
	int cntcopy;
	Client *c;

	if (up) {
		if (wss[cw].current == wss[cw].head)
			return;
		c = prev_client(wss[cw].current, cw);
		/* TODO optimise this by inserting the client only once
			* and in the correct location.*/
		for (; cnt > 0; move_down(c), cnt--)
			;
	} else {
		if (wss[cw].current == prev_client(wss[cw].head, cw))
			return;
		cntcopy = cnt;
		for (c = wss[cw].current; cntcopy > 0; c = next_client(c), cntcopy--)
			;
		for (; cnt > 0; move_up(c), cnt--)
			;
	}
}

/**
 * @brief Moves the current client down.
 *
 * @param arg Unused.
 */
void move_current_down(const Arg *arg)
{
	UNUSED(arg);
	move_down(wss[cw].current);
}

/**
 * @brief Moves the current client up.
 *
 * @param arg Unused.
 */
void move_current_up(const Arg *arg)
{
	UNUSED(arg);
	move_up(wss[cw].current);
}

/**
 * @brief Moves a client from one workspace to another.
 *
 * @param c The client to be moved.
 * @param ws The ws that the client should be moved to.
 */
void client_to_ws(Client *c, const int ws, bool follow)
{
	Client *last;
	Client *prev = prev_client(c, cw);

	/* Performed for the current workspace. */
	if (!c || ws == cw)
		return;
	/* Target workspace. */
	last = prev_client(wss[ws].head, ws);
	if (!wss[ws].head)
		wss[ws].head = c;
	else if (last)
		last->next = c;
	else
		wss[ws].head->next = c;
	wss[ws].current = c;
	wss[ws].client_cnt++;

	/* Current workspace. */
	if (c == wss[cw].head || !prev)
		wss[cw].head = c->next;
	else
		prev->next = c->next;
	wss[cw].current = prev;
	wss[cw].client_cnt--;

	c->next = NULL;
	xcb_unmap_window(dpy, c->win);

	log_info("Moved client <%p> from <%d> to <%d>", c, cw, ws);
	if (follow) {
		wss[ws].current = c;
		change_ws(&(Arg){ .i = ws });
	} else {
		update_focused_client(prev);
	}
}

/**
 * @brief Moves the current client to the workspace passed in through arg.
 *
 * @param arg arg->i is the target workspace.
 */
void current_to_ws(const Arg *arg)
{

	client_to_ws(wss[cw].current, arg->i, FOLLOW_MOVE);
}

/**
 * @brief Correctly wrap a workspace number.
 *
 * This prevents workspace numbers from being greater than WORKSPACES or less
 * than 1.
 *
 * @param ws The value that needs to be corrected.
 *
 * @return A corrected workspace number.
 */
int correct_ws(int ws)
{
	if (ws > WORKSPACES)
		return ws - WORKSPACES;
	if (ws < 1)
		return ws + WORKSPACES;

	return ws;
}

/**
 * @brief Focus the given window, so long as it isn't already focused.
 *
 * @param win A window that belongs to a client being managed by howm.
 */
void focus_window(xcb_window_t win)
{
	Client *c = find_client_by_win(win);

	if (c && c != wss[cw].current)
		update_focused_client(c);
	else
		/* We don't want warnings for clicking the root window... */
		if (!win == screen->root)
			log_warn("No client owns the window <0x%x>", win);
}

/**
 * @brief Operator function to move the current focus up.
 *
 * @param type Whether to focus on clients or workspaces.
 * @param cnt The number of times to move focus.
 */
void op_focus_up(const unsigned int type, int cnt)
{
	while (cnt > 0) {
		if (type == CLIENT)
			focus_next_client(NULL);
		else if (type == WORKSPACE)
			focus_next_ws(NULL);
		else
			return;
		cnt--;
	}
}

/**
 * @brief Operator function to move the current focus down.
 *
 * @param type Whether to focus on clients or workspaces.
 * @param cnt The number of times to move focus.
 */
void op_focus_down(const unsigned int type, int cnt)
{
	while (cnt > 0) {
		if (type == CLIENT)
			focus_prev_client(NULL);
		else if (type == WORKSPACE)
			focus_prev_ws(NULL);
		else
			return;
		cnt--;
	}
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
 * @brief Arrange the client's windows on the screen.
 *
 * This function takes some strain off of the layout handlers by passing the
 * client's dimensions to move_resize. This splits the layout handlers into
 * smaller, more understandable parts.
 */
void draw_clients(void)
{
	Client *c = NULL;

	log_debug("Drawing clients");
	for (c = wss[cw].head; c; c = c->next)
		if (wss[cw].layout == ZOOM && ZOOM_GAP && !c->is_floating) {
			set_border_width(c->win, 0);
			move_resize(c->win, c->x + c->gap, c->y + c->gap,
					c->w - (2 * c->gap), c->h - (2 * c->gap));
		} else if (c->is_floating) {
			set_border_width(c->win, BORDER_PX);
			move_resize(c->win, c->x, c->y,
					c->w, c->h);
		} else if (c->is_fullscreen || wss[cw].layout == ZOOM) {
			set_border_width(c->win, 0);
			move_resize(c->win, c->x, c->y, c->w, c->h);
		} else {
			move_resize(c->win, c->x + c->gap, c->y + c->gap,
					c->w - (2 * (c->gap + BORDER_PX)),
					c->h - (2 * (c->gap + BORDER_PX)));
		}
}

/**
 * @brief Change the size and location of a client.
 *
 * @param c The client to be changed.
 * @param x The x coordinate of the client's window.
 * @param y The y coordinate of the client's window.
 * @param w The width of the client's window.
 * @param h The height of the client's window.
 */
void change_client_geom(Client *c, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	log_debug("Changing geometry of client <%p> from {%d, %d, %d, %d} to {%d, %d, %d, %d}",
			c, c->x, c->y, c->w, c->h, x, y, w, h);
	c->x = x;
	c->y = y;
	c->w = w;
	c->h = h;
}

/**
 * @brief An operator to shrink the gaps of either workspaces or clients by
 * OP_GAP_SIZE.
 *
 * When the type is workspace, the gap size for that workspace is also changed.
 * This means that new windows will be spawned in with the modified gap size.
 *
 * @param type Whether the operation should be performed on a client or
 * workspace.
 * @param cnt The amount of clients or workspaces to perform the operation on.
 */
static void op_shrink_gaps(const unsigned int type, int cnt)
{
	change_gaps(type, cnt, -OP_GAP_SIZE);
}

/**
 * @brief An operator to grow the gaps of either workspaces or clients by
 * OP_GAP_SIZE.
 *
 * When the type is workspace, the gap size for that workspace is also changed.
 * This means that new windows will be spawned in with the modified gap size.
 *
 * @param type Whether the operation should be performed on a client or
 * workspace.
 * @param cnt The amount of clients or workspaces to perform the operation on.
 */
static void op_grow_gaps(const unsigned int type, int cnt)
{
	change_gaps(type, cnt, OP_GAP_SIZE);
}

/**
 * @brief A helper function to change the size of a client's gaps.
 *
 * @param c The client who's gap size should be changed.
 * @param size The size by which the gap should be changed.
 */
static void change_client_gaps(Client *c, int size)
{
	if (c->is_fullscreen)
		return;
	if ((int)c->gap + size <= 0)
		c->gap = 0;
	else
		c->gap += size;

	uint32_t space = c->gap + BORDER_PX;

	xcb_ewmh_set_frame_extents(ewmh, c->win, space, space, space, space);
	draw_clients();
}

/**
 * @brief Does the heavy lifting of changing the gaps of clients.
 *
 * @param type Whether to perform the operation on a client or workspace.
 * @param cnt The amount of times to perform the operation.
 * @param size The amount of pixels to change the gap size by. This is
 * configured through OP_GAP_SIZE.
 */
static void change_gaps(const unsigned int type, int cnt, int size)
{
	Client *c = NULL;

	if (type == WORKSPACE) {
		while (cnt > 0) {
			cnt--;
			wss[correct_ws(cw + cnt)].gap += size;
			log_info("Changing gaps of workspace <%d> by %dpx",
					correct_ws(cw + cnt), size);
			for (c = wss[correct_ws(cw + cnt)].head; c; c = c->next)
				change_client_gaps(c, size);
		}
	} else if (type == CLIENT) {
		c = wss[cw].current;
		while (cnt > 0) {
			log_info("Changing gaps of client <%p> by %dpx", c, size);
			change_client_gaps(c, size);
			c = next_client(c);
			cnt--;
		}
	}

}

/**
 * @brief Toggle a client between being in a floating or non-floating state.
 *
 * @param arg Unused.
 */
static void toggle_float(const Arg *arg)
{
	UNUSED(arg);
	if (!wss[cw].current)
		return;
	log_info("Toggling floating state of client <%p>", wss[cw].current);
	wss[cw].current->is_floating = !wss[cw].current->is_floating;
	if (wss[cw].current->is_floating && CENTER_FLOATING) {
		wss[cw].current->x = (screen_width / 2) - (wss[cw].current->w / 2);
		wss[cw].current->y = (screen_height - wss[cw].bar_height - wss[cw].current->h) / 2;
		log_info("Centering client <%p>", wss[cw].current);
	}
	arrange_windows();
}

/**
 * @brief Change the width of a floating client.
 *
 * Negative values will shift the right edge of the window to the left. The
 * inverse is true for positive values.
 *
 * @param arg The amount of pixels that the window's size should be changed by.
 */
static void resize_float_width(const Arg *arg)
{
	if (!wss[cw].current || !wss[cw].current->is_floating || (int)wss[cw].current->w + arg->i <= 0)
		return;
	log_info("Resizing width of client <%p> from %d by %d", wss[cw].current, wss[cw].current->w, arg->i);
	wss[cw].current->w += arg->i;
	draw_clients();
}

/**
 * @brief Change the height of a floating client.
 *
 * Negative values will shift the bottom edge of the window to the top. The
 * inverse is true for positive values.
 *
 * @param arg The amount of pixels that the window's size should be changed by.
 */
static void resize_float_height(const Arg *arg)
{
	if (!wss[cw].current || !wss[cw].current->is_floating || (int)wss[cw].current->h + arg->i <= 0)
		return;
	log_info("Resizing height of client <%p> from %d to %d", wss[cw].current, wss[cw].current->h, arg->i);
	wss[cw].current->h += arg->i;
	draw_clients();
}

/**
 * @brief Change a floating window's y coordinate.
 *
 * Negative values will move the window up. The inverse is true for positive
 * values.
 *
 * @param arg The amount of pixels that the window should be moved.
 */
static void move_float_y(const Arg *arg)
{
	if (!wss[cw].current || !wss[cw].current->is_floating)
		return;
	log_info("Changing y of client <%p> from %d to %d", wss[cw].current, wss[cw].current->y, arg->i);
	wss[cw].current->y += arg->i;
	draw_clients();

}

/**
 * @brief Change a floating window's x coordinate.
 *
 * Negative values will move the window to the left. The inverse is true
 * for positive values.
 *
 * @param arg The amount of pixels that the window should be moved.
 */
static void move_float_x(const Arg *arg)
{
	if (!wss[cw].current || !wss[cw].current->is_floating)
		return;
	log_info("Changing x of client <%p> from %d to %d", wss[cw].current, wss[cw].current->x, arg->i);
	wss[cw].current->x += arg->i;
	draw_clients();

}

/**
 * @brief Teleport a floating client's window to a location on the screen.
 *
 * @param arg Which location to teleport the window to.
 */
static void teleport_client(const Arg *arg)
{
	if (!wss[cw].current || !wss[cw].current->is_floating
			|| wss[cw].current->is_transient)
		return;

	/* A bit naughty, but it looks nicer- doesn't it?*/
	uint16_t g = wss[cw].current->gap;
	uint16_t w = wss[cw].current->w;
	uint16_t h = wss[cw].current->h;
	uint16_t bh = wss[cw].bar_height;

	switch (arg->i) {
	case TOP_LEFT:
		wss[cw].current->x = g;
		wss[cw].current->y = (BAR_BOTTOM ? 0 : bh) + g;
		break;
	case TOP_CENTER:
		wss[cw].current->x = (screen_width - w) / 2;
		wss[cw].current->y = (BAR_BOTTOM ? 0 : bh) + g;
		break;
	case TOP_RIGHT:
		wss[cw].current->x = screen_width - w - g - (2 * BORDER_PX);
		wss[cw].current->y = (BAR_BOTTOM ? 0 : bh) + g;
		break;
	case CENTER:
		wss[cw].current->x = (screen_width - w) / 2;
		wss[cw].current->y = (screen_height - bh - h) / 2;
		break;
	case BOTTOM_LEFT:
		wss[cw].current->x = g;
		wss[cw].current->y = (BAR_BOTTOM ? screen_height - bh : screen_height) - h - g - (2 * BORDER_PX);
		break;
	case BOTTOM_CENTER:
		wss[cw].current->x = (screen_width / 2) - (w / 2);
		wss[cw].current->y = (BAR_BOTTOM ? screen_height - bh : screen_height) - h - g - (2 * BORDER_PX);
		break;
	case BOTTOM_RIGHT:
		wss[cw].current->x = screen_width - w - g - (2 * BORDER_PX);
		wss[cw].current->y = (BAR_BOTTOM ? screen_height - bh : screen_height) - h - g - (2 * BORDER_PX);
		break;
	};
	draw_clients();
}

/**
 * @brief Quit howm and set the return value.
 *
 * @param arg The return value that howm will send.
 */
static void quit_howm(const Arg *arg)
{
	log_warn("Quitting");
	retval = arg->i;
	running = false;
}

/**
 * @brief Cleanup howm's resources.
 *
 * Delete all of the windows that have been created, remove button and key
 * grabs and remove pointer focus.
 */
static void cleanup(void)
{
	xcb_window_t *w;
	xcb_query_tree_reply_t *q;
	uint16_t i;

	log_warn("Cleaning up");
	xcb_ungrab_key(dpy, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);

	q = xcb_query_tree_reply(dpy, xcb_query_tree(dpy, screen->root), 0);
	if (q) {
		w = xcb_query_tree_children(q);
		for (i = 0; i != q->children_len; ++i)
			delete_win(w[i]);
	free(q);
	}
	xcb_set_input_focus(dpy, XCB_INPUT_FOCUS_POINTER_ROOT, screen->root,
			XCB_CURRENT_TIME);
	xcb_ewmh_connection_wipe(ewmh);
	if (ewmh)
		free(ewmh);
	stack_free(&del_reg);
}

/**
 * @brief Ask XCB to delete a window.
 *
 * @param win The window to be deleted.
 */
static void delete_win(xcb_window_t win)
{
	xcb_client_message_event_t ev;

	log_info("Sending WM_DELETE_WINDOW to window <0x%x>", win);
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.sequence = 0;
	ev.format = 32;
	ev.window = win;
	ev.type = wm_atoms[WM_PROTOCOLS];
	ev.data.data32[0] = wm_atoms[WM_DELETE_WINDOW];
	ev.data.data32[1] = XCB_CURRENT_TIME;
	xcb_send_event(dpy, 0, win, XCB_EVENT_MASK_NO_EVENT, (char *)&ev);
}

/**
 * @brief Resize the master window of a stack for the current workspace.
 *
 * @param arg The amount to resize the master window by. Treated as a
 * percentage. e.g. arg->i = 5 will increase the master window's size by 5% of
 * it maximum.
 */
static void resize_master(const Arg *arg)
{
	/* Resize master only when resizing is visible (i.e. in Stack layouts). */
	if (wss[cw].layout != HSTACK && wss[cw].layout != VSTACK)
		return;

	float change = ((float)arg->i) / 100;

	if (wss[cw].master_ratio + change >= 1
			|| wss[cw].master_ratio + change <= 0.1)
		return;
	log_info("Resizing master_ratio from <%.2f> to <%.2f>", wss[cw].master_ratio, wss[cw].master_ratio + change);
	wss[cw].master_ratio += change;
	arrange_windows();
}

/**
 * @brief Toggle the space reserved for a status bar.
 *
 * @param arg Unused.
 */
static void toggle_bar(const Arg *arg)
{
	UNUSED(arg);
	if (wss[cw].bar_height == 0 && BAR_HEIGHT > 0) {
		wss[cw].bar_height = BAR_HEIGHT;
		log_info("Toggled bar to shown");
	} else if (wss[cw].bar_height == BAR_HEIGHT) {
		wss[cw].bar_height = 0;
		log_info("Toggled bar to hidden");
	} else {
		return;
	}
	xcb_ewmh_geometry_t workarea[] = { { 0, BAR_BOTTOM ? 0 : wss[cw].bar_height,
				screen_width, screen_height - wss[cw].bar_height } };
	xcb_ewmh_set_workarea(ewmh, 0, LENGTH(workarea), workarea);
	arrange_windows();
}

/**
 * @brief Convert a window into a client.
 *
 * @param w A valid xcb window.
 *
 * @return A client that has already been inserted into the linked list of
 * clients.
 */
Client *create_client(xcb_window_t w)
{
	Client *c = (Client *)calloc(1, sizeof(Client));
	Client *t = prev_client(wss[cw].head, cw); /* Get the last element. */
	uint32_t vals[1] = { XCB_EVENT_MASK_PROPERTY_CHANGE |
				 (FOCUS_MOUSE ? XCB_EVENT_MASK_ENTER_WINDOW : 0)};

	if (!c) {
		log_err("Can't allocate memory for client.");
		exit(EXIT_FAILURE);
	}
	if (!wss[cw].head)
		wss[cw].head = c;
	else if (t)
		t->next = c;
	else
		wss[cw].head->next = c;
	c->win = w;
	c->gap = wss[cw].gap;
	xcb_change_window_attributes(dpy, c->win, XCB_CW_EVENT_MASK, vals);
	uint32_t space = c->gap + BORDER_PX;

	xcb_ewmh_set_frame_extents(ewmh, c->win, space, space, space, space);
	log_info("Created client <%p>", c);
	wss[cw].client_cnt++;
	return c;
}

/**
 * @brief Moves the current window to the master window, when in stack mode.
 *
 * @param arg Unused
 */
static void make_master(const Arg *arg)
{
	UNUSED(arg);
	if (!wss[cw].current || !wss[cw].head->next
			|| wss[cw].head == wss[cw].current
			|| !(wss[cw].layout == HSTACK
			|| wss[cw].layout == VSTACK))
		return;
	while (wss[cw].current != wss[cw].head)
		move_up(wss[cw].current);
	update_focused_client(wss[cw].head);
}

/**
 * @brief Create the EWMH connection, request all of the atoms and set some
 * sensible defaults for them.
 */
void setup_ewmh(void)
{
	xcb_ewmh_coordinates_t viewport[] = { {0, 0} };
	xcb_ewmh_geometry_t workarea[] = { {0, BAR_BOTTOM ? 0 : wss[cw].bar_height,
		 screen_width, screen_height - wss[cw].bar_height} };

	ewmh = calloc(1, sizeof(xcb_ewmh_connection_t));
	if (!ewmh) {
		log_err("Unable to create ewmh connection\n");
		exit(EXIT_FAILURE);
	}
	if (xcb_ewmh_init_atoms_replies(ewmh, xcb_ewmh_init_atoms(dpy, ewmh), NULL) == 0)
		log_err("Couldn't initialise ewmh atoms");

	xcb_atom_t ewmh_net_atoms[] = { ewmh->_NET_SUPPORTED,
				ewmh->_NET_SUPPORTING_WM_CHECK,
				ewmh->_NET_DESKTOP_VIEWPORT,
				ewmh->_NET_WM_NAME,
				ewmh->_NET_WM_STATE,
				ewmh->_NET_CLOSE_WINDOW,
				ewmh->_NET_WM_STATE_FULLSCREEN,
				ewmh->_NET_CURRENT_DESKTOP,
				ewmh->_NET_NUMBER_OF_DESKTOPS,
				ewmh->_NET_DESKTOP_GEOMETRY,
				ewmh->_NET_WORKAREA,
				ewmh->_NET_ACTIVE_WINDOW };

	xcb_ewmh_set_supported(ewmh, 0, LENGTH(ewmh_net_atoms), ewmh_net_atoms);
	xcb_ewmh_set_supporting_wm_check(ewmh, 0, screen->root);
	xcb_ewmh_set_desktop_viewport(ewmh, 0, LENGTH(viewport), viewport);
	xcb_ewmh_set_wm_name(ewmh, 0, strlen("howm"), "howm");
	xcb_ewmh_set_current_desktop(ewmh, 0, DEFAULT_WORKSPACE);
	xcb_ewmh_set_number_of_desktops(ewmh, 0, WORKSPACES);
	xcb_ewmh_set_workarea(ewmh, 0, LENGTH(workarea), workarea);
	xcb_ewmh_set_desktop_geometry(ewmh, 0, screen_width, screen_height);
}

/**
 * @brief Set the fullscreen state of the client. Change its geometry and
 * border widths.
 *
 * @param c The client which should have its fullscreen state altered.
 * @param fscr The fullscreen state that the client should be changed to.
 */
static void set_fullscreen(Client *c, bool fscr)
{
	long data[] = {fscr ? ewmh->_NET_WM_STATE_FULLSCREEN : XCB_NONE };

	if (!c || fscr == c->is_fullscreen)
		return;

	c->is_fullscreen = fscr;
	log_info("Setting client <%p>'s fullscreen state to %d", c, fscr);
	xcb_change_property(dpy, XCB_PROP_MODE_REPLACE,
			c->win, ewmh->_NET_WM_STATE, XCB_ATOM_ATOM, 32,
			fscr, data);
	if (fscr) {
		set_border_width(c->win, 0);
		change_client_geom(c, 0, 0, screen_width, screen_height);
		draw_clients();
	} else {
		set_border_width(c->win, !wss[cw].head->next ? 0 : BORDER_PX);
		arrange_windows();
		draw_clients();
	}
}

static void set_urgent(Client *c, bool urg)
{
	if (!c || urg == c->is_urgent)
		return;

	c->is_urgent = urg;
	xcb_change_window_attributes(dpy, c->win, XCB_CW_BORDER_PIXEL,
			urg ? &border_urgent : c == wss[cw].current
			? &border_focus : &border_unfocus);
}

/**
 * @brief Toggle the fullscreen state of the current client.
 *
 * @param arg Unused.
 */
static void toggle_fullscreen(const Arg *arg)
{
	UNUSED(arg);
	if (wss[cw].current != NULL)
		set_fullscreen(wss[cw].current, !wss[cw].current->is_fullscreen);
}

/**
 * @brief Handle messages sent by the client to alter its state.
 *
 * @param ev The client message as a generic event.
 */
static void client_message_event(xcb_generic_event_t *ev)
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

/**
 * @brief Save last operator, count and motion. These saved values are then
 * used in replay.
 *
 * @param op The last operator.
 * @param type The last type (defined by a motion).
 * @param cnt The last count.
 */
static void save_last_ocm(void (*op)(const unsigned int, int), const unsigned int type, int cnt)
{
	rep_state.last_op = op;
	rep_state.last_type = type;
	rep_state.last_cnt = cnt;
	rep_state.last_cmd = NULL;
}

/**
 * @brief Save the last command and argument that was passed to it. These saved
 * values are then used in replay.
 *
 * @param cmd The last command.
 * @param arg The argument passed to the last command.
 */
static void save_last_cmd(void (*cmd)(const Arg *arg), const Arg *arg)
{
	rep_state.last_cmd = cmd;
	rep_state.last_arg = arg;
	rep_state.last_op = NULL;
}

/**
 * @brief Replay the last command or operator, complete with the last arguments
 * passed to them.
 *
 * @param arg Unused
 */
static void replay(const Arg *arg)
{
	UNUSED(arg);
	if (rep_state.last_cmd)
		rep_state.last_cmd(rep_state.last_arg);
	else
		rep_state.last_op(rep_state.last_type, rep_state.last_cnt);
}

/**
 * @brief Restart howm.
 *
 * @param arg Unused.
 */
static void restart_howm(const Arg *arg)
{
	UNUSED(arg);
	log_warn("Restarting.");
	running = false;
	restart = true;
}

/**
 * @brief Handle client messages that are related to WM_STATE.
 *
 * TODO: Add more WM_STATE hints.
 *
 * @param c The client that is to have its WM_STATE modified.
 * @param a The atom representing which WM_STATE hint should be modified.
 * @param action Whether to remove, add or toggle the WM_STATE hint.
 */
static void ewmh_process_wm_state(Client *c, xcb_atom_t a, int action)
{
	if (a == ewmh->_NET_WM_STATE_FULLSCREEN) {
		if (action == _NET_WM_STATE_REMOVE)
			set_fullscreen(c, false);
		else if (action == _NET_WM_STATE_ADD)
			set_fullscreen(c, true);
		else if (action == _NET_WM_STATE_TOGGLE)
			set_fullscreen(c, !c->is_fullscreen);
	} else if (a == ewmh->_NET_WM_STATE_DEMANDS_ATTENTION) {
		if (action == _NET_WM_STATE_REMOVE)
			set_urgent(c, false);
		else if (action == _NET_WM_STATE_ADD)
			set_urgent(c, true);
		else if (action == _NET_WM_STATE_TOGGLE)
			set_urgent(c, !c->is_urgent);
	} else {
		log_warn("Unhandled wm state <%d> with action <%d>.", a, action);
	}
}

/**
 * @brief Dynamically allocate space for the contents of the stack.
 *
 * We don't know how big the stack will be when the struct is defined, so we
 * need to allocate it dynamically.
 *
 * @param s The stack that needs to have its contents allocated.
 */
static void stack_init(struct stack *s)
{
	s->contents = (Client **)malloc(sizeof(Client) * DELETE_REGISTER_SIZE);
	if (!s->contents)
		log_err("Failed to allocate memory for stack.");
}

/**
 * @brief Free the allocated contents.
 *
 * @param s The stack that needs to have its contents freed.
 */
static void stack_free(struct stack *s)
{
	free(s->contents);
}

/**
 * @brief Pushes a client onto the stack, as long as it isn't full.
 *
 * @param s The stack.
 * @param c The client to be pushed on. This client is treated as the head of a
 * linked list.
 */
static void stack_push(struct stack *s, Client *c)
{
	if (!c || !s) {
		return;
	} else if (s->size >= DELETE_REGISTER_SIZE) {
		log_warn("Can't push <%p> onto stack <%p>- it is full", c, s);
		return;
	}
	s->contents[++(s->size)] = c;
}

/**
 * @brief Remove the top item from the stack and return it.
 *
 * @param s The stack to be popped from.
 *
 * @return The client that was at the top of the stack. It acts as the head of
 * the linked list of clients.
 */
static Client *stack_pop(struct stack *s)
{
	if (!s) {
		return NULL;
	} else if (s->size == 0) {
		log_warn("Can't pop from stack <%p> as it is empty.", s);
		return NULL;
	}
	return s->contents[(s->size)--];
}

/**
 * @brief Cut one or more clients and add them onto howm's delete register
 * stack (if there is space).
 *
 * A segment of howm's internal client list is taken and placed onto the delete
 * register stack. All clients from the list segment must be unmapped and the
 * remaining clients must be refocused.
 *
 * @param type Whether to cut an entire workspace or client.
 * @param cnt The amount of clients or workspaces to cut.
 */
static void op_cut(const unsigned int type, int cnt)
{
	Client *tail = wss[cw].current;
	Client *head = wss[cw].current;
	Client *head_prev = prev_client(wss[cw].current, cw);
	bool wrap = false;

	if (!head)
		return;

	if (del_reg.size >= DELETE_REGISTER_SIZE) {
		log_warn("No more stack space.");
		return;
	}

	if ((type == CLIENT && cnt >= wss[cw].client_cnt) || type == WORKSPACE) {
		if (cnt + del_reg.size > DELETE_REGISTER_SIZE)
			return;

		while (cnt > 0) {
			head = wss[correct_ws(cw + cnt - 1)].head;
			for (tail = head; tail; tail = tail->next)
				xcb_unmap_window(dpy, tail->win);
			stack_push(&del_reg, head);
			wss[correct_ws(cw + cnt - 1)].head = NULL;
			wss[correct_ws(cw + cnt - 1)].prev_foc = NULL;
			wss[correct_ws(cw + cnt - 1)].current = NULL;
			cnt--;
			wss[correct_ws(cw + cnt - 1)].client_cnt = 0;
		}

	} else if (type == CLIENT) {
		xcb_unmap_window(dpy, head->win);
		wss[cw].client_cnt--;
		while (cnt > 1) {
			if (!tail->next && next_client(tail)) {
				wrap = true;
				/* Join the list into a circular linked list,
				 * just for now so that we don't miss any
				 * clients. */
				tail->next = next_client(tail);
			}
			if (tail == wss[cw].prev_foc)
				wss[cw].prev_foc = NULL;
			tail = next_client(tail);
			xcb_unmap_window(dpy, tail->win);
			cnt--;
			wss[cw].client_cnt--;
		}

		if (head == wss[cw].head) {
			wss[cw].head = head == next_client(tail) ? NULL : next_client(tail);
		} else if (wrap) {
			wss[cw].head = tail->next;
			head_prev->next = NULL;
		} else if (tail->next != head_prev) {
			head_prev->next = wrap ? NULL : tail->next;
		}

		wss[cw].current = head_prev;
		tail->next = NULL;
		update_focused_client(head_prev);
		stack_push(&del_reg, head);
	}
}

/**
 * @brief Focus a client that has an urgent hint.
 *
 * @param arg Unused.
 */
static void focus_urgent(const Arg *arg)
{
	UNUSED(arg);
	Client *c;
	int w;

	for (w = 1; w <= WORKSPACES; w++)
		for (c = wss[w].head; c && !c->is_urgent; c = c->next)
			;
	if (c) {
		log_info("Focusing urgent client <%p> on workspace <%d>", c, w);
		change_ws(&(Arg){.i = w});
		update_focused_client(c);
	}
}

/**
 * @brief Remove a list of clients from howm's delete register stack and paste
 * them after the currently focused window.
 *
 * @param arg Unused
 */
static void paste(const Arg *arg)
{
	UNUSED(arg);
	Client *head = stack_pop(&del_reg);
	Client *t, *c = head;

	if (!head) {
		log_warn("No clients on stack.");
		return;
	}

	if (!wss[cw].current) {
		wss[cw].head = head;
		wss[cw].current = head;
		while (c) {
			xcb_map_window(dpy, c->win);
			wss[cw].current = c;
			c = c->next;
			wss[cw].client_cnt++;
		}
	} else if (!wss[cw].current->next) {
		wss[cw].current->next = head;
		while (c) {
			xcb_map_window(dpy, c->win);
			wss[cw].current = c;
			c = c->next;
			wss[cw].client_cnt++;
		}
	} else {
		t = wss[cw].current->next;
		wss[cw].current->next = head;
		while (c) {
			xcb_map_window(dpy, c->win);
			wss[cw].client_cnt++;
			if (!c->next) {
				c->next = t;
				wss[cw].current = c;
				break;
			} else {
				wss[cw].current = c;
				c = c->next;
			}
		}
	}
	update_focused_client(wss[cw].current);
}

/**
 * @brief Set the properties of a client that has just been created,
 * according to the rules defined in the config file.
 *
 * @param c The client that has been created.
 */
static void apply_rules(Client *c)
{
	xcb_icccm_get_wm_class_reply_t wc;
	unsigned int i;

	if (xcb_icccm_get_wm_class_reply(dpy, xcb_icccm_get_wm_class(dpy,
					c->win), &wc, NULL)) {
		for (i = 0; i < LENGTH(rules); i++) {
			if (strstr(wc.instance_name, rules[i].class)
					|| strstr(wc.class_name, rules[i].class)) {
				c->is_floating = rules[i].is_floating;
				c->is_fullscreen = rules[i].is_fullscreen;
				client_to_ws(c, rules[i].ws == 0 ? cw
						: rules[i].ws, rules[i].follow);
				break;
			}
		}
		xcb_icccm_get_wm_class_reply_wipe(&wc);
	}
}

/**
 * @brief Send a client to the scratchpad and unmap it.
 *
 * @param arg Unused.
 */
void send_to_scratchpad(const Arg *arg)
{
	UNUSED(arg);
	Client *c = wss[cw].current;

	if (scratchpad || !c)
		return;

	log_info("Sending client <%p> to scratchpad", c);
	if (prev_client(c, cw))
		prev_client(c, cw)->next = c->next;

	/* TODO: This should be in a reusable function. */
	if (c == wss[cw].prev_foc)
		wss[cw].prev_foc = prev_client(wss[cw].current, cw);
	if (c == wss[cw].current || !wss[cw].head->next)
		wss[cw].current = wss[cw].prev_foc ? wss[cw].prev_foc : wss[cw].head;
	if (c == wss[cw].head) {
		wss[cw].head = c->next;
		wss[cw].current = c->next;
	}

	xcb_unmap_window(dpy, c->win);
	wss[cw].client_cnt--;
	update_focused_client(wss[cw].current);
	scratchpad = c;
}

/**
 * @brief Get a client from the scratchpad, attach it as the last item in the
 * client list and set it to float.
 *
 * @param arg Unused.
 */
void get_from_scratchpad(const Arg *arg)
{
	UNUSED(arg);
	if (!scratchpad)
		return;

	/* TODO: This should be in a reusable function. */
	if (!wss[cw].head)
		wss[cw].head = scratchpad;
	else if (!wss[cw].head->next)
		wss[cw].head->next = scratchpad;
	else
		prev_client(wss[cw].head, cw)->next = scratchpad;


	wss[cw].prev_foc = wss[cw].current;
	wss[cw].current = scratchpad;

	scratchpad = NULL;
	wss[cw].client_cnt++;

	wss[cw].current->is_floating = true;
	wss[cw].current->w = SCRATCHPAD_WIDTH;
	wss[cw].current->h = SCRATCHPAD_HEIGHT;
	wss[cw].current->x = (screen_width / 2) - (wss[cw].current->w / 2);
	wss[cw].current->y = (screen_height - wss[cw].bar_height - wss[cw].current->h) / 2;

	xcb_map_window(dpy, wss[cw].current->win);
	update_focused_client(wss[cw].current);
}

static int ipc_init(void)
{
	struct sockaddr_un addr;
	int sock_fd;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCK_PATH);
	unlink(SOCK_PATH);
	sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);

	if (sock_fd == -1) {
		log_err("Couldn't create the socket.");
		exit(EXIT_FAILURE);
	}

	if (bind(sock_fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		log_err("Couldn't bind a name to the socket.");
		exit(EXIT_FAILURE);
	}

	if (listen(sock_fd, 1) == -1) {
		log_err("Listening error.");
		exit(EXIT_FAILURE);
	}

	return sock_fd;
}

/**
 * @brief Receive a char array from a UNIX socket  and subsequently call a
 * function, passing the args from within msg.
 *
 * @param msg A char array from the UNIX socket. In the form:
 *
 * COMMAND\0ARG1\0ARG2\0 ....
 *
 * @param len The length of the msg.
 *
 * @return The error code, as set by this function itself or those that it
 * calls.
 */
static int ipc_process_cmd(char *msg, int len)
{
	unsigned int i;
	bool found = false;
	int err = IPC_ERR_NONE;
	char **args = ipc_process_args(msg, len, &err);

	if (err != IPC_ERR_NONE)
		goto end;

	for (i = 0; i < LENGTH(commands); i++)
		if (strcmp(args[0], commands[i].name) == 0) {
			found = true;
			if (commands[i].argc == 0) {
				commands[i].func(&(Arg){ NULL });
				break;
			} else if (commands[i].argc == 1 && args[1] && commands[i].arg_type == TYPE_INT) {
				commands[i].func(&(Arg){ .i = ipc_arg_to_int(args[1], &err) });
				break;
			} else if (commands[i].argc == 1 && args[1] && commands[i].arg_type == TYPE_CMD) {
				commands[i].func(&(Arg){ .cmd = args + 1 });
				break;
			} else if (commands[i].argc == 2 && args[1] && args[2] && *args[2] == 'w') {
				commands[i].operator(WORKSPACE, ipc_arg_to_int(args[1], &err));
				break;
			} else if (commands[i].argc == 2 && args[1] && args[2] && *args[2] == 'c') {
				commands[i].operator(CLIENT, ipc_arg_to_int(args[1], &err));
				break;
			} else {
				err = IPC_ERR_SYNTAX;
				goto end;
			}
		}
	err = found == true ? err : IPC_ERR_NO_CMD;
	goto end;

end:
	free(args);
	return err;
}

/**
 * @brief Convert a numerical string into a decimal value, such as "12"
 * becoming 12.
 *
 * Minus signs are handled. It is assumed that a two digit number won't start
 * with a zero. Args with more than two digits will not be accepted, nor will
 * args that aren't numerical.
 *
 * @param arg The string to be converted.
 * @param err Where errors are reported.
 *
 * @return The decimal representation of arg.
 */
static int ipc_arg_to_int(char *arg, int *err)
{
	int sign = 1;

	if (arg[0] == '-') {
		sign = -1;
		arg++;
	}

	if (strlen(arg) == 1 && '0' < *arg && *arg <= '9') {
		if ('0' <= *arg && *arg <= '9')
			return sign * (*arg - '0');
		*err = IPC_ERR_ARG_NOT_INT;
		return 0;
	} else if (strlen(arg) == 2 && '0' < arg[0] && arg[0] <= '9'
			&& '0' <= arg[1] && arg[1] <= '9') {
		if ('0' < arg[0] && arg[0] <= '9' && '0' <= arg[1] && arg[1] <= '9')
			return sign * (10 * (arg[0] - '0') + (arg[1] - '0'));
		*err = IPC_ERR_ARG_NOT_INT;
		return 0;
	} else {
		*err = IPC_ERR_ARG_TOO_LARGE;
		return 0;
	}
}

/**
 * @brief Accepts a char array and convert it into an array of strings.
 *
 * msg is split into strings (delimited by a null character) and placed in an
 * array. err is set with a corresponding error (such as args too few args), or
 * nothing.
 *
 * XXX: args must be freed by the caller.
 *
 * @param msg A char array that is read from a UNIX socket.
 * @param len The length of data in msg.
 * @param err Where any errors will be stored.
 *
 * @return A pointer to an array of strings, each one representing an argument
 * that has been passed over a UNIX socket.
 */
static char **ipc_process_args(char *msg, int len, int *err)
{
	int argc = 0, i = 0, arg_start = 0, lim = 2;
	char **args = malloc(lim * sizeof(char *));

	if (!args) {
		*err = IPC_ERR_ALLOC;
		return NULL;
	}

	for (; i < len; i++) {
		if (msg[i] == 0) {
			args[argc++] = msg + arg_start;
			arg_start = i + 1;

			if (argc == lim) {
				lim *= 2;
				char **new = realloc(args, lim * sizeof(char *));

				if (!new) {
					*err = IPC_ERR_ALLOC;
					return NULL;
				}
				args = new;
			}
		}
	}

	/* Make room to add the NULL after the last character. */
	if (argc == lim) {
		char **new = realloc(args, (lim + 1) * sizeof(char *));

		if (!new) {
			*err = IPC_ERR_ALLOC;
			return NULL;
		}
		args = new;
	}

	/* The end of the array should be NULL, as the whole array can be passed to
	 * spawn() and that expects a NULL terminated array.
	 *
	 * Use argc here as args are zero indexed. */
	args[argc] = NULL;

	if (argc < 1) {
		*err = IPC_ERR_TOO_FEW_ARGS;
		free(args);
		return NULL;
	}

	return args;
}
