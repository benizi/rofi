/**
 * rofi
 *
 * MIT/X11 License
 * Modified 2016 Qball Cow <qball@gmpclient.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <locale.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <xcb/xkb.h>
#include <xcb/xcb_ewmh.h>

#include <cairo.h>
#include <cairo-xcb.h>

#define SN_API_NOT_YET_FROZEN
#include <libsn/sn.h>

#include "settings.h"

#include "rofi.h"
#include "mode.h"
#include "xcb-internal.h"
#include "xkb-internal.h"
#include "helper.h"
#include "textbox.h"
#include "scrollbar.h"
#include "x11-helper.h"
#include "xrmoptions.h"
#include "dialogs/dialogs.h"

#include "view.h"
#include "view-internal.h"

#include "xcb.h"

const char *const separator_style_none = "none";
const char *const separator_style_dash = "dash";

GThreadPool       *tpool = NULL;

RofiViewState     *current_active_menu = NULL;

struct
{
    xcb_window_t    main_window;
    cairo_surface_t *surface;
    cairo_surface_t *fake_bg;
    cairo_t         *draw;
} CacheState = {
    .main_window = XCB_WINDOW_NONE,
    .surface     = NULL,
    .fake_bg     = NULL,
    .draw        = NULL,
};

static char * get_matching_state ( void )
{
    if ( config.case_sensitive ) {
        if ( config.levenshtein_sort ) {
            return "±";
        }
        else {
            return "-";
        }
    }
    else{
        if ( config.levenshtein_sort ) {
            return "+";
        }
    }
    return " ";
}

/**
 * Levenshtein Sorting.
 */
static int lev_sort ( const void *p1, const void *p2, void *arg )
{
    const int *a         = p1;
    const int *b         = p2;
    int       *distances = arg;

    return distances[*a] - distances[*b];
}

/**
 * Stores a screenshot of Rofi at that point in time.
 */
static void menu_capture_screenshot ( void )
{
    const char *outp = g_getenv ( "ROFI_PNG_OUTPUT" );
    if ( CacheState.surface == NULL ) {
        // Nothing to store.
        fprintf ( stderr, "There is no rofi surface to store\n" );
        return;
    }
    const char *xdg_pict_dir = g_get_user_special_dir ( G_USER_DIRECTORY_PICTURES );
    if ( outp == NULL && xdg_pict_dir == NULL ) {
        fprintf ( stderr, "XDG user picture directory or ROFI_PNG_OUTPUT is not set. Cannot store screenshot.\n" );
        return;
    }
    // Get current time.
    GDateTime *now = g_date_time_new_now_local ();
    // Format filename.
    char      *timestmp = g_date_time_format ( now, "rofi-%Y-%m-%d-%H%M" );
    char      *filename = g_strdup_printf ( "%s.png", timestmp );
    // Build full path
    char      *fpath = NULL;
    if ( outp == NULL ) {
        int index = 0;
        fpath = g_build_filename ( xdg_pict_dir, filename, NULL );
        while ( g_file_test ( fpath, G_FILE_TEST_EXISTS ) && index < 99 ) {
            g_free ( fpath );
            g_free ( filename );
            // Try the next index.
            index++;
            // Format filename.
            filename = g_strdup_printf ( "%s-%d.png", timestmp, index );
            // Build full path
            fpath = g_build_filename ( xdg_pict_dir, filename, NULL );
        }
    }
    else {
        fpath = g_strdup ( outp );
    }
    fprintf ( stderr, color_green "Storing screenshot %s\n"color_reset, fpath );
    cairo_status_t status = cairo_surface_write_to_png ( CacheState.surface, fpath );
    if ( status != CAIRO_STATUS_SUCCESS ) {
        fprintf ( stderr, "Failed to produce screenshot '%s', got error: '%s'\n", filename,
                  cairo_status_to_string ( status ) );
    }
    g_free ( fpath );
    g_free ( filename );
    g_free ( timestmp );
    g_date_time_unref ( now );
}

/**
 * @param state   the state of the View.
 * @param mon     the work area.
 *
 * Calculates the window poslition
 */
static void calculate_window_position ( RofiViewState *state )
{
    if ( config.fullscreen ) {
        state->x = state->mon.x;
        state->y = state->mon.y;
        return;
    }

    // Default location is center.
    state->y = state->mon.y + ( state->mon.h - state->h ) / 2;
    state->x = state->mon.x + ( state->mon.w - state->w ) / 2;
    // Determine window location
    switch ( config.location )
    {
    case WL_NORTH_WEST:
        state->x = state->mon.x;
    case WL_NORTH:
        state->y = state->mon.y;
        break;
    case WL_NORTH_EAST:
        state->y = state->mon.y;
    case WL_EAST:
        state->x = state->mon.x + state->mon.w - state->w;
        break;
    case WL_EAST_SOUTH:
        state->x = state->mon.x + state->mon.w - state->w;
    case WL_SOUTH:
        state->y = state->mon.y + state->mon.h - state->h;
        break;
    case WL_SOUTH_WEST:
        state->y = state->mon.y + state->mon.h - state->h;
    case WL_WEST:
        state->x = state->mon.x;
        break;
    case WL_CENTER:
    default:
        break;
    }
    // Apply offset.
    state->x += config.x_offset;
    state->y += config.y_offset;
}

void rofi_view_queue_redraw ( void  )
{
    if ( current_active_menu ) {
        current_active_menu->update = TRUE;
        xcb_clear_area ( xcb->connection, current_active_menu->window, 1, 0, 0, 1, 1 );
        xcb_flush ( xcb->connection );
    }
}

void rofi_view_restart ( RofiViewState *state )
{
    state->quit = FALSE;
    state->retv = MENU_CANCEL;
}

RofiViewState * rofi_view_get_active ( void )
{
    return current_active_menu;
}

void rofi_view_set_active ( RofiViewState *state )
{
    g_assert ( ( current_active_menu == NULL && state != NULL ) || ( current_active_menu != NULL && state == NULL ) );
    current_active_menu = state;
}

void rofi_view_set_selected_line ( RofiViewState *state, unsigned int selected_line )
{
    state->selected_line = selected_line;
    // Find the line.
    state->selected = 0;
    for ( unsigned int i = 0; ( ( state->selected_line ) ) < UINT32_MAX && !state->selected && i < state->filtered_lines; i++ ) {
        if ( state->line_map[i] == ( state->selected_line ) ) {
            state->selected = i;
            break;
        }
    }

    state->update = TRUE;
    xcb_clear_area ( xcb->connection, state->window, 1, 0, 0, 1, 1 );
    xcb_flush ( xcb->connection );
}

void rofi_view_free ( RofiViewState *state )
{
    // Do this here?
    // Wait for final release?
    textbox_free ( state->text );
    textbox_free ( state->prompt_tb );
    textbox_free ( state->case_indicator );
    scrollbar_free ( state->scrollbar );

    for ( unsigned int i = 0; i < state->max_elements; i++ ) {
        textbox_free ( state->boxes[i] );
    }

    g_free ( state->boxes );
    g_free ( state->line_map );
    g_free ( state->distance );
    g_free ( state->lines_not_ascii );
    // Free the switcher boxes.
    // When state is free'ed we should no longer need these.
    if ( config.sidebar_mode == TRUE ) {
        for ( unsigned int j = 0; j < state->num_modi; j++ ) {
            textbox_free ( state->modi[j] );
            state->modi[j] = NULL;
        }
        g_free ( state->modi );
        state->num_modi = 0;
    }
    g_free ( state );
}

MenuReturn rofi_view_get_return_value ( const RofiViewState *state )
{
    return state->retv;
}

unsigned int rofi_view_get_selected_line ( const RofiViewState *state )
{
    return state->selected_line;
}

unsigned int rofi_view_get_next_position ( const RofiViewState *state )
{
    unsigned int next_pos = state->selected_line;
    if ( ( state->selected + 1 ) < state->num_lines ) {
        ( next_pos ) = state->line_map[state->selected + 1];
    }
    return next_pos;
}

unsigned int rofi_view_get_completed ( const RofiViewState *state )
{
    return state->quit;
}

static void rofi_view_resize ( RofiViewState *state )
{
    if ( ( state->menu_flags & MENU_ERROR_DIALOG ) == MENU_ERROR_DIALOG ) {
        // Resize of error dialog.
        int entrybox_width = state->w - ( 2 * ( state->border ) );
        textbox_moveresize ( state->text, state->text->widget.x, state->text->widget.y, entrybox_width, state->line_height );
        state->rchanged = TRUE;
        state->update   = TRUE;
        return;
    }
    unsigned int sbw = config.line_margin + 8;
    widget_move ( WIDGET ( state->scrollbar ), state->w - state->border - sbw, state->top_offset );
    if ( config.sidebar_mode == TRUE ) {
        int width = ( state->w - ( 2 * ( state->border ) + ( state->num_modi - 1 ) * config.line_margin ) ) / state->num_modi;
        for ( unsigned int j = 0; j < state->num_modi; j++ ) {
            textbox_moveresize ( state->modi[j],
                                 state->border + j * ( width + config.line_margin ), state->h - state->line_height - state->border,
                                 width, state->line_height );
            textbox_draw ( state->modi[j], CacheState.draw );
        }
    }
    int entrybox_width = state->w - ( 2 * ( state->border ) );
    int offset         = 0;
    int width          = textbox_get_width ( state->case_indicator );
    entrybox_width -= width + textbox_get_width ( state->prompt_tb );
    offset          = width;
    textbox_moveresize ( state->text, state->text->widget.x, state->text->widget.y, entrybox_width, state->line_height );
    widget_move ( WIDGET ( state->case_indicator ), state->w - state->border - offset, state->border );
    /**
     * Resize in Height
     */
    if ( state->num_lines > 0 ) {
        unsigned int last_length    = state->max_elements;
        int          element_height = state->line_height * config.element_height + config.line_margin;
        // Calculated new number of boxes.
        int          h = ( state->h - state->top_offset - config.padding );
        if ( config.sidebar_mode == TRUE ) {
            h -= state->line_height + config.line_margin;
        }
        if ( h < 0 ) {
            fprintf ( stderr, "Current padding %u (on each side) does not fit within visible window %u.\n", config.padding, state->h );
            h = ( state->h - state->top_offset - state->h / 3 );
            if ( config.sidebar_mode == TRUE ) {
                h -= state->line_height + config.line_margin;
            }
        }
        state->max_rows     = MAX ( 1, ( h / element_height ) );
        state->menu_lines   = state->max_rows;
        state->max_elements = state->max_rows * config.menu_columns;
        // Free boxes no longer needed.
        for ( unsigned int i = state->max_elements; i < last_length; i++ ) {
            textbox_free ( state->boxes[i] );
        }
        // resize array.
        state->boxes = g_realloc ( state->boxes, state->max_elements * sizeof ( textbox* ) );

        int y_offset = state->top_offset;
        int x_offset = state->border;
        // Add newly added boxes.
        for ( unsigned int i = last_length; i < state->max_elements; i++ ) {
            state->boxes[i] = textbox_create ( 0, x_offset, y_offset,
                                               state->element_width, element_height, NORMAL, "" );
        }
        scrollbar_resize ( state->scrollbar, -1, ( state->max_rows ) * ( element_height ) - config.line_margin );
    }

    state->rchanged = TRUE;
    state->update   = TRUE;
}

void rofi_view_itterrate ( RofiViewState *state, xcb_generic_event_t *event, xkb_stuff *xkb )
{
    uint8_t type = event->response_type & ~0x80;
    switch ( type )
    {
    case XCB_EXPOSE:
        state->update = TRUE;
        break;
    case XCB_CONFIGURE_NOTIFY:
    {
        xcb_configure_notify_event_t *xce = (xcb_configure_notify_event_t *) event;
        if ( xce->window == state->window ) {
            if ( state->x != xce->x || state->y != xce->y ) {
                state->x      = xce->x;
                state->y      = xce->y;
                state->update = TRUE;
            }
            if ( state->w != xce->width || state->h != xce->height ) {
                state->w = xce->width;
                state->h = xce->height;
                cairo_xcb_surface_set_size ( CacheState.surface, state->w, state->h );
                rofi_view_resize ( state );
            }
        }
        break;
    }
    default:
        state->x11_event_loop ( state, event, xkb );
    }
    rofi_view_update ( state );
}

const char * rofi_view_get_user_input ( const RofiViewState *state )
{
    if ( state->text ) {
        return state->text->text;
    }
    return NULL;
}

/**
 * Create a new, 0 initialized RofiViewState structure.
 *
 * @returns a new 0 initialized RofiViewState
 */
static RofiViewState * __rofi_view_state_create ( void )
{
    return g_malloc0 ( sizeof ( RofiViewState ) );
}

typedef struct _thread_state
{
    RofiViewState *state;
    char          **tokens;
    unsigned int  start;
    unsigned int  stop;
    unsigned int  count;
    GCond         *cond;
    GMutex        *mutex;
    unsigned int  *acount;
    void ( *callback )( struct _thread_state *t, gpointer data );
}thread_state;

static void filter_elements ( thread_state *t, G_GNUC_UNUSED gpointer user_data )
{
    // input changed
    for ( unsigned int i = t->start; i < t->stop; i++ ) {
        int match = mode_token_match ( t->state->sw, t->tokens, t->state->lines_not_ascii[i],
                                       config.case_sensitive, i );
        // If each token was matched, add it to list.
        if ( match ) {
            t->state->line_map[t->start + t->count] = i;
            if ( config.levenshtein_sort ) {
                // This is inefficient, need to fix it.
                char * str = mode_get_completion ( t->state->sw, i );
                t->state->distance[i] = levenshtein ( t->state->text->text, str );
                g_free ( str );
            }
            t->count++;
        }
    }
    g_mutex_lock ( t->mutex );
    ( *( t->acount ) )--;
    g_cond_signal ( t->cond );
    g_mutex_unlock ( t->mutex );
}
static void check_is_ascii ( thread_state *t, G_GNUC_UNUSED gpointer user_data )
{
    for ( unsigned int i = t->start; i < t->stop; i++ ) {
        t->state->lines_not_ascii[i] = mode_is_not_ascii ( t->state->sw, i );
    }
    g_mutex_lock ( t->mutex );
    ( *( t->acount ) )--;
    g_cond_signal ( t->cond );
    g_mutex_unlock ( t->mutex );
}

static xcb_window_t __create_window ( MenuFlags menu_flags )
{
    uint32_t     selmask  = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t     selval[] = {
        0,
        0,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_BUTTON_1_MOTION,
        map
    };

    xcb_window_t box = xcb_generate_id ( xcb->connection );
    xcb_create_window ( xcb->connection, depth->depth, box, xcb_stuff_get_root_window ( xcb ),
                        0, 0, 200, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                        visual->visual_id, selmask, selval );

    CacheState.surface = cairo_xcb_surface_create ( xcb->connection, box, visual, 200, 100 );
    // Create a drawable.
    CacheState.draw = cairo_create ( CacheState.surface );
    g_assert ( CacheState.draw != NULL );
    cairo_set_operator ( CacheState.draw, CAIRO_OPERATOR_SOURCE );

    // Set up pango context.
    cairo_font_options_t *fo = cairo_font_options_create ();
    // Take font description from xlib surface
    cairo_surface_get_font_options ( CacheState.surface, fo );
    PangoContext *p = pango_cairo_create_context ( CacheState.draw );
    // Set the font options from the xlib surface
    pango_cairo_context_set_font_options ( p, fo );
    // Setup dpi
    if ( config.dpi > 0 ) {
        PangoFontMap *font_map = pango_cairo_font_map_get_default ();
        pango_cairo_font_map_set_resolution ( (PangoCairoFontMap *) font_map, (double) config.dpi );
    }
    // Setup font.
    if ( config.menu_font ) {
        PangoFontDescription *pfd = pango_font_description_from_string ( config.menu_font );
        pango_context_set_font_description ( p, pfd );
        pango_font_description_free ( pfd );
    }
    // Tell textbox to use this context.
    textbox_set_pango_context ( p );
    // cleanup
    g_object_unref ( p );
    cairo_font_options_destroy ( fo );

    // // make it an unmanaged window
    if ( ( ( menu_flags & MENU_NORMAL_WINDOW ) == 0 ) ) {
        window_set_atom_prop ( box, xcb->ewmh._NET_WM_STATE, &( xcb->ewmh._NET_WM_STATE_ABOVE ), 1 );
        uint32_t values[] = { 1 };
        xcb_change_window_attributes ( xcb->connection, box, XCB_CW_OVERRIDE_REDIRECT, values );
    }
    else{
        window_set_atom_prop ( box, xcb->ewmh._NET_WM_WINDOW_TYPE, &( xcb->ewmh._NET_WM_WINDOW_TYPE_NORMAL ), 1 );
    }
    if ( config.fullscreen ) {
        xcb_atom_t atoms[] = {
            xcb->ewmh._NET_WM_STATE_FULLSCREEN,
            xcb->ewmh._NET_WM_STATE_ABOVE
        };
        window_set_atom_prop (  box, xcb->ewmh._NET_WM_STATE, atoms, sizeof ( atoms ) / sizeof ( xcb_atom_t ) );
    }

    // Set the WM_NAME
    xcb_change_property ( xcb->connection, XCB_PROP_MODE_REPLACE, box, xcb->ewmh._NET_WM_NAME, xcb->ewmh.UTF8_STRING, 8, 4, "rofi" );
    xcb_change_property ( xcb->connection, XCB_PROP_MODE_REPLACE, box, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 4, "rofi" );

    x11_set_window_opacity ( box, config.window_opacity );
    return box;
}

void rofi_view_call_thread ( gpointer data, gpointer user_data )
{
    thread_state *t = (thread_state *) data;
    t->callback ( t, user_data );
}

/**
 * @param state Internal state of the menu.
 *
 * Calculate the number of rows, columns and elements to display based on the
 * configuration and available data.
 */
static void rofi_view_calculate_rows_columns ( RofiViewState *state )
{
    state->columns      = config.menu_columns;
    state->max_elements = MIN ( state->menu_lines * state->columns, state->num_lines );

    // Calculate the number or rows. We do this by getting the num_lines rounded up to X columns
    // (num elements is better name) then dividing by columns.
    state->max_rows = MIN ( state->menu_lines, (unsigned int) ( ( state->num_lines + ( state->columns - state->num_lines %
                                                                                       state->columns ) %
                                                                  state->columns ) / ( state->columns ) ) );
    // Always have at least one row.
    state->max_rows = MAX ( 1, state->max_rows );

    if ( config.fixed_num_lines == TRUE ) {
        state->max_elements = state->menu_lines * state->columns;
        state->max_rows     = state->menu_lines;
        // If it would fit in one column, only use one column.
        if ( state->num_lines < state->max_elements ) {
            state->columns =
                ( state->num_lines + ( state->max_rows - state->num_lines % state->max_rows ) % state->max_rows ) / state->max_rows;
            state->max_elements = state->menu_lines * state->columns;
        }
        // Sanitize.
        if ( state->columns == 0 ) {
            state->columns = 1;
        }
    }
}

/**
 * @param state Internal state of the menu.
 *
 * Calculate the width of the window and the width of an element.
 */
static void rofi_view_calculate_window_and_element_width ( RofiViewState *state )
{
    if ( config.fullscreen ) {
        state->w = state->mon.w;
    }
    else if ( config.menu_width < 0 ) {
        double fw = textbox_get_estimated_char_width ( );
        state->w  = -( fw * config.menu_width );
        state->w += 2 * state->border + 4; // 4 = 2*SIDE_MARGIN
    }
    else{
        // Calculate as float to stop silly, big rounding down errors.
        state->w = config.menu_width < 101 ? ( state->mon.w / 100.0f ) * ( float ) config.menu_width : config.menu_width;
    }

    if ( state->columns > 0 ) {
        state->element_width = state->w - ( 2 * ( state->border ) );
        // Divide by the # columns
        state->element_width = ( state->element_width - ( state->columns - 1 ) * config.line_margin ) / state->columns;
    }
}

/**
 * Nav helper functions, to avoid duplicate code.
 */
/**
 * @param state The current RofiViewState
 *
 * Move the selection one page down.
 * - No wrap around.
 * - Clip at top/bottom
 */
inline static void rofi_view_nav_page_next ( RofiViewState *state )
{
    // If no lines, do nothing.
    if ( state->filtered_lines == 0 ) {
        return;
    }
    state->selected += ( state->max_elements );
    if ( state->selected >= state->filtered_lines ) {
        state->selected = state->filtered_lines - 1;
    }
    state->update = TRUE;
}

/**
 * @param state The current RofiViewState
 *
 * Move the selection one page up.
 * - No wrap around.
 * - Clip at top/bottom
 */
inline static void rofi_view_nav_page_prev ( RofiViewState * state )
{
    if ( state->selected < state->max_elements ) {
        state->selected = 0;
    }
    else{
        state->selected -= ( state->max_elements );
    }
    state->update = TRUE;
}
/**
 * @param state The current RofiViewState
 *
 * Move the selection one column to the right.
 * - No wrap around.
 * - Do not move to top row when at start.
 */
inline static void rofi_view_nav_right ( RofiViewState *state )
{
    // If no lines, do nothing.
    if ( state->filtered_lines == 0 ) {
        return;
    }
    if ( ( state->selected + state->max_rows ) < state->filtered_lines ) {
        state->selected += state->max_rows;
        state->update    = TRUE;
    }
    else if ( state->selected < ( state->filtered_lines - 1 ) ) {
        // We do not want to move to last item, UNLESS the last column is only
        // partially filled, then we still want to move column and select last entry.
        // First check the column we are currently in.
        int col = state->selected / state->max_rows;
        // Check total number of columns.
        int ncol = state->filtered_lines / state->max_rows;
        // If there is an extra column, move.
        if ( col != ncol ) {
            state->selected = state->filtered_lines - 1;
            state->update   = TRUE;
        }
    }
}

/**
 * @param state The current RofiViewState
 *
 * Move the selection one column to the left.
 * - No wrap around.
 */
inline static void rofi_view_nav_left ( RofiViewState *state )
{
    if ( state->selected >= state->max_rows ) {
        state->selected -= state->max_rows;
        state->update    = TRUE;
    }
}

/**
 * @param state The current RofiViewState
 *
 * Move the selection one row up.
 * - Wrap around.
 */
inline static void rofi_view_nav_up ( RofiViewState *state )
{
    // Wrap around.
    if ( state->selected == 0 ) {
        state->selected = state->filtered_lines;
    }

    if ( state->selected > 0 ) {
        state->selected--;
    }
    state->update = TRUE;
}

/**
 * @param state The current RofiViewState
 *
 * Move the selection one row down.
 * - Wrap around.
 */
inline static void rofi_view_nav_down ( RofiViewState *state )
{
    // If no lines, do nothing.
    if ( state->filtered_lines == 0 ) {
        return;
    }
    state->selected = state->selected < state->filtered_lines - 1 ? MIN ( state->filtered_lines - 1, state->selected + 1 ) : 0;
    state->update   = TRUE;
}

/**
 * @param state The current RofiViewState
 *
 * Move the selection to first row.
 */
inline static void rofi_view_nav_first ( RofiViewState * state )
{
    state->selected = 0;
    state->update   = TRUE;
}

/**
 * @param state The current RofiViewState
 *
 * Move the selection to last row.
 */
inline static void rofi_view_nav_last ( RofiViewState * state )
{
    // If no lines, do nothing.
    if ( state->filtered_lines == 0 ) {
        return;
    }
    state->selected = state->filtered_lines - 1;
    state->update   = TRUE;
}

static unsigned int rofi_scroll_per_page ( RofiViewState * state )
{
    int offset = 0;

    // selected row is always visible.
    // If selected is visible do not scroll.
    if ( ( ( state->selected - ( state->last_offset ) ) < ( state->max_elements ) ) && ( state->selected >= ( state->last_offset ) ) ) {
        offset = state->last_offset;
    }
    else{
        // Do paginating
        int page = ( state->max_elements > 0 ) ? ( state->selected / state->max_elements ) : 0;
        offset             = page * state->max_elements;
        state->last_offset = offset;
        if ( page != state->cur_page ) {
            state->cur_page = page;
            state->rchanged = TRUE;
        }
        // Set the position
        scrollbar_set_handle ( state->scrollbar, page * state->max_elements );
    }
    return offset;
}

static unsigned int rofi_scroll_continious ( RofiViewState * state )
{
    unsigned int middle = ( state->menu_lines - ( ( state->menu_lines & 1 ) == 0 ) ) / 2;
    unsigned int offset = 0;
    if ( state->selected > middle ) {
        if ( state->selected < ( state->filtered_lines - ( state->menu_lines - middle ) ) ) {
            offset = state->selected - middle;
        }
        // Don't go below zero.
        else if ( state->filtered_lines > state->menu_lines ) {
            offset = state->filtered_lines - state->menu_lines;
        }
    }
    state->rchanged = TRUE;
    scrollbar_set_handle ( state->scrollbar, offset );
    return offset;
}

static void rofi_view_draw ( RofiViewState *state, cairo_t *d )
{
    unsigned int i, offset = 0;
    if ( config.scroll_method == 1 ) {
        offset = rofi_scroll_continious ( state );
    }
    else {
        offset = rofi_scroll_per_page ( state );
    }
    // Re calculate the boxes and sizes, see if we can move this in the menu_calc*rowscolumns
    // Get number of remaining lines to display.
    unsigned int a_lines = MIN ( ( state->filtered_lines - offset ), state->max_elements );

    // Calculate number of columns
    unsigned int columns = ( a_lines + ( state->max_rows - a_lines % state->max_rows ) % state->max_rows ) / state->max_rows;
    columns = MIN ( columns, state->columns );

    // Update the handle length.
    // Calculate number of visible rows.
    unsigned int max_elements = MIN ( a_lines, state->max_rows * columns );

    scrollbar_set_handle_length ( state->scrollbar, columns * state->max_rows );
    scrollbar_draw ( state->scrollbar, d );
    // Element width.
    unsigned int element_width = state->w - ( 2 * ( state->border ) );
    if ( state->scrollbar != NULL ) {
        element_width -= state->scrollbar->widget.w;
    }
    if ( columns > 0 ) {
        element_width = ( element_width - ( columns - 1 ) * config.line_margin ) / columns;
    }

    int element_height = state->line_height * config.element_height;
    int y_offset       = state->top_offset;
    int x_offset       = state->border;

    if ( state->rchanged ) {
        // Move, resize visible boxes and show them.
        for ( i = 0; i < max_elements && ( i + offset ) < state->filtered_lines; i++ ) {
            unsigned int ex = ( ( i ) / state->max_rows ) * ( element_width + config.line_margin );
            unsigned int ey = ( ( i ) % state->max_rows ) * ( element_height + config.line_margin );
            // Move it around.
            textbox_moveresize ( state->boxes[i], ex + x_offset, ey + y_offset, element_width, element_height );
            {
                TextBoxFontType type   = ( ( ( i % state->max_rows ) & 1 ) == 0 ) ? NORMAL : ALT;
                int             fstate = 0;
                char            *text  = mode_get_display_value ( state->sw, state->line_map[i + offset], &fstate, TRUE );
                TextBoxFontType tbft   = fstate | ( ( i + offset ) == state->selected ? HIGHLIGHT : type );
                textbox_font ( state->boxes[i], tbft );
                textbox_text ( state->boxes[i], text );
                g_free ( text );
            }
            textbox_draw ( state->boxes[i], d );
        }
        state->rchanged = FALSE;
    }
    else{
        // Only do basic redrawing + highlight of row.
        for ( i = 0; i < max_elements && ( i + offset ) < state->filtered_lines; i++ ) {
            TextBoxFontType type   = ( ( ( i % state->max_rows ) & 1 ) == 0 ) ? NORMAL : ALT;
            int             fstate = 0;
            mode_get_display_value ( state->sw, state->line_map[i + offset], &fstate, FALSE );
            TextBoxFontType tbft = fstate | ( ( i + offset ) == state->selected ? HIGHLIGHT : type );
            textbox_font ( state->boxes[i], tbft );
            textbox_draw ( state->boxes[i], d );
        }
    }
}

void rofi_view_update ( RofiViewState *state )
{
    if ( !state->update ) {
        return;
    }
    TICK ();
    cairo_surface_t * surf = cairo_image_surface_create ( CAIRO_FORMAT_ARGB32, state->w, state->h );
    cairo_t         *d     = cairo_create ( surf );
    cairo_set_operator ( d, CAIRO_OPERATOR_SOURCE );
    if ( config.fake_transparency ) {
        if ( CacheState.fake_bg != NULL ) {
            cairo_set_source_surface ( d, CacheState.fake_bg,
                                       -(double) ( state->x - state->mon.x ),
                                       -(double) ( state->y - state->mon.y ) );
            cairo_paint ( d );
            cairo_set_operator ( d, CAIRO_OPERATOR_OVER );
            color_background ( d );
            cairo_paint ( d );
        }
    }
    else {
        // Paint the background.
        color_background ( d );
        cairo_paint ( d );
    }
    TICK_N ( "Background" );
    color_border ( d );

    if ( config.menu_bw > 0 ) {
        cairo_save ( d );
        cairo_set_line_width ( d, config.menu_bw );
        cairo_rectangle ( d,
                          config.menu_bw / 2.0,
                          config.menu_bw / 2.0,
                          state->w - config.menu_bw,
                          state->h - config.menu_bw );
        cairo_stroke ( d );
        cairo_restore ( d );
    }

    // Always paint as overlay over the background.
    cairo_set_operator ( d, CAIRO_OPERATOR_OVER );
    if ( state->max_elements > 0 ) {
        rofi_view_draw ( state, d );
    }
    if ( state->prompt_tb ) {
        textbox_draw ( state->prompt_tb, d );
    }
    if ( state->text ) {
        textbox_draw ( state->text, d );
    }
    if ( state->case_indicator ) {
        textbox_draw ( state->case_indicator, d );
    }
    if ( state->message_tb ) {
        textbox_draw ( state->message_tb, d );
    }
    color_separator ( d );

    if ( strcmp ( config.separator_style, separator_style_none ) ) {
        if ( strcmp ( config.separator_style, separator_style_dash ) == 0 ) {
            const double dashes[1] = { 4 };
            cairo_set_dash ( d, dashes, 1, 0.0 );
        }
        cairo_move_to ( d, state->border, state->line_height + ( state->border ) * 1 + config.line_margin + 1 );
        cairo_line_to ( d, state->w - state->border, state->line_height + ( state->border ) * 1 + config.line_margin + 1 );
        cairo_stroke ( d );
        if ( state->message_tb ) {
            cairo_move_to ( d, state->border, state->top_offset - ( config.line_margin ) - 1 );
            cairo_line_to ( d, state->w - state->border, state->top_offset - ( config.line_margin ) - 1 );
            cairo_stroke ( d );
        }

        if ( config.sidebar_mode == TRUE ) {
            cairo_move_to ( d, state->border, state->h - state->line_height - ( state->border ) * 1 - 1 - config.line_margin );
            cairo_line_to ( d, state->w - state->border, state->h - state->line_height - ( state->border ) * 1 - 1 - config.line_margin );
            cairo_stroke ( d );
        }
    }
    if ( config.sidebar_mode == TRUE ) {
        for ( unsigned int j = 0; j < state->num_modi; j++ ) {
            if ( state->modi[j] != NULL ) {
                textbox_draw ( state->modi[j], d );
            }
        }
    }
    state->update = FALSE;

    // Draw to actual window.
    cairo_set_source_surface ( CacheState.draw, surf, 0, 0 );
    cairo_paint ( CacheState.draw );
    // Cleanup
    cairo_destroy ( d );
    cairo_surface_destroy ( surf );

    // Flush the surface.
    cairo_surface_flush ( CacheState.surface );
    xcb_flush ( xcb->connection );
    TICK ();
}

/**
 * @param state Internal state of the menu.
 * @param xse   X selection event.
 *
 * Handle paste event.
 */
static void rofi_view_paste ( RofiViewState *state, xcb_selection_notify_event_t *xse )
{
    if ( xse->property == XCB_ATOM_NONE ) {
        fprintf ( stderr, "Failed to convert selection\n" );
    }
    else if ( xse->property == xcb->ewmh.UTF8_STRING ) {
        gchar *text = window_get_text_prop ( state->window, xcb->ewmh.UTF8_STRING );
        if ( text != NULL && text[0] != '\0' ) {
            unsigned int dl = strlen ( text );
            // Strip new line
            while ( dl > 0 && text[dl] == '\n' ) {
                text[dl] = '\0';
                dl--;
            }
            // Insert string move cursor.
            textbox_insert ( state->text, state->text->cursor, text, dl );
            textbox_cursor ( state->text, state->text->cursor + dl  );
            // Force a redraw and refiltering of the text.
            state->update   = TRUE;
            state->refilter = TRUE;
        }
        g_free ( text );
    }
    else {
        fprintf ( stderr, "Failed\n" );
    }
}

/**
 * @param state Internal state of the menu.
 * @param key the Key being pressed.
 * @param modstate the modifier state.
 *
 * Keyboard navigation through the elements.
 */
static int rofi_view_keyboard_navigation ( RofiViewState *state, xkb_keysym_t key, unsigned int modstate )
{
    // pressing one of the global key bindings closes the switcher. This allows fast closing of the
    // menu if an item is not selected
    if ( abe_test_action ( CANCEL, modstate, key ) ) {
        state->retv = MENU_CANCEL;
        state->quit = TRUE;
        return 1;
    }
    // Up, Ctrl-p or Shift-Tab
    else if ( abe_test_action ( ROW_UP, modstate, key ) ) {
        rofi_view_nav_up ( state );
        return 1;
    }
    else if ( abe_test_action ( ROW_TAB, modstate, key ) ) {
        if ( state->filtered_lines == 1 ) {
            state->retv              = MENU_OK;
            ( state->selected_line ) = state->line_map[state->selected];
            state->quit              = 1;
            return 1;
        }

        // Double tab!
        if ( state->filtered_lines == 0 && key == state->prev_key ) {
            state->retv              = MENU_NEXT;
            ( state->selected_line ) = 0;
            state->quit              = TRUE;
        }
        else{
            rofi_view_nav_down ( state );
        }
        return 1;
    }
    // Down, Ctrl-n
    else if ( abe_test_action ( ROW_DOWN, modstate, key ) ) {
        rofi_view_nav_down ( state );
        return 1;
    }
    else if ( abe_test_action ( ROW_LEFT, modstate, key ) ) {
        rofi_view_nav_left ( state );
        return 1;
    }
    else if ( abe_test_action ( ROW_RIGHT, modstate, key ) ) {
        rofi_view_nav_right ( state );
        return 1;
    }
    else if ( abe_test_action ( PAGE_PREV, modstate, key ) ) {
        rofi_view_nav_page_prev ( state );
        return 1;
    }
    else if ( abe_test_action ( PAGE_NEXT, modstate, key ) ) {
        rofi_view_nav_page_next ( state );
        return 1;
    }
    else if  ( abe_test_action ( ROW_FIRST, modstate, key ) ) {
        rofi_view_nav_first ( state );
        return 1;
    }
    else if ( abe_test_action ( ROW_LAST, modstate, key ) ) {
        rofi_view_nav_last ( state );
        return 1;
    }
    else if ( abe_test_action ( ROW_SELECT, modstate, key ) ) {
        // If a valid item is selected, return that..
        if ( state->selected < state->filtered_lines ) {
            char *str = mode_get_completion ( state->sw, state->line_map[state->selected] );
            textbox_text ( state->text, str );
            g_free ( str );
            textbox_cursor_end ( state->text );
            state->update   = TRUE;
            state->refilter = TRUE;
        }
        return 1;
    }
    state->prev_key = key;
    return 0;
}

static void rofi_view_mouse_navigation ( RofiViewState *state, xcb_button_press_event_t *xbe )
{
    // Scroll event
    if ( xbe->detail > 3 ) {
        if ( xbe->detail == 4 ) {
            rofi_view_nav_up ( state );
        }
        else if ( xbe->detail == 5 ) {
            rofi_view_nav_down ( state );
        }
        else if ( xbe->detail == 6 ) {
            rofi_view_nav_left ( state );
        }
        else if ( xbe->detail == 7 ) {
            rofi_view_nav_right ( state );
        }
        return;
    }
    else {
        if ( state->scrollbar && widget_intersect ( &( state->scrollbar->widget ), xbe->event_x, xbe->event_y ) ) {
            state->selected = scrollbar_clicked ( state->scrollbar, xbe->event_y );
            state->update   = TRUE;
            return;
        }
        for ( unsigned int i = 0; config.sidebar_mode == TRUE && i < state->num_modi; i++ ) {
            if ( widget_intersect ( &( state->modi[i]->widget ), xbe->event_x, xbe->event_y ) ) {
                ( state->selected_line ) = 0;
                state->retv              = MENU_QUICK_SWITCH | ( i & MENU_LOWER_MASK );
                state->quit              = TRUE;
                state->skip_absorb       = TRUE;
                return;
            }
        }
        for ( unsigned int i = 0; i < state->max_elements; i++ ) {
            if ( widget_intersect ( &( state->boxes[i]->widget ), xbe->event_x, xbe->event_y ) ) {
                // Only allow items that are visible to be selected.
                if ( ( state->last_offset + i ) >= state->filtered_lines ) {
                    break;
                }
                //
                state->selected = state->last_offset + i;
                state->update   = TRUE;
                if ( ( xbe->time - state->last_button_press ) < 200 ) {
                    state->retv              = MENU_OK;
                    ( state->selected_line ) = state->line_map[state->selected];
                    // Quit
                    state->quit        = TRUE;
                    state->skip_absorb = TRUE;
                }
                state->last_button_press = xbe->time;
                break;
            }
        }
    }
}
static void rofi_view_refilter ( RofiViewState *state )
{
    TICK_N ( "Filter start" );
    if ( strlen ( state->text->text ) > 0 ) {
        unsigned int j        = 0;
        char         **tokens = tokenize ( state->text->text, config.case_sensitive );
        /**
         * On long lists it can be beneficial to parallelize.
         * If number of threads is 1, no thread is spawn.
         * If number of threads > 1 and there are enough (> 1000) items, spawn jobs for the thread pool.
         * For large lists with 8 threads I see a factor three speedup of the whole function.
         */
        unsigned int nt = MAX ( 1, state->num_lines / 500 );
        thread_state states[nt];
        GCond        cond;
        GMutex       mutex;
        g_mutex_init ( &mutex );
        g_cond_init ( &cond );
        unsigned int count = nt;
        unsigned int steps = ( state->num_lines + nt ) / nt;
        for ( unsigned int i = 0; i < nt; i++ ) {
            states[i].state    = state;
            states[i].tokens   = tokens;
            states[i].start    = i * steps;
            states[i].stop     = MIN ( state->num_lines, ( i + 1 ) * steps );
            states[i].count    = 0;
            states[i].cond     = &cond;
            states[i].mutex    = &mutex;
            states[i].acount   = &count;
            states[i].callback = filter_elements;
            if ( i > 0 ) {
                g_thread_pool_push ( tpool, &states[i], NULL );
            }
        }
        // Run one in this thread.
        filter_elements ( &states[0], NULL );
        // No need to do this with only one thread.
        if ( nt > 1 ) {
            g_mutex_lock ( &mutex );
            while ( count > 0 ) {
                g_cond_wait ( &cond, &mutex );
            }
            g_mutex_unlock ( &mutex );
        }
        g_cond_clear ( &cond );
        g_mutex_clear ( &mutex );
        for ( unsigned int i = 0; i < nt; i++ ) {
            if ( j != states[i].start ) {
                memmove ( &( state->line_map[j] ), &( state->line_map[states[i].start] ), sizeof ( unsigned int ) * ( states[i].count ) );
            }
            j += states[i].count;
        }
        if ( config.levenshtein_sort ) {
            g_qsort_with_data ( state->line_map, j, sizeof ( int ), lev_sort, state->distance );
        }

        // Cleanup + bookkeeping.
        state->filtered_lines = j;
        tokenize_free ( tokens );
    }
    else{
        for ( unsigned int i = 0; i < state->num_lines; i++ ) {
            state->line_map[i] = i;
        }
        state->filtered_lines = state->num_lines;
    }
    if ( state->filtered_lines > 0 ) {
        state->selected = MIN ( state->selected, state->filtered_lines - 1 );
    }
    else {
        state->selected = 0;
    }

    if ( config.auto_select == TRUE && state->filtered_lines == 1 && state->num_lines > 1 ) {
        ( state->selected_line ) = state->line_map[state->selected];
        state->retv              = MENU_OK;
        state->quit              = TRUE;
    }

    scrollbar_set_max_value ( state->scrollbar, state->filtered_lines );
    state->refilter = FALSE;
    state->rchanged = TRUE;
    state->update   = TRUE;
    TICK_N ( "Filter done" );
}
/**
 * @param state The Menu Handle
 *
 * Check if a finalize function is set, and if sets executes it.
 */
void rofi_view_finalize ( RofiViewState *state )
{
    if ( state && state->finalize ) {
        state->finalize ( state );
    }
}
void rofi_view_setup_fake_transparency ( RofiViewState *state )
{
    if ( CacheState.fake_bg == NULL ) {
        cairo_surface_t *s = cairo_xcb_surface_create ( xcb->connection,
                                                        xcb_stuff_get_root_window ( xcb ),
                                                        root_visual,
                                                        xcb->screen->width_in_pixels,
                                                        xcb->screen->height_in_pixels );

        CacheState.fake_bg = cairo_image_surface_create ( CAIRO_FORMAT_ARGB32, state->mon.w, state->mon.h );
        cairo_t *dr = cairo_create ( CacheState.fake_bg );
        cairo_set_source_surface ( dr, s, -state->mon.x, -state->mon.y );
        cairo_paint ( dr );
        cairo_destroy ( dr );
        cairo_surface_destroy ( s );
        TICK_N ( "Fake transparency" );
    }
}

static void rofi_view_mainloop_iter ( RofiViewState *state, xcb_generic_event_t *ev, xkb_stuff *xkb )
{
    switch ( ev->response_type & ~0x80 )
    {
    case XCB_FOCUS_IN:
        if ( ( state->menu_flags & MENU_NORMAL_WINDOW ) == 0 ) {
            take_keyboard ( state->window );
        }
        break;
    case XCB_FOCUS_OUT:
        if ( ( state->menu_flags & MENU_NORMAL_WINDOW ) == 0 ) {
            release_keyboard ( );
        }
        break;
    case XCB_MOTION_NOTIFY:
    {
        xcb_motion_notify_event_t *xme = (xcb_motion_notify_event_t *) ev;
        if ( xme->event_x >= state->scrollbar->widget.x && xme->event_x < ( state->scrollbar->widget.x + state->scrollbar->widget.w ) ) {
            state->selected = scrollbar_clicked ( state->scrollbar, xme->event_y );
            state->update   = TRUE;
        }
        break;
    }
    case XCB_BUTTON_PRESS:
        rofi_view_mouse_navigation ( state, (xcb_button_press_event_t *) ev );
        break;
    // Paste event.
    case XCB_SELECTION_NOTIFY:
        rofi_view_paste ( state, (xcb_selection_notify_event_t *) ev );
        break;
    case XCB_KEY_PRESS:
    {
        xcb_key_press_event_t *xkpe = (xcb_key_press_event_t *) ev;
        xcb_keysym_t          key;
        char                  pad[32];
        int                   len = 0;

        key = xkb_state_key_get_one_sym ( xkb->state, xkpe->detail );

        if ( xkb->compose.state != NULL ) {
            if ( ( key != XKB_KEY_NoSymbol ) && ( xkb_compose_state_feed ( xkb->compose.state, key ) == XKB_COMPOSE_FEED_ACCEPTED ) ) {
                switch ( xkb_compose_state_get_status ( xkb->compose.state ) )
                {
                case XKB_COMPOSE_CANCELLED:
                /* Eat the keysym that cancelled the compose sequence.
                 * This is default behaviour with Xlib */
                case XKB_COMPOSE_COMPOSING:
                    key = XKB_KEY_NoSymbol;
                    break;
                case XKB_COMPOSE_COMPOSED:
                    key = xkb_compose_state_get_one_sym ( xkb->compose.state );
                    len = xkb_compose_state_get_utf8 ( xkb->compose.state, pad, sizeof ( pad ) );
                    break;
                case XKB_COMPOSE_NOTHING:
                    break;
                }
                if ( ( key == XKB_KEY_NoSymbol ) && ( len == 0 ) ) {
                    break;
                }
            }
        }

        if ( len == 0 ) {
            len = xkb_state_key_get_utf8 ( xkb->state, xkpe->detail, pad, sizeof ( pad ) );
        }

        unsigned int modstate = x11_canonalize_mask ( xkpe->state );

        if ( key != XKB_KEY_NoSymbol ) {
            // Handling of paste
            if ( abe_test_action ( PASTE_PRIMARY, modstate, key ) ) {
                xcb_convert_selection ( xcb->connection, state->window, XCB_ATOM_PRIMARY,
                                        xcb->ewmh.UTF8_STRING, xcb->ewmh.UTF8_STRING, XCB_CURRENT_TIME );
                xcb_flush ( xcb->connection );
            }
            else if ( abe_test_action ( PASTE_SECONDARY, modstate, key ) ) {
                xcb_convert_selection ( xcb->connection, state->window, XCB_ATOM_SECONDARY,
                                        xcb->ewmh.UTF8_STRING, xcb->ewmh.UTF8_STRING, XCB_CURRENT_TIME );
                xcb_flush ( xcb->connection );
            }
            if ( abe_test_action ( SCREENSHOT, modstate, key ) ) {
                menu_capture_screenshot ( );
                break;
            }
            if ( abe_test_action ( TOGGLE_SORT, modstate, key ) ) {
                config.levenshtein_sort = !config.levenshtein_sort;
                state->refilter         = TRUE;
                state->update           = TRUE;
                textbox_text ( state->case_indicator, get_matching_state () );
                break;
            }
            else if ( abe_test_action ( MODE_PREVIOUS, modstate, key ) ) {
                state->retv              = MENU_PREVIOUS;
                ( state->selected_line ) = 0;
                state->quit              = TRUE;
                break;
            }
            // Menu navigation.
            else if ( abe_test_action ( MODE_NEXT, modstate, key ) ) {
                state->retv              = MENU_NEXT;
                ( state->selected_line ) = 0;
                state->quit              = TRUE;
                break;
            }
            // Toggle case sensitivity.
            else if ( abe_test_action ( TOGGLE_CASE_SENSITIVITY, modstate, key ) ) {
                config.case_sensitive    = !config.case_sensitive;
                ( state->selected_line ) = 0;
                state->refilter          = TRUE;
                state->update            = TRUE;
                textbox_text ( state->case_indicator, get_matching_state () );
                break;
            }
            // Special delete entry command.
            else if ( abe_test_action ( DELETE_ENTRY, modstate, key ) ) {
                if ( state->selected < state->filtered_lines ) {
                    ( state->selected_line ) = state->line_map[state->selected];
                    state->retv              = MENU_ENTRY_DELETE;
                    state->quit              = TRUE;
                    break;
                }
            }
            for ( unsigned int a = CUSTOM_1; a <= CUSTOM_19; a++ ) {
                if ( abe_test_action ( a, modstate, key ) ) {
                    state->selected_line = UINT32_MAX;
                    if ( state->selected < state->filtered_lines ) {
                        ( state->selected_line ) = state->line_map[state->selected];
                    }
                    state->retv = MENU_QUICK_SWITCH | ( ( a - CUSTOM_1 ) & MENU_LOWER_MASK );
                    state->quit = TRUE;
                    break;
                }
            }
            if ( rofi_view_keyboard_navigation ( state, key, modstate ) ) {
                break;
            }
        }
        {
            // Skip if we detected key before.
            if ( state->quit ) {
                break;
            }

            int rc = textbox_keypress ( state->text, pad, len, modstate, key );
            // Row is accepted.
            if ( rc < 0 ) {
                // If a valid item is selected, return that..
                state->selected_line = UINT32_MAX;
                if ( state->selected < state->filtered_lines ) {
                    xcb->last_timestamp      = xkpe->time;
                    ( state->selected_line ) = state->line_map[state->selected];
                    state->retv              = MENU_OK;
                }
                else{
                    // Nothing entered and nothing selected.
                    state->retv = MENU_CUSTOM_INPUT;
                }
                if ( rc == -2 ) {
                    state->retv |= MENU_CUSTOM_ACTION;
                }

                state->quit = TRUE;
            }
            // Key press is handled by entry box.
            else if ( rc == 1 ) {
                state->refilter = TRUE;
                state->update   = TRUE;
            }
            else if (  rc == 2 ) {
                // redraw.
                state->update = TRUE;
            }
        }
        break;
    }
    }
    // Update if requested.
    if ( state->refilter ) {
        rofi_view_refilter ( state );
    }
    rofi_view_update ( state );
}
RofiViewState *rofi_view_create ( Mode *sw,
                                  const char *input,
                                  char *prompt,
                                  const char *message,
                                  MenuFlags menu_flags,
                                  void ( *finalize )( RofiViewState *state ) )
{
    TICK ();
    RofiViewState *state = __rofi_view_state_create ();
    state->menu_flags    = menu_flags;
    state->sw            = sw;
    state->selected_line = UINT32_MAX;
    state->retv          = MENU_CANCEL;
    state->distance      = NULL;
    state->quit          = FALSE;
    state->skip_absorb   = FALSE;
    //We want to filter on the first run.
    state->refilter       = TRUE;
    state->update         = FALSE;
    state->rchanged       = TRUE;
    state->cur_page       = -1;
    state->border         = config.padding + config.menu_bw;
    state->x11_event_loop = rofi_view_mainloop_iter;
    state->finalize       = finalize;

    // Request the lines to show.
    state->num_lines       = mode_get_num_entries ( sw );
    state->lines_not_ascii = g_malloc0_n ( state->num_lines, sizeof ( int ) );

    // main window isn't explicitly destroyed in case we switch modes. Reusing it prevents flicker
    if ( CacheState.main_window == XCB_WINDOW_NONE ) {
        CacheState.main_window = __create_window ( menu_flags );
        if ( xcb->sncontext != NULL ) {
            sn_launchee_context_setup_window ( xcb->sncontext, state->window );
        }
    }
    state->window = CacheState.main_window;
    // find out which lines contain non-ascii codepoints, so we can be faster in some cases.
    if ( state->num_lines > 0 ) {
        TICK_N ( "Is ASCII start" );
        unsigned int nt = MAX ( 1, state->num_lines / 5000 );
        thread_state states[nt];
        unsigned int steps = ( state->num_lines + nt ) / nt;
        unsigned int count = nt;
        GCond        cond;
        GMutex       mutex;
        g_mutex_init ( &mutex );
        g_cond_init ( &cond );
        for ( unsigned int i = 0; i < nt; i++ ) {
            states[i].state    = state;
            states[i].start    = i * steps;
            states[i].stop     = MIN ( ( i + 1 ) * steps, state->num_lines );
            states[i].acount   = &count;
            states[i].mutex    = &mutex;
            states[i].cond     = &cond;
            states[i].callback = check_is_ascii;
            if ( i > 0 ) {
                g_thread_pool_push ( tpool, &( states[i] ), NULL );
            }
        }
        // Run one in this thread.
        check_is_ascii ( &( states[0] ), NULL );
        // No need to do this with only one thread.
        if ( nt > 1 ) {
            g_mutex_lock ( &mutex );
            while ( count > 0 ) {
                g_cond_wait ( &cond, &mutex );
            }
            g_mutex_unlock ( &mutex );
        }
        g_cond_clear ( &cond );
        g_mutex_clear ( &mutex );
        TICK_N ( "Is ASCII stop" );
    }
    TICK_N ( "Startup notification" );

    // Try to grab the keyboard as early as possible.
    // We grab this using the rootwindow (as dmenu does it).
    // this seems to result in the smallest delay for most people.
    if ( ( menu_flags & MENU_NORMAL_WINDOW ) == 0 ) {
        int has_keyboard = take_keyboard ( xcb_stuff_get_root_window ( xcb ) );

        if ( !has_keyboard ) {
            fprintf ( stderr, "Failed to grab keyboard, even after %d uS.", 500 * 1000 );
            // Break off.
            rofi_view_free ( state );
            return NULL;
        }
    }
    TICK_N ( "Grab keyboard" );
    // Get active monitor size.
    monitor_active ( &( state->mon ) );
    TICK_N ( "Get active monitor" );
    if ( config.fake_transparency ) {
        rofi_view_setup_fake_transparency ( state );
    }

    // we need this at this point so we can get height.
    state->line_height = textbox_get_estimated_char_height ();
    int element_height = state->line_height * config.element_height;
    state->case_indicator = textbox_create ( TB_AUTOWIDTH, ( state->border ), ( state->border ),
                                             0, state->line_height, NORMAL, "*" );
    state->top_offset = state->border * 1 + state->line_height + 2 + config.line_margin * 2;
    // Height of a row.
    if ( config.menu_lines == 0 || config.fullscreen  ) {
        // Autosize it.
        int h = state->mon.h - state->top_offset - config.padding;
        if ( config.sidebar_mode == TRUE ) {
            h -= state->line_height + config.line_margin;
        }
        int r = MAX ( 1, ( h ) / ( element_height + config.line_margin ) );
        state->menu_lines = r;
    }
    else {
        state->menu_lines = config.menu_lines;
    }
    rofi_view_calculate_rows_columns ( state );
    rofi_view_calculate_window_and_element_width ( state );

    // Prompt box.
    state->prompt_tb = textbox_create ( TB_AUTOWIDTH, ( state->border ), ( state->border ),
                                        0, state->line_height, NORMAL, prompt );
    // Entry box
    int          entrybox_width = state->w - ( 2 * ( state->border ) ) - textbox_get_width ( state->prompt_tb )
                                  - textbox_get_width ( state->case_indicator );
    TextboxFlags tfl = TB_EDITABLE;
    tfl        |= ( ( menu_flags & MENU_PASSWORD ) == MENU_PASSWORD ) ? TB_PASSWORD : 0;
    state->text = textbox_create ( tfl,
                                   ( state->border ) + textbox_get_width ( state->prompt_tb ), ( state->border ),
                                   entrybox_width, state->line_height, NORMAL, input );

    // Move indicator to end.
    widget_move ( WIDGET ( state->case_indicator ), state->border + textbox_get_width ( state->prompt_tb ) + entrybox_width,
                  state->border );

    textbox_text ( state->case_indicator, get_matching_state () );
    state->message_tb = NULL;
    if ( message ) {
        state->message_tb = textbox_create ( TB_AUTOHEIGHT | TB_MARKUP | TB_WRAP,
                                             ( state->border ), state->top_offset, state->w - ( 2 * ( state->border ) ),
                                             -1, NORMAL, message );
        state->top_offset += textbox_get_height ( state->message_tb );
        state->top_offset += config.line_margin * 2 + 2;
    }

    // filtered list display
    state->boxes = g_malloc0_n ( state->max_elements, sizeof ( textbox* ) );

    int y_offset = state->top_offset;
    int x_offset = state->border;

    for ( unsigned int i = 0; i < state->max_elements; i++ ) {
        state->boxes[i] = textbox_create ( 0, x_offset, y_offset,
                                           state->element_width, element_height, NORMAL, "" );
    }
    if ( !config.hide_scrollbar ) {
        unsigned int sbw = config.line_margin + config.scrollbar_width;
        state->scrollbar = scrollbar_create ( state->w - state->border - sbw, state->top_offset,
                                              sbw, ( state->max_rows - 1 ) * ( element_height + config.line_margin ) + element_height );
    }

    scrollbar_set_max_value ( state->scrollbar, state->num_lines );
    // filtered list
    state->line_map = g_malloc0_n ( state->num_lines, sizeof ( unsigned int ) );
    state->distance = (int *) g_malloc0_n ( state->num_lines, sizeof ( int ) );

    // resize window vertically to suit
    // Subtract the margin of the last row.
    state->h  = state->top_offset + ( element_height + config.line_margin ) * ( state->max_rows ) - config.line_margin;
    state->h += state->border;
    state->h += 0;
    // Add entry
    if ( config.sidebar_mode == TRUE ) {
        state->h += state->line_height + 2 * config.line_margin + 2;
    }

    // Sidebar or fullscreen mode.
    if ( config.menu_lines == 0 || config.fullscreen ) {
        state->h = state->mon.h;
    }

    // Move the window to the correct x,y position.
    calculate_window_position ( state );

    if ( config.sidebar_mode == TRUE ) {
        state->num_modi = rofi_get_num_enabled_modi ();
        int width = ( state->w - ( 2 * ( state->border ) + ( state->num_modi - 1 ) * config.line_margin ) ) / state->num_modi;
        state->modi = g_malloc0 ( state->num_modi * sizeof ( textbox * ) );
        for ( unsigned int j = 0; j < state->num_modi; j++ ) {
            const Mode * mode = rofi_get_mode ( j );
            state->modi[j] = textbox_create ( TB_CENTER, state->border + j * ( width + config.line_margin ),
                                              state->h - state->line_height - state->border, width, state->line_height,
                                              ( mode == state->sw ) ? HIGHLIGHT : NORMAL, mode_get_name ( mode  ) );
        }
    }
    uint16_t mask   = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    uint32_t vals[] = { state->x, state->y, state->w, state->h };

    // Display it.
    xcb_configure_window ( xcb->connection, state->window, mask, vals );
    cairo_xcb_surface_set_size ( CacheState.surface, state->w, state->h );

    // if grabbing keyboard failed, fall through
    state->selected = 0;

    state->quit   = FALSE;
    state->update = TRUE;
    rofi_view_refilter ( state );

    rofi_view_update ( state );
    xcb_map_window ( xcb->connection, state->window );
    xcb_flush ( xcb->connection );
    if ( xcb->sncontext != NULL ) {
        sn_launchee_context_complete ( xcb->sncontext );
    }
    return state;
}
static void __error_dialog_event_loop ( RofiViewState *state, xcb_generic_event_t *ev, G_GNUC_UNUSED xkb_stuff *xkb )
{
    // Handle event.
    switch ( ev->response_type & ~0x80 )
    {
    // Key press event.
    case XCB_KEY_PRESS:
        state->quit = TRUE;
    }
    rofi_view_update ( state );
}
void process_result_error ( RofiViewState *state );
int rofi_view_error_dialog ( const char *msg, int markup )
{
    RofiViewState *state = __rofi_view_state_create ();
    state->retv           = MENU_CANCEL;
    state->update         = TRUE;
    state->border         = config.padding + config.menu_bw;
    state->x11_event_loop = __error_dialog_event_loop;
    state->finalize       = process_result_error;
    state->menu_flags     = MENU_ERROR_DIALOG;

    // Get active monitor size.
    monitor_active ( &( state->mon ) );
    if ( config.fake_transparency ) {
        rofi_view_setup_fake_transparency ( state );
    }
    // main window isn't explicitly destroyed in case we switch modes. Reusing it prevents flicker
    if ( CacheState.main_window == XCB_WINDOW_NONE ) {
        CacheState.main_window = __create_window ( MENU_NORMAL );
    }
    state->window = CacheState.main_window;

    // Try to grab the keyboard as early as possible.
    // We grab this using the rootwindow (as dmenu does it).
    // this seems to result in the smallest delay for most people.
    int has_keyboard = take_keyboard ( xcb_stuff_get_root_window ( xcb ) );
    if ( !has_keyboard ) {
        fprintf ( stderr, "Failed to grab keyboard, even after %d uS.", 500 * 1000 );
        return FALSE;
    }

    rofi_view_calculate_window_and_element_width ( state );
    state->max_elements = 0;

    state->text = textbox_create ( ( TB_AUTOHEIGHT | TB_WRAP ) + ( ( markup ) ? TB_MARKUP : 0 ),
                                   ( state->border ), ( state->border ),
                                   ( state->w - ( 2 * ( state->border ) ) ), 1, NORMAL, ( msg != NULL ) ? msg : "" );
    state->line_height = textbox_get_height ( state->text );

    // resize window vertically to suit
    state->h = state->line_height + ( state->border ) * 2;

    // Calculte window position.
    calculate_window_position ( state );

    // Move the window to the correct x,y position.
    uint16_t mask   = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    uint32_t vals[] = { state->x, state->y, state->w, state->h };

    xcb_configure_window ( xcb->connection, state->window, mask, vals );
    calculate_window_position ( state );
    cairo_xcb_surface_set_size ( CacheState.surface, state->w, state->h );
    // Display it.
    xcb_map_window ( xcb->connection, state->window );

    if ( xcb->sncontext != NULL ) {
        sn_launchee_context_complete ( xcb->sncontext );
    }

    // Set it has current window.
    rofi_view_set_active ( state );
    return TRUE;
}

void rofi_view_cleanup ()
{
    if ( CacheState.fake_bg ) {
        cairo_surface_destroy ( CacheState.fake_bg );
        CacheState.fake_bg = NULL;
    }
    if ( CacheState.draw ) {
        cairo_destroy ( CacheState.draw );
        CacheState.draw = NULL;
    }
    if ( CacheState.surface ) {
        cairo_surface_destroy ( CacheState.surface );
        CacheState.surface = NULL;
    }
    if ( CacheState.main_window != XCB_WINDOW_NONE ) {
        xcb_unmap_window ( xcb->connection, CacheState.main_window );
        xcb_destroy_window ( xcb->connection, CacheState.main_window );
        CacheState.main_window = XCB_WINDOW_NONE;
    }
    if ( map != XCB_COLORMAP_NONE ) {
        xcb_free_colormap ( xcb->connection, map );
        map = XCB_COLORMAP_NONE;
    }
}
void rofi_view_workers_initialize ( void )
{
    TICK_N ( "Setup Threadpool, start" );
    if ( config.threads == 0 ) {
        config.threads = 1;
        long procs = sysconf ( _SC_NPROCESSORS_CONF );
        if ( procs > 0 ) {
            config.threads = MIN ( procs, 128l );
        }
    }
    // Create thread pool
    GError *error = NULL;
    tpool = g_thread_pool_new ( rofi_view_call_thread, NULL, config.threads, FALSE, &error );
    if ( error == NULL ) {
        // Idle threads should stick around for a max of 60 seconds.
        g_thread_pool_set_max_idle_time ( 60000 );
        // We are allowed to have
        g_thread_pool_set_max_threads ( tpool, config.threads, &error );
    }
    // If error occured during setup of pool, tell user and exit.
    if ( error != NULL ) {
        char *msg = g_strdup_printf ( "Failed to setup thread pool: '%s'", error->message );
        show_error_message ( msg, FALSE );
        g_free ( msg );
        g_error_free ( error );
        exit ( EXIT_FAILURE );
    }
    TICK_N ( "Setup Threadpool, done" );
}
void rofi_view_workers_finalize ( void )
{
    if ( tpool ) {
        g_thread_pool_free ( tpool, TRUE, FALSE );
        tpool = NULL;
    }
}
Mode * rofi_view_get_mode ( RofiViewState *state )
{
    return state->sw;
}

