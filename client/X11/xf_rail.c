/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * X11 RAIL
 *
 * Copyright 2011 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <winpr/cast.h>
#include <winpr/assert.h>
#include <winpr/wlog.h>
#include <winpr/print.h>

#include <freerdp/client/rail.h>

#include "xf_window.h"
#include "xf_rail.h"
#include "xf_utils.h"

#include <freerdp/log.h>
#define TAG CLIENT_TAG("x11")

static const char* error_code_names[] = { "RAIL_EXEC_S_OK",
	                                      "RAIL_EXEC_E_HOOK_NOT_LOADED",
	                                      "RAIL_EXEC_E_DECODE_FAILED",
	                                      "RAIL_EXEC_E_NOT_IN_ALLOWLIST",
	                                      "RAIL_EXEC_E_FILE_NOT_FOUND",
	                                      "RAIL_EXEC_E_FAIL",
	                                      "RAIL_EXEC_E_SESSION_LOCKED" };

#ifdef WITH_DEBUG_RAIL
static const char* movetype_names[] = {
	"(invalid)",        "RAIL_WMSZ_LEFT",       "RAIL_WMSZ_RIGHT",
	"RAIL_WMSZ_TOP",    "RAIL_WMSZ_TOPLEFT",    "RAIL_WMSZ_TOPRIGHT",
	"RAIL_WMSZ_BOTTOM", "RAIL_WMSZ_BOTTOMLEFT", "RAIL_WMSZ_BOTTOMRIGHT",
	"RAIL_WMSZ_MOVE",   "RAIL_WMSZ_KEYMOVE",    "RAIL_WMSZ_KEYSIZE"
};
#endif

struct xf_rail_icon
{
	long* data;
	int length;
};
typedef struct xf_rail_icon xfRailIcon;

struct xf_rail_icon_cache
{
	xfRailIcon* entries;
	UINT32 numCaches;
	UINT32 numCacheEntries;
	xfRailIcon scratch;
};

typedef struct
{
	xfContext* xfc;
	const RECTANGLE_16* rect;
} rail_paint_fn_arg_t;

void xf_rail_enable_remoteapp_mode(xfContext* xfc)
{
	WINPR_ASSERT(xfc);
	if (!xfc->remote_app)
	{
		xfc->remote_app = TRUE;
		xfc->drawable = xf_CreateDummyWindow(xfc);
		xf_DestroyDesktopWindow(xfc, xfc->window);
		xfc->window = NULL;
	}
}

void xf_rail_disable_remoteapp_mode(xfContext* xfc)
{
	WINPR_ASSERT(xfc);
	if (xfc->remote_app)
	{
		xfc->remote_app = FALSE;
		xf_DestroyDummyWindow(xfc, xfc->drawable);
		xf_create_window(xfc);
		xf_create_image(xfc);
	}
}

void xf_rail_send_activate(xfContext* xfc, Window xwindow, BOOL enabled)
{
	RAIL_ACTIVATE_ORDER activate = { 0 };
	xfAppWindow* appWindow = xf_AppWindowFromX11Window(xfc, xwindow);

	if (!appWindow)
		return;

	if (enabled)
		xf_SetWindowStyle(xfc, appWindow, appWindow->dwStyle, appWindow->dwExStyle);

	WINPR_ASSERT(appWindow->windowId <= UINT32_MAX);
	activate.windowId = (UINT32)appWindow->windowId;
	activate.enabled = enabled;
	xfc->rail->ClientActivate(xfc->rail, &activate);
}

BOOL xf_rail_send_client_system_command(xfContext* xfc, UINT64 windowId, UINT16 command)
{
	WINPR_ASSERT(xfc);
	WINPR_ASSERT(xfc->rail);
	WINPR_ASSERT(xfc->rail->ClientSystemCommand);
	if (windowId > UINT32_MAX)
		return FALSE;

	const RAIL_SYSCOMMAND_ORDER syscommand = { .windowId = (UINT32)windowId, .command = command };
	const UINT rc = xfc->rail->ClientSystemCommand(xfc->rail, &syscommand);
	return rc == CHANNEL_RC_OK;
}

/**
 * The position of the X window can become out of sync with the RDP window
 * if the X window is moved locally by the window manager.  In this event
 * send an update to the RDP server informing it of the new window position
 * and size.
 */
void xf_rail_adjust_position(xfContext* xfc, xfAppWindow* appWindow)
{
	RAIL_WINDOW_MOVE_ORDER windowMove = { 0 };

	WINPR_ASSERT(xfc);
	WINPR_ASSERT(appWindow);
	if (!appWindow->is_mapped || appWindow->local_move.state != LMS_NOT_ACTIVE)
		return;

	/* If current window position disagrees with RDP window position, send update to RDP server */
	if (appWindow->x != appWindow->windowOffsetX || appWindow->y != appWindow->windowOffsetY ||
	    appWindow->width != (INT64)appWindow->windowWidth ||
	    appWindow->height != (INT64)appWindow->windowHeight)
	{
		WINPR_ASSERT(appWindow->windowId <= UINT32_MAX);
		windowMove.windowId = (UINT32)appWindow->windowId;
		/*
		 * Calculate new size/position for the rail window(new values for
		 * windowOffsetX/windowOffsetY/windowWidth/windowHeight) on the server
		 */
		const INT16 left = WINPR_ASSERTING_INT_CAST(INT16, appWindow->resizeMarginLeft);
		const INT16 right = WINPR_ASSERTING_INT_CAST(INT16, appWindow->resizeMarginRight);
		const INT16 top = WINPR_ASSERTING_INT_CAST(INT16, appWindow->resizeMarginTop);
		const INT16 bottom = WINPR_ASSERTING_INT_CAST(INT16, appWindow->resizeMarginBottom);
		windowMove.left = WINPR_ASSERTING_INT_CAST(INT16, appWindow->x - left);
		windowMove.top = WINPR_ASSERTING_INT_CAST(INT16, appWindow->y - top);
		windowMove.right = WINPR_ASSERTING_INT_CAST(INT16, appWindow->x + appWindow->width + right);
		windowMove.bottom =
		    WINPR_ASSERTING_INT_CAST(INT16, appWindow->y + appWindow->height + bottom);
		xfc->rail->ClientWindowMove(xfc->rail, &windowMove);
	}
}

void xf_rail_end_local_move(xfContext* xfc, xfAppWindow* appWindow)
{
	int x = 0;
	int y = 0;
	int child_x = 0;
	int child_y = 0;
	unsigned int mask = 0;
	Window root_window = 0;
	Window child_window = 0;

	WINPR_ASSERT(xfc);
	WINPR_ASSERT(appWindow);

	if ((appWindow->local_move.direction == NET_WM_MOVERESIZE_MOVE_KEYBOARD) ||
	    (appWindow->local_move.direction == NET_WM_MOVERESIZE_SIZE_KEYBOARD))
	{
		RAIL_WINDOW_MOVE_ORDER windowMove = { 0 };

		/*
		 * For keyboard moves send and explicit update to RDP server
		 */
		WINPR_ASSERT(appWindow->windowId <= UINT32_MAX);
		windowMove.windowId = (UINT32)appWindow->windowId;
		/*
		 * Calculate new size/position for the rail window(new values for
		 * windowOffsetX/windowOffsetY/windowWidth/windowHeight) on the server
		 *
		 */
		const INT16 left = WINPR_ASSERTING_INT_CAST(INT16, appWindow->resizeMarginLeft);
		const INT16 right = WINPR_ASSERTING_INT_CAST(INT16, appWindow->resizeMarginRight);
		const INT16 top = WINPR_ASSERTING_INT_CAST(INT16, appWindow->resizeMarginTop);
		const INT16 bottom = WINPR_ASSERTING_INT_CAST(INT16, appWindow->resizeMarginBottom);
		const INT16 w = WINPR_ASSERTING_INT_CAST(INT16, appWindow->width + right);
		const INT16 h = WINPR_ASSERTING_INT_CAST(INT16, appWindow->height + bottom);
		windowMove.left = WINPR_ASSERTING_INT_CAST(INT16, appWindow->x - left);
		windowMove.top = WINPR_ASSERTING_INT_CAST(INT16, appWindow->y - top);
		windowMove.right = WINPR_ASSERTING_INT_CAST(INT16, appWindow->x + w); /* In the update to
		           RDP the position is one past the window */
		windowMove.bottom = WINPR_ASSERTING_INT_CAST(INT16, appWindow->y + h);
		xfc->rail->ClientWindowMove(xfc->rail, &windowMove);
	}

	/*
	 * Simulate button up at new position to end the local move (per RDP spec)
	 */
	XQueryPointer(xfc->display, appWindow->handle, &root_window, &child_window, &x, &y, &child_x,
	              &child_y, &mask);

	/* only send the mouse coordinates if not a keyboard move or size */
	if ((appWindow->local_move.direction != NET_WM_MOVERESIZE_MOVE_KEYBOARD) &&
	    (appWindow->local_move.direction != NET_WM_MOVERESIZE_SIZE_KEYBOARD))
	{
		freerdp_client_send_button_event(&xfc->common, FALSE, PTR_FLAGS_BUTTON1, x, y);
	}

	/*
	 * Proactively update the RAIL window dimensions.  There is a race condition where
	 * we can start to receive GDI orders for the new window dimensions before we
	 * receive the RAIL ORDER for the new window size.  This avoids that race condition.
	 */
	appWindow->windowOffsetX = appWindow->x;
	appWindow->windowOffsetY = appWindow->y;
	appWindow->windowWidth = WINPR_ASSERTING_INT_CAST(uint32_t, appWindow->width);
	appWindow->windowHeight = WINPR_ASSERTING_INT_CAST(uint32_t, appWindow->height);
	appWindow->local_move.state = LMS_TERMINATING;
}

BOOL xf_rail_paint_surface(xfContext* xfc, UINT64 windowId, const RECTANGLE_16* rect)
{
	xfAppWindow* appWindow = xf_rail_get_window(xfc, windowId);

	WINPR_ASSERT(rect);

	if (!appWindow)
		return FALSE;

	const RECTANGLE_16 windowRect = {
		.left = WINPR_ASSERTING_INT_CAST(UINT16, MAX(appWindow->x, 0)),
		.top = WINPR_ASSERTING_INT_CAST(UINT16, MAX(appWindow->y, 0)),
		.right = WINPR_ASSERTING_INT_CAST(UINT16, MAX(appWindow->x + appWindow->width, 0)),
		.bottom = WINPR_ASSERTING_INT_CAST(UINT16, MAX(appWindow->y + appWindow->height, 0))
	};

	REGION16 windowInvalidRegion = { 0 };
	region16_init(&windowInvalidRegion);
	region16_union_rect(&windowInvalidRegion, &windowInvalidRegion, &windowRect);
	region16_intersect_rect(&windowInvalidRegion, &windowInvalidRegion, rect);

	if (!region16_is_empty(&windowInvalidRegion))
	{
		const RECTANGLE_16* extents = region16_extents(&windowInvalidRegion);

		const RECTANGLE_16 updateRect = {
			.left = WINPR_ASSERTING_INT_CAST(UINT16, extents->left - appWindow->x),
			.top = WINPR_ASSERTING_INT_CAST(UINT16, extents->top - appWindow->y),
			.right = WINPR_ASSERTING_INT_CAST(UINT16, extents->right - appWindow->x),
			.bottom = WINPR_ASSERTING_INT_CAST(UINT16, extents->bottom - appWindow->y)
		};

		xf_UpdateWindowArea(xfc, appWindow, updateRect.left, updateRect.top,
		                    updateRect.right - updateRect.left, updateRect.bottom - updateRect.top);
	}
	region16_uninit(&windowInvalidRegion);
	return TRUE;
}

static BOOL rail_paint_fn(const void* pvkey, WINPR_ATTR_UNUSED void* value, void* pvarg)
{
	rail_paint_fn_arg_t* arg = pvarg;
	WINPR_ASSERT(pvkey);
	WINPR_ASSERT(arg);

	const UINT64 key = *(const UINT64*)pvkey;
	return xf_rail_paint_surface(arg->xfc, key, arg->rect);
}

BOOL xf_rail_paint(xfContext* xfc, const RECTANGLE_16* rect)
{
	rail_paint_fn_arg_t arg = { .xfc = xfc, .rect = rect };

	WINPR_ASSERT(xfc);
	WINPR_ASSERT(rect);

	if (!xfc->railWindows)
		return TRUE;

	return HashTable_Foreach(xfc->railWindows, rail_paint_fn, &arg);
}

#define window_state_log_style(log, windowState) \
	window_state_log_style_int((log), (windowState), __FILE__, __func__, __LINE__)
static void window_state_log_style_int(wLog* log, const WINDOW_STATE_ORDER* windowState,
                                       const char* file, const char* fkt, size_t line)
{
	const DWORD log_level = WLOG_DEBUG;

	WINPR_ASSERT(log);
	WINPR_ASSERT(windowState);
	if (WLog_IsLevelActive(log, log_level))
	{
		char buffer1[128] = { 0 };
		char buffer2[128] = { 0 };

		window_styles_to_string(windowState->style, buffer1, sizeof(buffer1));
		window_styles_ex_to_string(windowState->extendedStyle, buffer2, sizeof(buffer2));
		WLog_PrintMessage(log, WLOG_MESSAGE_TEXT, log_level, line, file, fkt,
		                  "windowStyle={%s, %s}", buffer1, buffer2);
	}
}

/* RemoteApp Core Protocol Extension */

static BOOL xf_rail_window_common(rdpContext* context, const WINDOW_ORDER_INFO* orderInfo,
                                  const WINDOW_STATE_ORDER* windowState)
{
	xfAppWindow* appWindow = NULL;
	xfContext* xfc = (xfContext*)context;

	WINPR_ASSERT(xfc);
	WINPR_ASSERT(orderInfo);
	WINPR_ASSERT(windowState);

	UINT32 fieldFlags = orderInfo->fieldFlags;
	BOOL position_or_size_updated = FALSE;
	appWindow = xf_rail_get_window(xfc, orderInfo->windowId);

	if (fieldFlags & WINDOW_ORDER_STATE_NEW)
	{
		if (!appWindow)
			appWindow = xf_rail_add_window(xfc, orderInfo->windowId, windowState->windowOffsetX,
			                               windowState->windowOffsetY, windowState->windowWidth,
			                               windowState->windowHeight, 0xFFFFFFFF);

		if (!appWindow)
			return FALSE;

		appWindow->dwStyle = windowState->style;
		appWindow->dwExStyle = windowState->extendedStyle;
		window_state_log_style(xfc->log, windowState);

		/* Ensure window always gets a window title */
		if (fieldFlags & WINDOW_ORDER_FIELD_TITLE)
		{
			union
			{
				WCHAR* wc;
				BYTE* b;
			} cnv;
			char* title = NULL;

			cnv.b = windowState->titleInfo.string;
			if (windowState->titleInfo.length == 0)
			{
				if (!(title = _strdup("")))
				{
					WLog_ERR(TAG, "failed to duplicate empty window title string");
					/* error handled below */
				}
			}
			else if (!(title = ConvertWCharNToUtf8Alloc(
			               cnv.wc, windowState->titleInfo.length / sizeof(WCHAR), NULL)))
			{
				WLog_ERR(TAG, "failed to convert window title");
				/* error handled below */
			}

			appWindow->title = title;
		}
		else
		{
			if (!(appWindow->title = _strdup("RdpRailWindow")))
				WLog_ERR(TAG, "failed to duplicate default window title string");
		}

		if (!appWindow->title)
		{
			free(appWindow);
			return FALSE;
		}

		xf_AppWindowInit(xfc, appWindow);
	}

	if (!appWindow)
		return FALSE;

	/* Keep track of any position/size update so that we can force a refresh of the window */
	if ((fieldFlags & WINDOW_ORDER_FIELD_WND_OFFSET) ||
	    (fieldFlags & WINDOW_ORDER_FIELD_WND_SIZE) ||
	    (fieldFlags & WINDOW_ORDER_FIELD_CLIENT_AREA_OFFSET) ||
	    (fieldFlags & WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE) ||
	    (fieldFlags & WINDOW_ORDER_FIELD_WND_CLIENT_DELTA) ||
	    (fieldFlags & WINDOW_ORDER_FIELD_VIS_OFFSET) ||
	    (fieldFlags & WINDOW_ORDER_FIELD_VISIBILITY))
	{
		position_or_size_updated = TRUE;
	}

	/* Update Parameters */

	if (fieldFlags & WINDOW_ORDER_FIELD_WND_OFFSET)
	{
		appWindow->windowOffsetX = windowState->windowOffsetX;
		appWindow->windowOffsetY = windowState->windowOffsetY;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_WND_SIZE)
	{
		appWindow->windowWidth = windowState->windowWidth;
		appWindow->windowHeight = windowState->windowHeight;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_RESIZE_MARGIN_X)
	{
		appWindow->resizeMarginLeft = windowState->resizeMarginLeft;
		appWindow->resizeMarginRight = windowState->resizeMarginRight;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_RESIZE_MARGIN_Y)
	{
		appWindow->resizeMarginTop = windowState->resizeMarginTop;
		appWindow->resizeMarginBottom = windowState->resizeMarginBottom;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_OWNER)
	{
		appWindow->ownerWindowId = windowState->ownerWindowId;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_STYLE)
	{
		appWindow->dwStyle = windowState->style;
		appWindow->dwExStyle = windowState->extendedStyle;
		window_state_log_style(xfc->log, windowState);
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_SHOW)
	{
		appWindow->showState = windowState->showState;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_TITLE)
	{
		char* title = NULL;
		union
		{
			WCHAR* wc;
			BYTE* b;
		} cnv;

		cnv.b = windowState->titleInfo.string;
		if (windowState->titleInfo.length == 0)
		{
			if (!(title = _strdup("")))
			{
				WLog_ERR(TAG, "failed to duplicate empty window title string");
				return FALSE;
			}
		}
		else if (!(title = ConvertWCharNToUtf8Alloc(
		               cnv.wc, windowState->titleInfo.length / sizeof(WCHAR), NULL)))
		{
			WLog_ERR(TAG, "failed to convert window title");
			return FALSE;
		}

		free(appWindow->title);
		appWindow->title = title;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_CLIENT_AREA_OFFSET)
	{
		appWindow->clientOffsetX = windowState->clientOffsetX;
		appWindow->clientOffsetY = windowState->clientOffsetY;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE)
	{
		appWindow->clientAreaWidth = windowState->clientAreaWidth;
		appWindow->clientAreaHeight = windowState->clientAreaHeight;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_WND_CLIENT_DELTA)
	{
		appWindow->windowClientDeltaX = windowState->windowClientDeltaX;
		appWindow->windowClientDeltaY = windowState->windowClientDeltaY;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_WND_RECTS)
	{
		if (appWindow->windowRects)
		{
			free(appWindow->windowRects);
			appWindow->windowRects = NULL;
		}

		appWindow->numWindowRects = windowState->numWindowRects;

		if (appWindow->numWindowRects)
		{
			appWindow->windowRects =
			    (RECTANGLE_16*)calloc(appWindow->numWindowRects, sizeof(RECTANGLE_16));

			if (!appWindow->windowRects)
				return FALSE;

			CopyMemory(appWindow->windowRects, windowState->windowRects,
			           appWindow->numWindowRects * sizeof(RECTANGLE_16));
		}
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_VIS_OFFSET)
	{
		appWindow->visibleOffsetX = windowState->visibleOffsetX;
		appWindow->visibleOffsetY = windowState->visibleOffsetY;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_VISIBILITY)
	{
		if (appWindow->visibilityRects)
		{
			free(appWindow->visibilityRects);
			appWindow->visibilityRects = NULL;
		}

		appWindow->numVisibilityRects = windowState->numVisibilityRects;

		if (appWindow->numVisibilityRects)
		{
			appWindow->visibilityRects =
			    (RECTANGLE_16*)calloc(appWindow->numVisibilityRects, sizeof(RECTANGLE_16));

			if (!appWindow->visibilityRects)
				return FALSE;

			CopyMemory(appWindow->visibilityRects, windowState->visibilityRects,
			           appWindow->numVisibilityRects * sizeof(RECTANGLE_16));
		}
	}

	/* Update Window */

	if (fieldFlags & WINDOW_ORDER_FIELD_STYLE)
	{
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_SHOW)
	{
		xf_ShowWindow(xfc, appWindow, WINPR_ASSERTING_INT_CAST(UINT8, appWindow->showState));
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_TITLE)
	{
		if (appWindow->title)
			xf_SetWindowText(xfc, appWindow, appWindow->title);
	}

	if (position_or_size_updated)
	{
		const INT32 visibilityRectsOffsetX =
		    (appWindow->visibleOffsetX -
		     (appWindow->clientOffsetX - appWindow->windowClientDeltaX));
		const INT32 visibilityRectsOffsetY =
		    (appWindow->visibleOffsetY -
		     (appWindow->clientOffsetY - appWindow->windowClientDeltaY));

		/*
		 * The rail server like to set the window to a small size when it is minimized even though
		 * it is hidden in some cases this can cause the window not to restore back to its original
		 * size. Therefore we don't update our local window when that rail window state is minimized
		 */
		if (appWindow->rail_state != WINDOW_SHOW_MINIMIZED)
		{
			/* Redraw window area if already in the correct position */
			if (appWindow->x == (INT64)appWindow->windowOffsetX &&
			    appWindow->y == (INT64)appWindow->windowOffsetY &&
			    appWindow->width == (INT64)appWindow->windowWidth &&
			    appWindow->height == (INT64)appWindow->windowHeight)
			{
				xf_UpdateWindowArea(xfc, appWindow, 0, 0,
				                    WINPR_ASSERTING_INT_CAST(int, appWindow->windowWidth),
				                    WINPR_ASSERTING_INT_CAST(int, appWindow->windowHeight));
			}
			else
			{
				xf_MoveWindow(xfc, appWindow, appWindow->windowOffsetX, appWindow->windowOffsetY,
				              WINPR_ASSERTING_INT_CAST(int, appWindow->windowWidth),
				              WINPR_ASSERTING_INT_CAST(int, appWindow->windowHeight));
			}

			xf_SetWindowVisibilityRects(
			    xfc, appWindow, WINPR_ASSERTING_INT_CAST(uint32_t, visibilityRectsOffsetX),
			    WINPR_ASSERTING_INT_CAST(uint32_t, visibilityRectsOffsetY),
			    appWindow->visibilityRects,
			    WINPR_ASSERTING_INT_CAST(int, appWindow->numVisibilityRects));
		}

		if (appWindow->rail_state == WINDOW_SHOW_MAXIMIZED)
		{
			xf_SendClientEvent(xfc, appWindow->handle, xfc->NET_WM_STATE, 4, NET_WM_STATE_ADD,
			                   xfc->NET_WM_STATE_MAXIMIZED_VERT, xfc->NET_WM_STATE_MAXIMIZED_HORZ,
			                   0, 0);
		}
	}

	if (fieldFlags & (WINDOW_ORDER_STATE_NEW | WINDOW_ORDER_FIELD_STYLE))
		xf_SetWindowStyle(xfc, appWindow, appWindow->dwStyle, appWindow->dwExStyle);

	/* We should only be using the visibility rects for shaping the window */
	/*if (fieldFlags & WINDOW_ORDER_FIELD_WND_RECTS)
	{
	    xf_SetWindowRects(xfc, appWindow, appWindow->windowRects, appWindow->numWindowRects);
	}*/
	return TRUE;
}

static BOOL xf_rail_window_delete(rdpContext* context, const WINDOW_ORDER_INFO* orderInfo)
{
	xfContext* xfc = (xfContext*)context;
	WINPR_ASSERT(xfc);
	return xf_rail_del_window(xfc, orderInfo->windowId);
}

static xfRailIconCache* RailIconCache_New(rdpSettings* settings)
{
	xfRailIconCache* cache = calloc(1, sizeof(xfRailIconCache));

	if (!cache)
		return NULL;

	cache->numCaches = freerdp_settings_get_uint32(settings, FreeRDP_RemoteAppNumIconCaches);
	cache->numCacheEntries =
	    freerdp_settings_get_uint32(settings, FreeRDP_RemoteAppNumIconCacheEntries);
	cache->entries = calloc(1ull * cache->numCaches * cache->numCacheEntries, sizeof(xfRailIcon));

	if (!cache->entries)
	{
		WLog_ERR(TAG, "failed to allocate icon cache %" PRIu32 " x %" PRIu32 " entries",
		         cache->numCaches, cache->numCacheEntries);
		free(cache);
		return NULL;
	}

	return cache;
}

static void RailIconCache_Free(xfRailIconCache* cache)
{
	if (!cache)
		return;

	for (UINT32 i = 0; i < cache->numCaches * cache->numCacheEntries; i++)
	{
		xfRailIcon* cur = &cache->entries[i];
		free(cur->data);
	}

	free(cache->scratch.data);
	free(cache->entries);
	free(cache);
}

static xfRailIcon* RailIconCache_Lookup(xfRailIconCache* cache, UINT8 cacheId, UINT16 cacheEntry)
{
	WINPR_ASSERT(cache);
	/*
	 * MS-RDPERP 2.2.1.2.3 Icon Info (TS_ICON_INFO)
	 *
	 * CacheId (1 byte):
	 *     If the value is 0xFFFF, the icon SHOULD NOT be cached.
	 *
	 * Yes, the spec says "0xFFFF" in the 2018-03-16 revision,
	 * but the actual protocol field is 1-byte wide.
	 */
	if (cacheId == 0xFF)
		return &cache->scratch;

	if (cacheId >= cache->numCaches)
		return NULL;

	if (cacheEntry >= cache->numCacheEntries)
		return NULL;

	return &cache->entries[cache->numCacheEntries * cacheId + cacheEntry];
}

/*
 * _NET_WM_ICON format is defined as "array of CARDINAL" values which for
 * Xlib must be represented with an array of C's "long" values. Note that
 * "long" != "INT32" on 64-bit systems. Therefore we can't simply cast
 * the bitmap data as (unsigned char*), we have to copy all the pixels.
 *
 * The first two values are width and height followed by actual color data
 * in ARGB format (e.g., 0xFFFF0000L is opaque red), pixels are in normal,
 * left-to-right top-down order.
 */
static BOOL convert_rail_icon(const ICON_INFO* iconInfo, xfRailIcon* railIcon)
{
	WINPR_ASSERT(iconInfo);
	WINPR_ASSERT(railIcon);

	BYTE* nextPixel = NULL;
	long* pixels = NULL;
	BYTE* argbPixels = calloc(1ull * iconInfo->width * iconInfo->height, 4);

	if (!argbPixels)
		goto error;

	if (!freerdp_image_copy_from_icon_data(
	        argbPixels, PIXEL_FORMAT_ARGB32, 0, 0, 0,
	        WINPR_ASSERTING_INT_CAST(UINT16, iconInfo->width),
	        WINPR_ASSERTING_INT_CAST(UINT16, iconInfo->height), iconInfo->bitsColor,
	        WINPR_ASSERTING_INT_CAST(UINT16, iconInfo->cbBitsColor), iconInfo->bitsMask,
	        WINPR_ASSERTING_INT_CAST(UINT16, iconInfo->cbBitsMask), iconInfo->colorTable,
	        WINPR_ASSERTING_INT_CAST(UINT16, iconInfo->cbColorTable), iconInfo->bpp))
		goto error;

	const UINT32 nelements = 2 + iconInfo->width * iconInfo->height;
	pixels = realloc(railIcon->data, nelements * sizeof(long));

	if (!pixels)
		goto error;

	railIcon->data = pixels;

	railIcon->length = WINPR_ASSERTING_INT_CAST(int, nelements);
	pixels[0] = iconInfo->width;
	pixels[1] = iconInfo->height;
	nextPixel = argbPixels;

	for (UINT32 i = 2; i < nelements; i++)
	{
		pixels[i] = FreeRDPReadColor(nextPixel, PIXEL_FORMAT_BGRA32);
		nextPixel += 4;
	}

	free(argbPixels);
	return TRUE;
error:
	free(argbPixels);
	return FALSE;
}

static void xf_rail_set_window_icon(xfContext* xfc, xfAppWindow* railWindow, xfRailIcon* icon,
                                    BOOL replace)
{
	WINPR_ASSERT(xfc);

	LogDynAndXChangeProperty(xfc->log, xfc->display, railWindow->handle, xfc->NET_WM_ICON,
	                         XA_CARDINAL, 32, replace ? PropModeReplace : PropModeAppend,
	                         (unsigned char*)icon->data, icon->length);
	LogDynAndXFlush(xfc->log, xfc->display);
}

static BOOL xf_rail_window_icon(rdpContext* context, const WINDOW_ORDER_INFO* orderInfo,
                                const WINDOW_ICON_ORDER* windowIcon)
{
	xfContext* xfc = (xfContext*)context;
	BOOL replaceIcon = 0;
	xfAppWindow* railWindow = xf_rail_get_window(xfc, orderInfo->windowId);

	if (!railWindow)
		return TRUE;

	WINPR_ASSERT(windowIcon);
	WINPR_ASSERT(windowIcon->iconInfo);
	xfRailIcon* icon = RailIconCache_Lookup(
	    xfc->railIconCache, WINPR_ASSERTING_INT_CAST(UINT8, windowIcon->iconInfo->cacheId),
	    WINPR_ASSERTING_INT_CAST(UINT16, windowIcon->iconInfo->cacheEntry));

	if (!icon)
	{
		WLog_WARN(TAG, "failed to get icon from cache %02X:%04X", windowIcon->iconInfo->cacheId,
		          windowIcon->iconInfo->cacheEntry);
		return FALSE;
	}

	if (!convert_rail_icon(windowIcon->iconInfo, icon))
	{
		WLog_WARN(TAG, "failed to convert icon for window %08X", orderInfo->windowId);
		return FALSE;
	}

	replaceIcon = !!(orderInfo->fieldFlags & WINDOW_ORDER_STATE_NEW);
	xf_rail_set_window_icon(xfc, railWindow, icon, replaceIcon);
	return TRUE;
}

static BOOL xf_rail_window_cached_icon(rdpContext* context, const WINDOW_ORDER_INFO* orderInfo,
                                       const WINDOW_CACHED_ICON_ORDER* windowCachedIcon)
{
	xfContext* xfc = (xfContext*)context;
	WINPR_ASSERT(orderInfo);

	BOOL replaceIcon = 0;
	xfAppWindow* railWindow = xf_rail_get_window(xfc, orderInfo->windowId);

	if (!railWindow)
		return TRUE;

	WINPR_ASSERT(windowCachedIcon);

	xfRailIcon* icon = RailIconCache_Lookup(
	    xfc->railIconCache, WINPR_ASSERTING_INT_CAST(UINT8, windowCachedIcon->cachedIcon.cacheId),
	    WINPR_ASSERTING_INT_CAST(UINT16, windowCachedIcon->cachedIcon.cacheEntry));

	if (!icon)
	{
		WLog_WARN(TAG, "failed to get icon from cache %02X:%04X",
		          windowCachedIcon->cachedIcon.cacheId, windowCachedIcon->cachedIcon.cacheEntry);
		return FALSE;
	}

	replaceIcon = !!(orderInfo->fieldFlags & WINDOW_ORDER_STATE_NEW);
	xf_rail_set_window_icon(xfc, railWindow, icon, replaceIcon);
	return TRUE;
}

static BOOL
xf_rail_notify_icon_common(WINPR_ATTR_UNUSED rdpContext* context,
                           const WINDOW_ORDER_INFO* orderInfo,
                           WINPR_ATTR_UNUSED const NOTIFY_ICON_STATE_ORDER* notifyIconState)
{
	WLog_ERR("TODO", "TODO: implement");
	if (orderInfo->fieldFlags & WINDOW_ORDER_FIELD_NOTIFY_VERSION)
	{
	}

	if (orderInfo->fieldFlags & WINDOW_ORDER_FIELD_NOTIFY_TIP)
	{
	}

	if (orderInfo->fieldFlags & WINDOW_ORDER_FIELD_NOTIFY_INFO_TIP)
	{
	}

	if (orderInfo->fieldFlags & WINDOW_ORDER_FIELD_NOTIFY_STATE)
	{
	}

	if (orderInfo->fieldFlags & WINDOW_ORDER_ICON)
	{
	}

	if (orderInfo->fieldFlags & WINDOW_ORDER_CACHED_ICON)
	{
	}

	return TRUE;
}

static BOOL xf_rail_notify_icon_create(rdpContext* context, const WINDOW_ORDER_INFO* orderInfo,
                                       const NOTIFY_ICON_STATE_ORDER* notifyIconState)
{
	return xf_rail_notify_icon_common(context, orderInfo, notifyIconState);
}

static BOOL xf_rail_notify_icon_update(rdpContext* context, const WINDOW_ORDER_INFO* orderInfo,
                                       const NOTIFY_ICON_STATE_ORDER* notifyIconState)
{
	return xf_rail_notify_icon_common(context, orderInfo, notifyIconState);
}

static BOOL xf_rail_notify_icon_delete(WINPR_ATTR_UNUSED rdpContext* context,
                                       WINPR_ATTR_UNUSED const WINDOW_ORDER_INFO* orderInfo)
{
	WLog_ERR("TODO", "TODO: implement");
	return TRUE;
}

static BOOL
xf_rail_monitored_desktop(WINPR_ATTR_UNUSED rdpContext* context,
                          WINPR_ATTR_UNUSED const WINDOW_ORDER_INFO* orderInfo,
                          WINPR_ATTR_UNUSED const MONITORED_DESKTOP_ORDER* monitoredDesktop)
{
	WLog_ERR("TODO", "TODO: implement");
	return TRUE;
}

static BOOL xf_rail_non_monitored_desktop(rdpContext* context,
                                          WINPR_ATTR_UNUSED const WINDOW_ORDER_INFO* orderInfo)
{
	xfContext* xfc = (xfContext*)context;
	xf_rail_disable_remoteapp_mode(xfc);
	return TRUE;
}

static void xf_rail_register_update_callbacks(rdpUpdate* update)
{
	WINPR_ASSERT(update);

	rdpWindowUpdate* window = update->window;
	WINPR_ASSERT(window);

	window->WindowCreate = xf_rail_window_common;
	window->WindowUpdate = xf_rail_window_common;
	window->WindowDelete = xf_rail_window_delete;
	window->WindowIcon = xf_rail_window_icon;
	window->WindowCachedIcon = xf_rail_window_cached_icon;
	window->NotifyIconCreate = xf_rail_notify_icon_create;
	window->NotifyIconUpdate = xf_rail_notify_icon_update;
	window->NotifyIconDelete = xf_rail_notify_icon_delete;
	window->MonitoredDesktop = xf_rail_monitored_desktop;
	window->NonMonitoredDesktop = xf_rail_non_monitored_desktop;
}

/* RemoteApp Virtual Channel Extension */

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT xf_rail_server_execute_result(RailClientContext* context,
                                          const RAIL_EXEC_RESULT_ORDER* execResult)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(execResult);

	xfContext* xfc = (xfContext*)context->custom;
	WINPR_ASSERT(xfc);

	if (execResult->execResult != RAIL_EXEC_S_OK)
	{
		WLog_ERR(TAG, "RAIL exec error: execResult=%s NtError=0x%X\n",
		         error_code_names[execResult->execResult], execResult->rawResult);
		freerdp_abort_connect_context(&xfc->common.context);
	}
	else
	{
		xf_rail_enable_remoteapp_mode(xfc);
	}

	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT xf_rail_server_system_param(WINPR_ATTR_UNUSED RailClientContext* context,
                                        WINPR_ATTR_UNUSED const RAIL_SYSPARAM_ORDER* sysparam)
{
	// TODO: Actually apply param
	WLog_ERR("TODO", "TODO: implement");
	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT xf_rail_server_handshake(RailClientContext* context,
                                     WINPR_ATTR_UNUSED const RAIL_HANDSHAKE_ORDER* handshake)
{
	return client_rail_server_start_cmd(context);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT
xf_rail_server_handshake_ex(RailClientContext* context,
                            WINPR_ATTR_UNUSED const RAIL_HANDSHAKE_EX_ORDER* handshakeEx)
{
	return client_rail_server_start_cmd(context);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT xf_rail_server_local_move_size(RailClientContext* context,
                                           const RAIL_LOCALMOVESIZE_ORDER* localMoveSize)
{
	int x = 0;
	int y = 0;
	int direction = 0;
	Window child_window = 0;
	WINPR_ASSERT(context);
	WINPR_ASSERT(localMoveSize);

	xfContext* xfc = (xfContext*)context->custom;
	xfAppWindow* appWindow = xf_rail_get_window(xfc, localMoveSize->windowId);

	if (!appWindow)
		return ERROR_INTERNAL_ERROR;

	switch (localMoveSize->moveSizeType)
	{
		case RAIL_WMSZ_LEFT:
			direction = NET_WM_MOVERESIZE_SIZE_LEFT;
			x = localMoveSize->posX;
			y = localMoveSize->posY;
			break;

		case RAIL_WMSZ_RIGHT:
			direction = NET_WM_MOVERESIZE_SIZE_RIGHT;
			x = localMoveSize->posX;
			y = localMoveSize->posY;
			break;

		case RAIL_WMSZ_TOP:
			direction = NET_WM_MOVERESIZE_SIZE_TOP;
			x = localMoveSize->posX;
			y = localMoveSize->posY;
			break;

		case RAIL_WMSZ_TOPLEFT:
			direction = NET_WM_MOVERESIZE_SIZE_TOPLEFT;
			x = localMoveSize->posX;
			y = localMoveSize->posY;
			break;

		case RAIL_WMSZ_TOPRIGHT:
			direction = NET_WM_MOVERESIZE_SIZE_TOPRIGHT;
			x = localMoveSize->posX;
			y = localMoveSize->posY;
			break;

		case RAIL_WMSZ_BOTTOM:
			direction = NET_WM_MOVERESIZE_SIZE_BOTTOM;
			x = localMoveSize->posX;
			y = localMoveSize->posY;
			break;

		case RAIL_WMSZ_BOTTOMLEFT:
			direction = NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT;
			x = localMoveSize->posX;
			y = localMoveSize->posY;
			break;

		case RAIL_WMSZ_BOTTOMRIGHT:
			direction = NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT;
			x = localMoveSize->posX;
			y = localMoveSize->posY;
			break;

		case RAIL_WMSZ_MOVE:
			direction = NET_WM_MOVERESIZE_MOVE;
			XTranslateCoordinates(xfc->display, appWindow->handle, RootWindowOfScreen(xfc->screen),
			                      localMoveSize->posX, localMoveSize->posY, &x, &y, &child_window);
			break;

		case RAIL_WMSZ_KEYMOVE:
			direction = NET_WM_MOVERESIZE_MOVE_KEYBOARD;
			x = localMoveSize->posX;
			y = localMoveSize->posY;
			/* FIXME: local keyboard moves not working */
			return CHANNEL_RC_OK;

		case RAIL_WMSZ_KEYSIZE:
			direction = NET_WM_MOVERESIZE_SIZE_KEYBOARD;
			x = localMoveSize->posX;
			y = localMoveSize->posY;
			/* FIXME: local keyboard moves not working */
			return CHANNEL_RC_OK;
		default:
			break;
	}

	if (localMoveSize->isMoveSizeStart)
		xf_StartLocalMoveSize(xfc, appWindow, direction, x, y);
	else
		xf_EndLocalMoveSize(xfc, appWindow);

	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT xf_rail_server_min_max_info(RailClientContext* context,
                                        const RAIL_MINMAXINFO_ORDER* minMaxInfo)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(minMaxInfo);

	xfContext* xfc = (xfContext*)context->custom;
	xfAppWindow* appWindow = xf_rail_get_window(xfc, minMaxInfo->windowId);

	if (appWindow)
	{
		xf_SetWindowMinMaxInfo(xfc, appWindow, minMaxInfo->maxWidth, minMaxInfo->maxHeight,
		                       minMaxInfo->maxPosX, minMaxInfo->maxPosY, minMaxInfo->minTrackWidth,
		                       minMaxInfo->minTrackHeight, minMaxInfo->maxTrackWidth,
		                       minMaxInfo->maxTrackHeight);
	}

	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT
xf_rail_server_language_bar_info(WINPR_ATTR_UNUSED RailClientContext* context,
                                 WINPR_ATTR_UNUSED const RAIL_LANGBAR_INFO_ORDER* langBarInfo)
{
	WLog_ERR("TODO", "TODO: implement");
	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT
xf_rail_server_get_appid_response(WINPR_ATTR_UNUSED RailClientContext* context,
                                  WINPR_ATTR_UNUSED const RAIL_GET_APPID_RESP_ORDER* getAppIdResp)
{
	WLog_ERR("TODO", "TODO: implement");
	return CHANNEL_RC_OK;
}

static BOOL rail_window_key_equals(const void* key1, const void* key2)
{
	const UINT64* k1 = (const UINT64*)key1;
	const UINT64* k2 = (const UINT64*)key2;

	if (!k1 || !k2)
		return FALSE;

	return *k1 == *k2;
}

static UINT32 rail_window_key_hash(const void* key)
{
	const UINT64* k1 = (const UINT64*)key;
	return (UINT32)*k1;
}

static void rail_window_free(void* value)
{
	xfAppWindow* appWindow = (xfAppWindow*)value;

	if (!appWindow)
		return;

	xf_DestroyWindow(appWindow->xfc, appWindow);
}

int xf_rail_init(xfContext* xfc, RailClientContext* rail)
{
	rdpContext* context = (rdpContext*)xfc;

	if (!xfc || !rail)
		return 0;

	xfc->rail = rail;
	xf_rail_register_update_callbacks(context->update);
	rail->custom = (void*)xfc;
	rail->ServerExecuteResult = xf_rail_server_execute_result;
	rail->ServerSystemParam = xf_rail_server_system_param;
	rail->ServerHandshake = xf_rail_server_handshake;
	rail->ServerHandshakeEx = xf_rail_server_handshake_ex;
	rail->ServerLocalMoveSize = xf_rail_server_local_move_size;
	rail->ServerMinMaxInfo = xf_rail_server_min_max_info;
	rail->ServerLanguageBarInfo = xf_rail_server_language_bar_info;
	rail->ServerGetAppIdResponse = xf_rail_server_get_appid_response;
	xfc->railWindows = HashTable_New(TRUE);

	if (!xfc->railWindows)
		return 0;

	if (!HashTable_SetHashFunction(xfc->railWindows, rail_window_key_hash))
		goto fail;
	{
		wObject* obj = HashTable_KeyObject(xfc->railWindows);
		obj->fnObjectEquals = rail_window_key_equals;
	}
	{
		wObject* obj = HashTable_ValueObject(xfc->railWindows);
		obj->fnObjectFree = rail_window_free;
	}
	xfc->railIconCache = RailIconCache_New(xfc->common.context.settings);

	if (!xfc->railIconCache)
	{
	}

	return 1;
fail:
	HashTable_Free(xfc->railWindows);
	return 0;
}

int xf_rail_uninit(xfContext* xfc, RailClientContext* rail)
{
	WINPR_UNUSED(rail);

	if (xfc->rail)
	{
		xfc->rail->custom = NULL;
		xfc->rail = NULL;
	}

	if (xfc->railWindows)
	{
		HashTable_Free(xfc->railWindows);
		xfc->railWindows = NULL;
	}

	if (xfc->railIconCache)
	{
		RailIconCache_Free(xfc->railIconCache);
		xfc->railIconCache = NULL;
	}

	return 1;
}

xfAppWindow* xf_rail_add_window(xfContext* xfc, UINT64 id, INT32 x, INT32 y, UINT32 width,
                                UINT32 height, UINT32 surfaceId)
{
	if (!xfc)
		return NULL;

	xfAppWindow* appWindow = (xfAppWindow*)calloc(1, sizeof(xfAppWindow));

	if (!appWindow)
		return NULL;

	appWindow->xfc = xfc;
	appWindow->windowId = id;
	appWindow->surfaceId = surfaceId;
	appWindow->x = x;
	appWindow->y = y;
	appWindow->width = WINPR_ASSERTING_INT_CAST(int, width);
	appWindow->height = WINPR_ASSERTING_INT_CAST(int, height);

	if (!xf_AppWindowCreate(xfc, appWindow))
		goto fail;
	if (!HashTable_Insert(xfc->railWindows, &appWindow->windowId, (void*)appWindow))
		goto fail;
	return appWindow;
fail:
	rail_window_free(appWindow);
	return NULL;
}

BOOL xf_rail_del_window(xfContext* xfc, UINT64 id)
{
	if (!xfc)
		return FALSE;

	if (!xfc->railWindows)
		return FALSE;

	return HashTable_Remove(xfc->railWindows, &id);
}

xfAppWindow* xf_rail_get_window(xfContext* xfc, UINT64 id)
{
	if (!xfc)
		return NULL;

	if (!xfc->railWindows)
		return FALSE;

	return HashTable_GetItemValue(xfc->railWindows, &id);
}
