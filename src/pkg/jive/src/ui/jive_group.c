/*
** Copyright 2007 Logitech. All Rights Reserved.
**
** This file is subject to the Logitech Public Source License Version 1.0. Please see the LICENCE file for details.
*/

#include "common.h"
#include "jive.h"


typedef struct group_widget {
	JiveWidget w;

	JiveTile *bg_tile;
} GroupWidget;


static JivePeerMeta groupPeerMeta = {
	sizeof(GroupWidget),
	"Group",
	jiveL_group_gc,
};


int jiveL_group_iterate(lua_State *L) {
	/* stack is:
	 * 1: widget
	 * 2: closure
	 */

	// group widgets
	lua_getfield(L, 1, "widgets");
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		lua_pushvalue(L, 2);
		lua_pushvalue(L, -2);
		lua_call(L, 1, 0);

		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	return 0;
}


int jiveL_group_skin(lua_State *L) {
	GroupWidget *peer;
	JiveTile *bg_tile;

	/* stack is:
	 * 1: widget
	 */

	lua_pushcfunction(L, jiveL_style_path);
	lua_pushvalue(L, -2);
	lua_call(L, 1, 0);

	peer = jive_getpeer(L, 1, &groupPeerMeta);

	jive_widget_pack(L, 1, (JiveWidget *)peer);


	bg_tile = jive_style_tile(L, 1, "bgImg", NULL);
	if (bg_tile != peer->bg_tile) {
		if (peer->bg_tile) {
			jive_tile_free(peer->bg_tile);
		}
		peer->bg_tile = jive_tile_ref(bg_tile);
	}

	/* group order table */
	lua_pushcfunction(L, jiveL_style_value);
	lua_pushvalue(L, 1);
	lua_pushstring(L, "order");
	lua_pushnil(L);
	lua_call(L, 3, 1);
	lua_setfield(L, 1, "_order");

	return 0;
}


int jiveL_group_layout(lua_State *L) {
	GroupWidget *peer;
	JiveInset *border;
	int *w;
	int fc = 0, fw = 0, num = 0;
	int max_w, sum_w;
	int i, x, y, h;

	peer = jive_getpeer(L, 1, &groupPeerMeta);

	lua_getfield(L, 1, "widgets");

	lua_getfield(L, 1, "_order");
	if (lua_isnil(L, 3)) {
		lua_pushnil(L);
		while (lua_next(L, 2) != 0) {
			num++;
			lua_pop(L, 1);
		}
	}
	else {
		/* create table with ordered widgets */
		lua_newtable(L);

		lua_pushnil(L);
		while (lua_next(L, 3) != 0) {
			lua_gettable(L, 2);
			lua_rawseti(L, -3, ++num);
		}

		lua_replace(L, 2); /* replace widgets */
	}
	lua_pop(L, 1); /* pop _order */

	
	/* stack is:
	 * 1: widget
	 * 2: widget.widgets (or ordered widgets)
	 */

	w = calloc(num, sizeof(int));
	h = 0;
	border = calloc(num, sizeof(JiveInset));

	/* first pass, find widget preferred bounds */
	i = 0;
	lua_pushnil(L);
	while (lua_next(L, 2) != 0) {
		if (jive_getmethod(L, -1, "getBorder")) {
			lua_pushvalue(L, -2);
			lua_call(L, 1, 4);
				
			border[i].left = lua_tointeger(L, -4);
			border[i].top = lua_tointeger(L, -3);
			border[i].right = lua_tointeger(L, -2);
			border[i].bottom = lua_tointeger(L, -1);
			lua_pop(L, 4);
		}
		else {
			border[i].left = 0;
			border[i].top = 0;
			border[i].right = 0;
			border[i].bottom = 0;
		}

		if (jive_getmethod(L, -1, "getPreferredBounds")) {
			lua_pushvalue(L, -2);
			lua_call(L, 1, 4);

			w[i] = lua_tointeger(L, -2) + border[i].left + border[i].right;
			h = MAX(h + border[i].top + border[i].bottom, lua_tointeger(L, -1));

			if (w[i] == JIVE_WH_FILL) {
				fc++;
			}
			else {
				fw += w[i];
			}
			lua_pop(L, 4);
		}

		i++;
		lua_pop(L, 1);
	}
		
	max_w = peer->w.bounds.w - peer->w.padding.left - peer->w.padding.right;
	sum_w = 0;
	for (i=0; i<num; i++) {
		/* set width for widgets to fill space */
		if (w[i] == JIVE_WH_FILL) {
			w[i] = (max_w - fw) / fc;
		}

		/* don't exceed widget width */
		if (sum_w + w[i] > max_w) {
			w[i] = max_w - sum_w;
		}
		sum_w += w[i];
	}

	if (h != JIVE_WH_NIL && h != JIVE_WH_FILL) {
		h = MAX(h, peer->w.bounds.h - peer->w.padding.top - peer->w.padding.bottom);
	}

	x = peer->w.bounds.x + peer->w.padding.left;
	y = peer->w.bounds.y + peer->w.padding.top;

	/* second pass, set widget bounds */
	i = 0;
	lua_pushnil(L);
	while (lua_next(L, 2) != 0) {
		if (jive_getmethod(L, -1, "setBounds")) {
			lua_pushvalue(L, -2);
			lua_pushinteger(L, x + border[i].left);
			lua_pushinteger(L, y + border[i].top);
			lua_pushinteger(L, w[i] - border[i].left - border[i].right);
			lua_pushinteger(L, h - border[i].top - border[i].bottom);
			
			lua_call(L, 5, 0);
		}

		x += w[i++];

		lua_pop(L, 1);
	}

	free(w);
	free(border);

	return 0;
}


static int draw_closure(lua_State *L) {
	if (jive_getmethod(L, 1, "draw")) {
		lua_pushvalue(L, 1); // widget
		lua_pushvalue(L, lua_upvalueindex(1)); // surface
		lua_pushvalue(L, lua_upvalueindex(2)); // layer
		lua_call(L, 3, 0);
	}

	return 0;
}


int jiveL_group_draw(lua_State *L) {

	/* stack is:
	 * 1: widget
	 * 2: surface
	 * 3: layer
	 */

	GroupWidget *peer = jive_getpeer(L, 1, &groupPeerMeta);
	JiveSurface *srf = tolua_tousertype(L, 2, 0);
	bool drawLayer = luaL_optinteger(L, 3, JIVE_LAYER_ALL) & peer->w.layer;

	if (drawLayer && peer->bg_tile) {
		jive_tile_blit(peer->bg_tile, srf, peer->w.bounds.x, peer->w.bounds.y, peer->w.bounds.w, peer->w.bounds.h);
		
		//jive_surface_boxColor(srf, peer->w.bounds.x, peer->w.bounds.y, peer->w.bounds.x + peer->w.bounds.w-1, peer->w.bounds.y + peer->w.bounds.h-1, 0xFF00007F);
	}

	/* draw widgets */
	if (jive_getmethod(L, 1, "iterate")) {
		lua_pushvalue(L, 1); // widget

		lua_pushvalue(L, 2); // surface
		lua_pushvalue(L, 3); // layer
		lua_pushcclosure(L, draw_closure, 2);

		lua_call(L, 2, 0);
	}

	return 0;
}


int jiveL_group_get_preferred_bounds(lua_State *L) {
	GroupWidget *peer;
	int w = 0, h = 0;

	if (jive_getmethod(L, 1, "checkLayout")) {
		lua_pushvalue(L, 1);
		lua_call(L, 1, 0);
	}

	peer = jive_getpeer(L, 1, &groupPeerMeta);

	lua_getfield(L, 1, "widgets");
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		if (jive_getmethod(L, -1, "getPreferredBounds")) {
			lua_pushvalue(L, -2);
			lua_call(L, 1, 4);

			w += lua_tointeger(L, -2);
			h = MAX(h, lua_tointeger(L, -1));

			lua_pop(L, 4);
		}

		lua_pop(L, 1);
	}
	lua_pop(L, 1);


	if (peer->w.preferred_bounds.x == JIVE_XY_NIL) {
		lua_pushnil(L);
	}
	else {
		lua_pushinteger(L, peer->w.preferred_bounds.x);
	}
	if (peer->w.preferred_bounds.y == JIVE_XY_NIL) {
		lua_pushnil(L);
	}
	else {
		lua_pushinteger(L, peer->w.preferred_bounds.y);
	}
	lua_pushinteger(L, (peer->w.preferred_bounds.w == JIVE_WH_NIL) ? w : peer->w.preferred_bounds.w);
	lua_pushinteger(L, (peer->w.preferred_bounds.h == JIVE_WH_NIL) ? h : peer->w.preferred_bounds.h);

	return 4;
}


int jiveL_group_gc(lua_State *L) {
	GroupWidget *peer = lua_touserdata(L, 1);

	if (peer->bg_tile) {
		jive_tile_free(peer->bg_tile);
		peer->bg_tile = NULL;
	}

	return 0;
}
