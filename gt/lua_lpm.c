/*
 * Gatekeeper - DDoS protection system.
 * Copyright (C) 2016 Digirati LTDA.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <arpa/inet.h>
#include <lauxlib.h>

#include <rte_lcore.h>
#include <rte_atomic.h>

#include "gatekeeper_fib.h"
#include "gatekeeper_gt.h"
#include "lua_lpm.h"

static int
l_str_to_prefix(lua_State *l)
{
	int ret;
	struct ipaddr ip_addr;

	/* First argument must be an IP prefix string. */
	const char *prefix_str = luaL_checkstring(l, 1);

	if (lua_gettop(l) != 1)
		luaL_error(l, "Expected one argument, however it got %d arguments",
			lua_gettop(l));

	ret = parse_ip_prefix(prefix_str, &ip_addr);
	if (ret < 0 || ip_addr.proto != RTE_ETHER_TYPE_IPV4)
		luaL_error(l, "gk: failed to parse the IPv4 prefix: %s",
			prefix_str);

	lua_pushinteger(l, ip_addr.ip.v4.s_addr);
	lua_pushinteger(l, ret);

	return 2;
}

#define CTYPE_STRUCT_IN6_ADDR "struct in6_addr"
#define CTYPE_STRUCT_IN6_ADDR_REF "struct in6_addr &"
#define CTYPE_STRUCT_IN6_ADDR_PTR "struct in6_addr *"

static struct in6_addr *
get_ipv6_addr(lua_State *l, int idx)
{
	/* Testing for type CTYPE_STRUCT_IN6_ADDR. */
	uint32_t correct_ctypeid_in6_addr = luaL_get_ctypeid(l,
		CTYPE_STRUCT_IN6_ADDR);
	uint32_t ctypeid;
	void *cdata = luaL_checkcdata(l, idx, &ctypeid,	CTYPE_STRUCT_IN6_ADDR);
	if (ctypeid == correct_ctypeid_in6_addr)
		return cdata;

	/* Testing for type CTYPE_STRUCT_IN6_ADDR_REF. */
	correct_ctypeid_in6_addr = luaL_get_ctypeid(l,
		CTYPE_STRUCT_IN6_ADDR_REF);
	cdata = luaL_checkcdata(l, idx, &ctypeid, CTYPE_STRUCT_IN6_ADDR_REF);
	if (likely(ctypeid == correct_ctypeid_in6_addr))
		return *(struct in6_addr **)cdata;

	/* Testing for type CTYPE_STRUCT_IN6_ADDR_PTR. */
	correct_ctypeid_in6_addr = luaL_get_ctypeid(l,
		CTYPE_STRUCT_IN6_ADDR_PTR);
	cdata = luaL_checkcdata(l, idx, &ctypeid, CTYPE_STRUCT_IN6_ADDR_PTR);
	if (likely(ctypeid == correct_ctypeid_in6_addr))
		return *(struct in6_addr **)cdata;

	luaL_error(l, "Expected '%s', `%s', or '%s' as argument #%d",
		CTYPE_STRUCT_IN6_ADDR, CTYPE_STRUCT_IN6_ADDR_REF,
		CTYPE_STRUCT_IN6_ADDR_PTR, idx);
	/* Make compiler happy; the above luaL_error() doesn't return. */
	return NULL;
}

static int
l_str_to_prefix6(lua_State *l)
{
	int ret;
	struct ipaddr ip_addr;
	struct in6_addr *cdata;
	uint32_t correct_ctypeid_in6_addr;

	/* First argument must be an IP prefix string. */
	const char *prefix_str = luaL_checkstring(l, 1);

	if (lua_gettop(l) != 1)
		luaL_error(l, "Expected one argument, however it got %d arguments",
			lua_gettop(l));

	ret = parse_ip_prefix(prefix_str, &ip_addr);
	if (ret < 0 || ip_addr.proto != RTE_ETHER_TYPE_IPV6)
		luaL_error(l, "gk: failed to parse the IPv6 prefix: %s",
			prefix_str);

	correct_ctypeid_in6_addr = luaL_get_ctypeid(l,
		CTYPE_STRUCT_IN6_ADDR);
	cdata = luaL_pushcdata(l, correct_ctypeid_in6_addr,
		sizeof(struct in6_addr));
	*cdata = ip_addr.ip.v6;

	lua_pushinteger(l, ret);

	return 2;
}

#define LUA_LPM_UD_TNAME "gt_lpm_ud"

struct lpm_lua_userdata {
	struct	 fib_head fib;
	/* Parameters of @fib. */
	uint32_t max_rules;
	uint32_t num_tbl8s;
};

static int
l_new_lpm(lua_State *l)
{
	struct lpm_lua_userdata *lpm_ud;
	static rte_atomic32_t identifier = RTE_ATOMIC32_INIT(0);
	char fib_name[128];
	unsigned int lcore_id;
	int ret;

	if (unlikely(lua_gettop(l) != 2)) {
		luaL_error(l, "%s(): expected two arguments, however it got %d arguments",
			__func__, lua_gettop(l));
	}

	lpm_ud = lua_newuserdata(l, sizeof(*lpm_ud));
	/* First argument must be a Lua number. */
	lpm_ud->max_rules = luaL_checknumber(l, 1);
	/* Second argument must be a Lua number. */
	lpm_ud->num_tbl8s = luaL_checknumber(l, 2);

	/* Get @lcore_id. */
	lua_getfield(l, LUA_REGISTRYINDEX, GT_LUA_LCORE_ID_NAME);
	lcore_id = lua_tonumber(l, -1);
	lua_pop(l, 1);

	/* Obtain unique name. */
	ret = snprintf(fib_name, sizeof(fib_name),
		"gt_fib_ipv4_%u_%u", lcore_id,
		rte_atomic32_add_return(&identifier, 1));
	RTE_VERIFY(ret > 0 && ret < (int)sizeof(fib_name));

	ret = fib_create(&lpm_ud->fib, fib_name,
		rte_lcore_to_socket_id(lcore_id), 32,
		lpm_ud->max_rules, lpm_ud->num_tbl8s);
	if (unlikely(ret < 0)) {
		luaL_error(l, "%s(): failed to initialize the IPv4 LPM table for Lua policies (errno=%d): %s",
			__func__, -ret, strerror(-ret));
	}

	luaL_getmetatable(l, LUA_LPM_UD_TNAME);
	lua_setmetatable(l, -2);

	return 1;
}

static int
l_lpm_add(lua_State *l)
{
	int ret;

	/* First argument must be of type struct lpm_lua_userdata *. */
	struct lpm_lua_userdata *lpm_ud =
		luaL_checkudata(l, 1, LUA_LPM_UD_TNAME);

	/*
	 * Second argument must be a Lua number.
	 * @ip must be in network order.
	 */
	uint32_t ip = luaL_checknumber(l, 2);

	/* Third argument must be a Lua number. */
	uint8_t depth = luaL_checknumber(l, 3);

	/* Fourth argument must be a Lua number. */
	uint32_t label = luaL_checknumber(l, 4);

	if (unlikely(lua_gettop(l) != 4)) {
		luaL_error(l, "%s(): expected four arguments, however it got %d arguments",
			__func__, lua_gettop(l));
	}

	ret = fib_add(&lpm_ud->fib, (uint8_t *)&ip, depth, label);
	if (unlikely(ret < 0)) {
		luaL_error(l, "%s(): failed to add network policy [ip: %d, depth: %d, label: %d] (errno=%d): %s",
			__func__, ip, depth, label, -ret, strerror(-ret));
	}

	return 0;
}

static int
l_lpm_del(lua_State *l)
{
	/* First argument must be of type struct lpm_lua_userdata *. */
	struct lpm_lua_userdata *lpm_ud =
		luaL_checkudata(l, 1, LUA_LPM_UD_TNAME);

	/*
	 * Second argument must be a Lua number.
	 * @ip must be in network order.
	 * */
	uint32_t ip = luaL_checknumber(l, 2);

	/* Third argument must be a Lua number. */
	uint8_t depth = luaL_checknumber(l, 3);

	if (unlikely(lua_gettop(l) != 3)) {
		luaL_error(l, "%s(): expected three arguments, however it got %d arguments",
			__func__, lua_gettop(l));
	}

	lua_pushinteger(l, fib_delete(&lpm_ud->fib, (uint8_t *)&ip, depth));
	return 1;
}

static int
l_lpm_lookup(lua_State *l)
{
	/* First argument must be of type struct lpm_lua_userdata *. */
	struct lpm_lua_userdata *lpm_ud =
		luaL_checkudata(l, 1, LUA_LPM_UD_TNAME);
	uint32_t label;
	int ret;

	/*
	 * Second argument must be a Lua number.
	 * @ip must be in network order.
	 */
	uint32_t ip = luaL_checknumber(l, 2);

	if (unlikely(lua_gettop(l) != 2)) {
		luaL_error(l, "%s(): expected two arguments, however it got %d arguments",
			__func__, lua_gettop(l));
	}

	ret = fib_lookup(&lpm_ud->fib, (uint8_t *)&ip, &label);
	lua_pushinteger(l, ret >= 0 ? (lua_Integer)label : ret);
	return 1;
}

static int
debug_lookup(lua_State *l, struct fib_head *fib, const uint8_t *address)
{
	uint32_t rib_label, fib_label;
	int rib_ret, fib_ret;

	rib_ret = rib_lookup(fib_get_rib(fib), address, &rib_label);
	if (unlikely(rib_ret < 0 && rib_ret != -ENOENT)) {
		luaL_error(l, "%s(): RIB lookup failed (errno=%d): %s",
			__func__, -rib_ret, strerror(-rib_ret));
	}

	fib_ret = fib_lookup(fib, address, &fib_label);
	if (unlikely(fib_ret < 0 && fib_ret != -ENOENT)) {
		luaL_error(l, "%s(): RIB lookup (ret=%d, label=%d); FIB lookup failed (errno=%d): %s",
			__func__, rib_ret, rib_label,
			-fib_ret, strerror(-fib_ret));
	}

	if (rib_ret == 0) {
		if (likely(fib_ret == 0 && rib_label == fib_label))
			return 0;
	} else {
		if (likely(rib_ret == -ENOENT && fib_ret == -ENOENT))
			return 0;
	}

	luaL_error(l, "%s(): RIB lookup (ret=%d, label=%d) != FIB lookup (ret=%d, label=%d)",
		__func__, rib_ret, rib_label, fib_ret, fib_label);
	return -EFAULT;
}

static int
l_lpm_debug_lookup(lua_State *l)
{
	/* First argument must be of type struct lpm_lua_userdata *. */
	struct lpm_lua_userdata *lpm_ud =
		luaL_checkudata(l, 1, LUA_LPM_UD_TNAME);

	/*
	 * Second argument must be a Lua number.
	 * @ip must be in network order.
	 */
	uint32_t ip = luaL_checknumber(l, 2);

	if (unlikely(lua_gettop(l) != 2)) {
		luaL_error(l, "%s(): expected two arguments, however it got %d arguments",
			__func__, lua_gettop(l));
	}

	lua_pushinteger(l, debug_lookup(l, &lpm_ud->fib, (uint8_t *)&ip));
	return 1;
}

static int
l_ip_mask_addr(lua_State *l)
{
	uint32_t masked_ip;
	struct in_addr mask;
	char buf[INET_ADDRSTRLEN];

	/*
	 * First argument must be a Lua number.
	 * @ip must be in network order.
	 */
	uint32_t ip = luaL_checknumber(l, 1);

	/* Second argument must be a Lua number. */
	uint8_t depth = luaL_checknumber(l, 2);
	if (unlikely(depth > 32)) {
		luaL_error(l, "%s(): depth=%d must be in [0, 32]",
			__func__, depth);
	}

	if (unlikely(lua_gettop(l) != 2)) {
		luaL_error(l, "%s(): expected two arguments, however it got %d arguments",
			__func__, lua_gettop(l));
	}

	ip4_prefix_mask(depth, &mask);
	masked_ip = htonl(ntohl(ip) & rte_be_to_cpu_32(mask.s_addr));

	if (unlikely(inet_ntop(AF_INET, &masked_ip, buf, sizeof(buf)) ==
			NULL)) {
		luaL_error(l, "%s(): failed to convert a number to an IPv4 address (errno=%d): %s",
			__func__, errno, strerror(errno));
	}

	lua_pushstring(l, buf);
	return 1;
}

static int
l_lpm_get_paras(lua_State *l)
{
	/* First argument must be of type struct lpm_lua_userdata *. */
	struct lpm_lua_userdata *lpm_ud =
		luaL_checkudata(l, 1, LUA_LPM_UD_TNAME);

	if (unlikely(lua_gettop(l) != 1)) {
		luaL_error(l, "%s(): expected one argument, however it got %d arguments",
			__func__, lua_gettop(l));
	}

	lua_pushinteger(l, lpm_ud->max_rules);
	lua_pushinteger(l, lpm_ud->num_tbl8s);
	return 2;
}

#define LUA_LPM6_UD_TNAME "gt_lpm6_ud"

/*
 * This struct is currently identical to struct lpm_lua_userdata.
 * These structs are kept independent of each other to enable a possible
 * divergence in the future as have happened in the past.
 */
struct lpm6_lua_userdata {
	struct	 fib_head fib;
	/* Parameters of @fib. */
	uint32_t max_rules;
	uint32_t num_tbl8s;
};

static int
l_new_lpm6(lua_State *l)
{
	struct lpm6_lua_userdata *lpm6_ud;
	static rte_atomic32_t identifier6 = RTE_ATOMIC32_INIT(0);
	char fib_name[128];
	unsigned int lcore_id;
	int ret;

	if (unlikely(lua_gettop(l) != 2)) {
		luaL_error(l, "%s(): expected two arguments, however it got %d arguments",
			__func__, lua_gettop(l));
	}

	lpm6_ud = lua_newuserdata(l, sizeof(*lpm6_ud));
	/* First argument must be a Lua number. */
	lpm6_ud->max_rules = luaL_checknumber(l, 1);
	/* Second argument must be a Lua number. */
	lpm6_ud->num_tbl8s = luaL_checknumber(l, 2);

	/* Get @lcore_id. */
	lua_getfield(l, LUA_REGISTRYINDEX, GT_LUA_LCORE_ID_NAME);
	lcore_id = lua_tonumber(l, -1);
	lua_pop(l, 1);

	/* Obtain unique name. */
	ret = snprintf(fib_name, sizeof(fib_name),
		"gt_fib_ipv6_%u_%u", lcore_id,
		rte_atomic32_add_return(&identifier6, 1));
	RTE_VERIFY(ret > 0 && ret < (int)sizeof(fib_name));

	ret = fib_create(&lpm6_ud->fib, fib_name,
		rte_lcore_to_socket_id(lcore_id), 128,
		lpm6_ud->max_rules, lpm6_ud->num_tbl8s);
	if (unlikely(ret < 0)) {
		luaL_error(l, "%s(): failed to initialize the IPv6 LPM table for Lua policies (errno=%d): %s",
			__func__, -ret, strerror(-ret));
	}

	luaL_getmetatable(l, LUA_LPM6_UD_TNAME);
	lua_setmetatable(l, -2);

	return 1;
}

static int
l_lpm6_add(lua_State *l)
{
	int ret;

	/* First argument must be of type struct lpm6_lua_userdata *. */
	struct lpm6_lua_userdata *lpm6_ud =
		luaL_checkudata(l, 1, LUA_LPM6_UD_TNAME);

	/* Second argument must be a struct in6_add. */
	struct in6_addr *ipv6_addr = get_ipv6_addr(l, 2);

	/* Third argument must be a Lua number. */
	uint8_t depth = luaL_checknumber(l, 3);

	/* Fourth argument must be a Lua number. */
	uint32_t label = luaL_checknumber(l, 4);

	if (unlikely(lua_gettop(l) != 4)) {
		luaL_error(l, "%s(): expected four arguments, however it got %d arguments",
			__func__, lua_gettop(l));
	}

	ret = fib_add(&lpm6_ud->fib, ipv6_addr->s6_addr, depth, label);
	if (unlikely(ret < 0)) {
		char addr_buf[INET6_ADDRSTRLEN];
		if (unlikely(inet_ntop(AF_INET6, ipv6_addr, addr_buf,
				sizeof(addr_buf)) == NULL)) {
			luaL_error(l, "%s(): failed to add a network policy to the lpm6 table (errno=%d): %s",
				__func__, -ret, strerror(-ret));
		}
		luaL_error(l, "%s(%s/%d): failed to add a network policy to the lpm6 table (errno=%d): %s",
			__func__, addr_buf, depth, -ret, strerror(-ret));
	}

	return 0;
}

static int
l_lpm6_del(lua_State *l)
{
	/* First argument must be of type struct lpm6_lua_userdata *. */
	struct lpm6_lua_userdata *lpm6_ud =
		luaL_checkudata(l, 1, LUA_LPM6_UD_TNAME);

	/* Second argument must be a struct in6_add. */
	struct in6_addr *ipv6_addr = get_ipv6_addr(l, 2);

	/* Third argument must be a Lua number. */
	uint8_t depth = luaL_checknumber(l, 3);

	if (unlikely(lua_gettop(l) != 3)) {
		luaL_error(l, "%s(): expected three arguments, however it got %d arguments",
			__func__, lua_gettop(l));
	}

	lua_pushinteger(l, fib_delete(&lpm6_ud->fib, ipv6_addr->s6_addr,
		depth));
	return 1;
}

static int
l_lpm6_lookup(lua_State *l)
{
	/* First argument must be of type struct lpm6_lua_userdata *. */
	struct lpm6_lua_userdata *lpm6_ud =
		luaL_checkudata(l, 1, LUA_LPM6_UD_TNAME);
	uint32_t label;
	int ret;

	/* Second argument must be a struct in6_add. */
	struct in6_addr *ipv6_addr = get_ipv6_addr(l, 2);

	if (unlikely(lua_gettop(l) != 2)) {
		luaL_error(l, "%s(): expected two arguments, however it got %d arguments",
			__func__, lua_gettop(l));
	}

	ret = fib_lookup(&lpm6_ud->fib, ipv6_addr->s6_addr, &label);
	lua_pushinteger(l, ret >= 0 ? (lua_Integer)label : ret);
	return 1;
}

static int
l_lpm6_debug_lookup(lua_State *l)
{
	/* First argument must be of type struct lpm6_lua_userdata *. */
	struct lpm6_lua_userdata *lpm6_ud =
		luaL_checkudata(l, 1, LUA_LPM6_UD_TNAME);

	/* Second argument must be a struct in6_add. */
	struct in6_addr *ipv6_addr = get_ipv6_addr(l, 2);

	if (unlikely(lua_gettop(l) != 2)) {
		luaL_error(l, "%s(): expected two arguments, however it got %d arguments",
			__func__, lua_gettop(l));
	}

	lua_pushinteger(l, debug_lookup(l, &lpm6_ud->fib, ipv6_addr->s6_addr));
	return 1;
}

/*
 * Takes an array of uint8_t (IPv6 address) and masks it using the depth.
 */
static void
ip6_mask_addr(uint8_t *ip, uint8_t depth)
{
	struct in6_addr mask;
	uint64_t *paddr = (uint64_t *)ip;
	const uint64_t *pmask = (const uint64_t *)mask.s6_addr;

	ip6_prefix_mask(depth, &mask);

	paddr[0] &= pmask[0];
	paddr[1] &= pmask[1];
}

/* Copy ipv6 address. */
static inline void
ip6_copy_addr(uint8_t *dst, const uint8_t *src)
{
	rte_memcpy(dst, src, sizeof(struct in6_addr));
}

static int
l_ip6_mask_addr(lua_State *l)
{
	struct in6_addr masked_ip;
	char buf[INET6_ADDRSTRLEN];

	/* First argument must be a struct in6_add. */
	struct in6_addr *ipv6_addr = get_ipv6_addr(l, 1);

	/* Second argument must be a Lua number. */
	uint8_t depth = luaL_checknumber(l, 2);
	if (unlikely(depth > 128)) {
		luaL_error(l, "%s(): depth=%d must be in [0, 128]",
			__func__, depth);
	}

	if (unlikely(lua_gettop(l) != 2)) {
		luaL_error(l, "%s(): expected two arguments, however it got %d arguments",
			__func__, lua_gettop(l));
	}

	ip6_copy_addr(masked_ip.s6_addr, ipv6_addr->s6_addr);
	ip6_mask_addr(masked_ip.s6_addr, depth);

	if (unlikely(inet_ntop(AF_INET6, masked_ip.s6_addr, buf, sizeof(buf))
			== NULL)) {
		luaL_error(l, "%s(): failed to convert a number to an IPv6 address (errno=%d): %s",
			__func__, errno, strerror(errno));
	}

	lua_pushstring(l, buf);
	return 1;
}

static int
l_lpm6_get_paras(lua_State *l)
{
	/* First argument must be of type struct lpm6_lua_userdata *. */
	struct lpm6_lua_userdata *lpm6_ud =
		luaL_checkudata(l, 1, LUA_LPM6_UD_TNAME);

	if (lua_gettop(l) != 1)
		luaL_error(l, "Expected one argument, however it got %d arguments",
			lua_gettop(l));

	lua_pushinteger(l, lpm6_ud->max_rules);
	lua_pushinteger(l, lpm6_ud->num_tbl8s);
	return 2;
}

static const struct luaL_reg lpmlib_lua_c_funcs [] = {
	{"str_to_prefix",     l_str_to_prefix},
	{"new_lpm",           l_new_lpm},
	{"lpm_add",           l_lpm_add},
	{"lpm_del",           l_lpm_del},
	{"lpm_lookup",        l_lpm_lookup},
	{"ip_mask_addr",      l_ip_mask_addr},
	{"lpm_get_paras",     l_lpm_get_paras},
	{"lpm_debug_lookup",  l_lpm_debug_lookup},
	{"str_to_prefix6",    l_str_to_prefix6},
	{"new_lpm6",          l_new_lpm6},
	{"lpm6_add",          l_lpm6_add},
	{"lpm6_del",          l_lpm6_del},
	{"lpm6_lookup",       l_lpm6_lookup},
	{"ip6_mask_addr",     l_ip6_mask_addr},
	{"lpm6_get_paras",    l_lpm6_get_paras},
	{"lpm6_debug_lookup", l_lpm6_debug_lookup},
	{NULL,             NULL}	/* Sentinel. */
};

static int
lpm_ud_gc(lua_State *l) {
	struct lpm_lua_userdata *lpm_ud = lua_touserdata(l, 1);
	fib_free(&lpm_ud->fib);
	return 0;
}

static int
lpm6_ud_gc(lua_State *l) {
	struct lpm6_lua_userdata *lpm6_ud = lua_touserdata(l, 1);
	fib_free(&lpm6_ud->fib);
	return 0;
}

void
lualpm_openlib(lua_State *l) {
	luaL_newmetatable(l, LUA_LPM_UD_TNAME);
	lua_pushstring(l, "__gc");
	lua_pushcfunction(l, lpm_ud_gc);
	lua_settable(l, -3);

	luaL_newmetatable(l, LUA_LPM6_UD_TNAME);
	lua_pushstring(l, "__gc");
	lua_pushcfunction(l, lpm6_ud_gc);
	lua_settable(l, -3);

	luaL_register(l, "lpmlib", lpmlib_lua_c_funcs);
}
