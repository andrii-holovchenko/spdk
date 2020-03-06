/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"
#include "spdk/jsonrpc.h"
#include "spdk/rpc.h"
#include "spdk/event.h"
#include "spdk/util.h"
#include "spdk/env.h"

#include <ncurses.h>
#include <panel.h>
#include <menu.h>


#define RPC_MAX_THREADS 1024
#define RPC_MAX_POLLERS 1024
#define MAX_THREAD_NAME 128
#define MAX_THREAD_NAME_DISP 30
#define MAX_POLLER_NAME 128
#define MAX_POLLER_NAME_DISP 33
#define MAX_POLLER_TYPE_DISP 15
#define MAX_POLLER_COUNT_STR 4
#define MAX_POLLER_COUNT_DISP 25

#define MAX_STRING_LEN 12289 /* 3x 4k monitors + 1 */
#define TAB_WIN_HEIGHT 3
#define TAB_WIN_LOCATION_ROW 1
#define TABS_SPACING 2
#define TABS_LOCATION_ROW 4
#define TABS_LOCATION_COL 0
#define TABS_DATA_START_ROW 3
#define TABS_DATA_START_COL 3
#define TABS_COL_COUNT 10
#define MENU_WIN_HEIGHT 3
#define MENU_WIN_SPACING 4
#define MENU_WIN_LOCATION_COL 0

enum tabs {
	THREADS_TAB,
	POLLERS_TAB,
	CORES_TAB,
	NUMBER_OF_TABS,
};

enum spdk_poller_type {
	SPDK_ACTIVE_POLLER,
	SPDK_TIMED_POLLER,
	SPDK_PAUSED_POLLER,
	SPDK_POLLER_TYPES_COUNT,
};

struct col_desc {
	const char *name;
	uint8_t name_len;
	bool disabled;
};

const char *poller_type_str[SPDK_POLLER_TYPES_COUNT] = {"Active", "Timed", "Paused"};
const char *g_tab_title[NUMBER_OF_TABS] = {"[1] THREADS", "[2] POLLERS", "[3] CORES"};
struct spdk_jsonrpc_client *g_rpc_client;
WINDOW *g_menu_win, *g_tab_win[NUMBER_OF_TABS], *g_tabs[NUMBER_OF_TABS];
PANEL *g_panels[NUMBER_OF_TABS];
uint16_t g_max_row, g_max_col;
uint16_t g_data_win_size;
uint32_t g_last_threads_count, g_last_pollers_count, g_last_cores_count;
uint8_t g_current_sort_col[NUMBER_OF_TABS] = {0, 0, 0};
static struct col_desc g_col_desc[NUMBER_OF_TABS][TABS_COL_COUNT] = {
	{	{.name = "     Thread name     "},
		{.name = "     Active pollers     "},
		{.name = "     Timed pollers     "},
		{.name = "     Paused pollers     "},
		{.name = (char *)NULL}
	},
	{	{.name = "          Poller name          "},
		{.name = "     Type     "},
		{.name = "     On thread     "},
		{.name = (char *)NULL}
	},
	{	{.name = "     Core     "},
		{.name = (char *)NULL}
	}
};

struct rpc_thread_info {
	char *name;
	uint64_t id;
	char *cpumask;
	uint64_t busy;
	uint64_t idle;
	uint64_t active_pollers_count;
	uint64_t timed_pollers_count;
	uint64_t paused_pollers_count;
};

struct rpc_threads {
	uint64_t threads_count;
	struct rpc_thread_info thread_info[RPC_MAX_THREADS];
};

struct rpc_threads_stats {
	uint64_t tick_rate;
	struct rpc_threads threads;
};

struct rpc_poller_info {
	char *name;
	char *state;
	uint64_t run_count;
	uint64_t busy_count;
	uint64_t period_ticks;
	enum spdk_poller_type type;
	char thread_name[MAX_THREAD_NAME];
};

struct rpc_pollers {
	uint64_t pollers_count;
	struct rpc_poller_info pollers[RPC_MAX_POLLERS];
};

struct rpc_poller_thread_info {
	char *name;
	struct rpc_pollers active_pollers;
	struct rpc_pollers timed_pollers;
	struct rpc_pollers paused_pollers;
};

struct rpc_pollers_threads {
	uint64_t threads_count;
	struct rpc_poller_thread_info threads[RPC_MAX_THREADS];
};

struct rpc_pollers_stats {
	uint64_t tick_rate;
	struct rpc_pollers_threads pollers_threads;
};

struct rpc_threads_stats g_threads_stats;
struct rpc_pollers_stats g_pollers_stats;

static void
init_str_len(void)
{
	int i, j;

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		for (j = 0; g_col_desc[i][j].name != NULL; j++) {
			g_col_desc[i][j].name_len = strlen(g_col_desc[i][j].name);
		}
	}
}

static void
free_rpc_threads_stats(struct rpc_threads_stats *req)
{
	uint64_t i;

	for (i = 0; i < req->threads.threads_count; i++) {
		free(req->threads.thread_info[i].name);
		req->threads.thread_info[i].name = NULL;
		free(req->threads.thread_info[i].cpumask);
		req->threads.thread_info[i].cpumask = NULL;
	}
}

static const struct spdk_json_object_decoder rpc_thread_info_decoders[] = {
	{"name", offsetof(struct rpc_thread_info, name), spdk_json_decode_string},
	{"id", offsetof(struct rpc_thread_info, id), spdk_json_decode_uint64},
	{"cpumask", offsetof(struct rpc_thread_info, cpumask), spdk_json_decode_string},
	{"busy", offsetof(struct rpc_thread_info, busy), spdk_json_decode_uint64},
	{"idle", offsetof(struct rpc_thread_info, idle), spdk_json_decode_uint64},
	{"active_pollers_count", offsetof(struct rpc_thread_info, active_pollers_count), spdk_json_decode_uint64},
	{"timed_pollers_count", offsetof(struct rpc_thread_info, timed_pollers_count), spdk_json_decode_uint64},
	{"paused_pollers_count", offsetof(struct rpc_thread_info, paused_pollers_count), spdk_json_decode_uint64},
};

static int
rpc_decode_threads_object(const struct spdk_json_val *val, void *out)
{
	struct rpc_thread_info *info = out;

	return spdk_json_decode_object(val, rpc_thread_info_decoders,
				       SPDK_COUNTOF(rpc_thread_info_decoders), info);
}

static int
rpc_decode_threads_array(const struct spdk_json_val *val, void *out)
{
	struct rpc_threads *threads = out;

	return spdk_json_decode_array(val, rpc_decode_threads_object, threads->thread_info, RPC_MAX_THREADS,
				      &threads->threads_count, sizeof(struct rpc_thread_info));
}

static const struct spdk_json_object_decoder rpc_threads_stats_decoders[] = {
	{"tick_rate", offsetof(struct rpc_threads_stats, tick_rate), spdk_json_decode_uint64},
	{"threads", offsetof(struct rpc_threads_stats, threads), rpc_decode_threads_array},
};

static void
free_rpc_poller(struct rpc_poller_info *poller)
{
	free(poller->name);
	poller->name = NULL;
	free(poller->state);
	poller->state = NULL;
}

static void
free_rpc_pollers_stats(struct rpc_pollers_stats *req)
{
	struct rpc_poller_thread_info *thread;
	uint64_t i, j;

	for (i = 0; i < req->pollers_threads.threads_count; i++) {
		thread = &req->pollers_threads.threads[i];

		for (j = 0; j < thread->active_pollers.pollers_count; j++) {
			free_rpc_poller(&thread->active_pollers.pollers[j]);
		}

		for (j = 0; j < thread->timed_pollers.pollers_count; j++) {
			free_rpc_poller(&thread->timed_pollers.pollers[j]);
		}

		for (j = 0; j < thread->paused_pollers.pollers_count; j++) {
			free_rpc_poller(&thread->paused_pollers.pollers[j]);
		}

		free(thread->name);
		thread->name = NULL;
	}
}

static const struct spdk_json_object_decoder rpc_pollers_decoders[] = {
	{"name", offsetof(struct rpc_poller_info, name), spdk_json_decode_string},
	{"state", offsetof(struct rpc_poller_info, state), spdk_json_decode_string},
	{"run_count", offsetof(struct rpc_poller_info, run_count), spdk_json_decode_uint64},
	{"busy_count", offsetof(struct rpc_poller_info, busy_count), spdk_json_decode_uint64},
	{"period_ticks", offsetof(struct rpc_poller_info, period_ticks), spdk_json_decode_uint64, true},
};

static int
rpc_decode_pollers_object(const struct spdk_json_val *val, void *out)
{
	struct rpc_poller_info *info = out;

	return spdk_json_decode_object(val, rpc_pollers_decoders, SPDK_COUNTOF(rpc_pollers_decoders), info);
}

static int
rpc_decode_pollers_array(const struct spdk_json_val *val, void *out)
{
	struct rpc_pollers *pollers = out;

	return spdk_json_decode_array(val, rpc_decode_pollers_object, pollers->pollers, RPC_MAX_THREADS,
				      &pollers->pollers_count, sizeof(struct rpc_poller_info));
}

static const struct spdk_json_object_decoder rpc_pollers_threads_decoders[] = {
	{"name", offsetof(struct rpc_poller_thread_info, name), spdk_json_decode_string},
	{"active_pollers", offsetof(struct rpc_poller_thread_info, active_pollers), rpc_decode_pollers_array},
	{"timed_pollers", offsetof(struct rpc_poller_thread_info, timed_pollers), rpc_decode_pollers_array},
	{"paused_pollers", offsetof(struct rpc_poller_thread_info, paused_pollers), rpc_decode_pollers_array},
};

static int
rpc_decode_pollers_threads_object(const struct spdk_json_val *val, void *out)
{
	struct rpc_poller_thread_info *info = out;

	return spdk_json_decode_object(val, rpc_pollers_threads_decoders,
				       SPDK_COUNTOF(rpc_pollers_threads_decoders), info);
}

static int
rpc_decode_pollers_threads_array(const struct spdk_json_val *val, void *out)
{
	struct rpc_pollers_threads *pollers_threads = out;

	return spdk_json_decode_array(val, rpc_decode_pollers_threads_object, pollers_threads->threads,
				      RPC_MAX_THREADS, &pollers_threads->threads_count, sizeof(struct rpc_poller_thread_info));
}

static const struct spdk_json_object_decoder rpc_pollers_stats_decoders[] = {
	{"tick_rate", offsetof(struct rpc_pollers_stats, tick_rate), spdk_json_decode_uint64},
	{"threads", offsetof(struct rpc_pollers_stats, pollers_threads), rpc_decode_pollers_threads_array},
};

static int
rpc_send_req(char *rpc_name, struct spdk_jsonrpc_client_response **resp)
{
	struct spdk_jsonrpc_client_response *json_resp = NULL;
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_client_request *request;
	int rc;

	request = spdk_jsonrpc_client_create_request();
	if (request == NULL) {
		return -ENOMEM;
	}

	w = spdk_jsonrpc_begin_request(request, 1, rpc_name);
	spdk_jsonrpc_end_request(request, w);
	spdk_jsonrpc_client_send_request(g_rpc_client, request);

	do {
		rc = spdk_jsonrpc_client_poll(g_rpc_client, 1);
	} while (rc == 0 || rc == -ENOTCONN);

	if (rc <= 0) {
		return -1;
	}

	json_resp = spdk_jsonrpc_client_get_response(g_rpc_client);
	if (json_resp == NULL) {
		return -1;
	}

	/* Check for error response */
	if (json_resp->error != NULL) {
		return -1;
	}

	assert(json_resp->result);

	*resp = json_resp;

	return 0;
}

static int
get_data(void)
{
	struct spdk_jsonrpc_client_response *json_resp = NULL;
	int rc = 0;

	rc = rpc_send_req("thread_get_stats", &json_resp);
	if (rc) {
		goto end;
	}

	/* Decode json */
	memset(&g_threads_stats, 0, sizeof(g_threads_stats));
	if (spdk_json_decode_object(json_resp->result, rpc_threads_stats_decoders,
				    SPDK_COUNTOF(rpc_threads_stats_decoders), &g_threads_stats)) {
		rc = -EINVAL;
		goto end;
	}

	spdk_jsonrpc_client_free_response(json_resp);

	rc = rpc_send_req("thread_get_pollers", &json_resp);
	if (rc) {
		goto end;
	}

	/* Decode json */
	memset(&g_pollers_stats, 0, sizeof(g_pollers_stats));
	if (spdk_json_decode_object(json_resp->result, rpc_pollers_stats_decoders,
				    SPDK_COUNTOF(rpc_pollers_stats_decoders), &g_pollers_stats)) {
		rc = -EINVAL;
		goto end;
	}

end:
	spdk_jsonrpc_client_free_response(json_resp);
	return rc;
}

static void
free_data(void)
{
	free_rpc_threads_stats(&g_threads_stats);
	free_rpc_pollers_stats(&g_pollers_stats);
}

static void
print_max_len(WINDOW *win, int row, uint16_t col, uint16_t max_len, const char *string)
{
	const char dots[] = "...";
	int DOTS_STR_LEN = sizeof(dots) / sizeof(dots[0]);
	int len, max_col, max_str;
	int max_row __attribute__((unused));

	len = strlen(string);
	getmaxyx(win, max_row, max_col);

	assert(row < max_row);

	if (max_len != 0 && col + max_len < max_col) {
		max_col = col + max_len;
	}

	max_str = max_col - col;

	if (max_str <= DOTS_STR_LEN) {
		/* No space to print anything */
		return;
	}

	if (col + len > max_col - 1) {
		char tmp_str[MAX_STRING_LEN];

		snprintf(tmp_str, max_str - DOTS_STR_LEN - 1, "%s", string);
		snprintf(&tmp_str[max_str - DOTS_STR_LEN - 2], DOTS_STR_LEN, "%s", dots);
		mvwprintw(win, row, col, tmp_str);
	} else {
		if (max_len) {
			char tmp_str[max_str];

			snprintf(tmp_str, max_str, "%s%*c", string, max_len - len - 1, ' ');
			mvwprintw(win, row, col, tmp_str);
		} else {
			mvwprintw(win, row, col, string);
		}
	}
	refresh();
	wrefresh(win);
}

static void
draw_menu_win(void)
{
	wbkgd(g_menu_win, COLOR_PAIR(2));
	box(g_menu_win, 0, 0);
	print_max_len(g_menu_win, 1, 1, 0,
		      "   [q] Quit   |   [1-3] TAB selection   |   [PgUp] Previous page   |   [PgDown] Next page   |   [c] Columns   |   [s] Sorting");
}

static void
draw_tab_win(enum tabs tab)
{
	uint16_t col;
	uint8_t white_spaces = TABS_SPACING * NUMBER_OF_TABS;

	wbkgd(g_tab_win[tab], COLOR_PAIR(2));
	box(g_tab_win[tab], 0, 0);

	col = ((g_max_col - white_spaces) / NUMBER_OF_TABS / 2) - (strlen(g_tab_title[tab]) / 2) -
	      TABS_SPACING;
	print_max_len(g_tab_win[tab], 1, col, 0, g_tab_title[tab]);
}

static void
draw_tabs(enum tabs tab_index, uint8_t sort_col)
{
	struct col_desc *col_desc = g_col_desc[tab_index];
	WINDOW *tab = g_tabs[tab_index];
	int i, j;
	uint16_t offset;

	for (i = 0; col_desc[i].name != NULL; i++) {
		if (col_desc[i].disabled) {
			continue;
		}

		offset = 1;
		for (j = i; j != 0; j--) {
			offset += col_desc[j - 1].name_len + 1;
		}

		if (i == sort_col) {
			wattron(tab, COLOR_PAIR(3));
			print_max_len(tab, 1, offset, 0, col_desc[i].name);
			wattroff(tab, COLOR_PAIR(3));
		} else {
			print_max_len(tab, 1, offset, 0, col_desc[i].name);
		}

		if (col_desc[i + 1].name != NULL) {
			print_max_len(tab, 1, offset + col_desc[i].name_len, 0, "|");
		}
	}

	print_max_len(tab, 2, 1, 0, ""); /* Move to next line */
	whline(tab, ACS_HLINE, MAX_STRING_LEN);
	box(tab, 0, 0);
	wrefresh(tab);
}

static void
resize_interface(enum tabs tab)
{
	int i;

	clear();
	wclear(g_menu_win);
	mvwin(g_menu_win, g_max_row - MENU_WIN_SPACING, MENU_WIN_LOCATION_COL);
	wresize(g_menu_win, MENU_WIN_HEIGHT, g_max_col);
	draw_menu_win();

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		wclear(g_tabs[i]);
		wresize(g_tabs[i], g_max_row - MENU_WIN_HEIGHT - TAB_WIN_HEIGHT - 2, g_max_col);
		mvwin(g_tabs[i], TABS_LOCATION_ROW, TABS_LOCATION_COL);
		draw_tabs(i, g_current_sort_col[i]);
	}

	draw_tabs(tab, g_current_sort_col[tab]);

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		wclear(g_tab_win[i]);
		wresize(g_tab_win[i], TAB_WIN_HEIGHT,
			(g_max_col - (TABS_SPACING * NUMBER_OF_TABS)) / NUMBER_OF_TABS);
		mvwin(g_tab_win[i], TAB_WIN_LOCATION_ROW, 1 + (g_max_col / NUMBER_OF_TABS) * i);
		draw_tab_win(i);
	}

	update_panels();
	doupdate();
}

static void
switch_tab(enum tabs tab)
{
	top_panel(g_panels[tab]);
	update_panels();
	doupdate();
}

static int
sort_threads(const void *p1, const void *p2)
{
	const struct rpc_thread_info *thread_info1 = *(struct rpc_thread_info **)p1;
	const struct rpc_thread_info *thread_info2 = *(struct rpc_thread_info **)p2;
	uint64_t count1, count2;

	switch (g_current_sort_col[THREADS_TAB]) {
	case 0: /* Sort by name */
		return strcmp(thread_info1->name, thread_info2->name);
	case 1: /* Sort by active pollers number */
		count1 = thread_info1->active_pollers_count;
		count2 = thread_info2->active_pollers_count;
		break;
	case 2: /* Sort by timed pollers number */
		count1 = thread_info1->timed_pollers_count;
		count2 = thread_info2->timed_pollers_count;
		break;
	case 3: /* Sort by paused pollers number */
		count1 = thread_info1->paused_pollers_count;
		count2 = thread_info2->paused_pollers_count;
		break;
	default:
		return 0;
	}

	if (count2 > count1) {
		return 1;
	} else if (count2 < count1) {
		return -1;
	} else {
		return 0;
	}
}

static void
refresh_threads_tab(void)
{
	struct col_desc *col_desc = g_col_desc[THREADS_TAB];
	uint64_t i, threads_count;
	uint16_t j;
	uint16_t col;
	char pollers_number[MAX_POLLER_COUNT_STR];
	struct rpc_thread_info *thread_info[g_threads_stats.threads.threads_count];

	threads_count = g_threads_stats.threads.threads_count;

	/* Clear screen if number of threads changed */
	if (g_last_threads_count != threads_count) {
		for (i = TABS_DATA_START_ROW; i < g_data_win_size; i++) {
			for (j = 1; j < (uint64_t)g_max_col - 1; j++) {
				mvwprintw(g_tabs[THREADS_TAB], i, j, " ");
			}
		}

		g_last_threads_count = threads_count;
	}

	for (i = 0; i < threads_count; i++) {
		thread_info[i] = &g_threads_stats.threads.thread_info[i];
	}

	qsort(thread_info, threads_count, sizeof(thread_info[0]), sort_threads);

	for (i = 0; i < threads_count; i++) {
		col = TABS_DATA_START_COL;
		if (!col_desc[0].disabled) {
			print_max_len(g_tabs[THREADS_TAB], TABS_DATA_START_ROW + i, col, 0, thread_info[i]->name);
		}

		col += MAX_THREAD_NAME_DISP;
		if (!col_desc[1].disabled) {
			snprintf(pollers_number, MAX_POLLER_COUNT_STR, "%ld", thread_info[i]->active_pollers_count);
			print_max_len(g_tabs[THREADS_TAB], TABS_DATA_START_ROW + i, col, 0, pollers_number);
		}

		col += MAX_POLLER_COUNT_DISP;
		if (!col_desc[2].disabled) {
			snprintf(pollers_number, MAX_POLLER_COUNT_STR, "%ld", thread_info[i]->timed_pollers_count);
			print_max_len(g_tabs[THREADS_TAB], TABS_DATA_START_ROW + i, col, 0, pollers_number);
		}

		col += MAX_POLLER_COUNT_DISP;
		if (!col_desc[3].disabled) {
			snprintf(pollers_number, MAX_POLLER_COUNT_STR, "%ld", thread_info[i]->paused_pollers_count);
			print_max_len(g_tabs[THREADS_TAB], TABS_DATA_START_ROW + i, col, 0, pollers_number);
		}
	}
}

enum sort_type {
	BY_NAME,
	USE_GLOBAL,
};

static int
#ifdef __FreeBSD__
sort_pollers(void *arg, const void *p1, const void *p2)
#else
sort_pollers(const void *p1, const void *p2, void *arg)
#endif
{
	const struct rpc_poller_info *poller1 = *(struct rpc_poller_info **)p1;
	const struct rpc_poller_info *poller2 = *(struct rpc_poller_info **)p2;
	int rc;
	enum sort_type sorting = *(enum sort_type *)arg;

	if (sorting == BY_NAME) {
		/* Sorting by name requested explicitly */
		return strcmp(poller1->name, poller2->name);
	} else {
		/* Use globaly set sorting */
		switch (g_current_sort_col[POLLERS_TAB]) {
		case 0: /* Sort by name */
			rc = strcmp(poller1->name, poller2->name);
			break;
		case 1: /* Sort by type */
			rc = poller1->type - poller2->type;
			break;
		case 2: /* Sort by thread */
			rc = strcmp(poller1->thread_name, poller2->thread_name);
			break;
		default:
			rc = 0;
			break;
		}
	}

	return rc;
}

static void
copy_pollers(struct rpc_pollers *pollers, uint64_t pollers_count, enum spdk_poller_type type,
	     struct rpc_poller_thread_info *thread, uint64_t *current_count,
	     struct rpc_poller_info **pollers_info)
{
	uint64_t i;

	for (i = 0; i < pollers_count; i++) {
		pollers_info[*current_count] = &pollers->pollers[i];
		snprintf(pollers_info[*current_count]->thread_name, MAX_POLLER_NAME - 1, "%s", thread->name);
		pollers_info[(*current_count)++]->type = type;
	}
}

static void
refresh_pollers_tab(void)
{
	struct col_desc *col_desc = g_col_desc[POLLERS_TAB];
	struct rpc_poller_thread_info *thread;
	uint64_t i, count = 0;
	uint16_t col, j;
	enum sort_type sorting;
	struct rpc_poller_info *pollers[RPC_MAX_POLLERS];

	for (i = 0; i < g_pollers_stats.pollers_threads.threads_count; i++) {
		thread = &g_pollers_stats.pollers_threads.threads[i];

		copy_pollers(&thread->active_pollers, thread->active_pollers.pollers_count, SPDK_ACTIVE_POLLER,
			     thread, &count, pollers);
		copy_pollers(&thread->timed_pollers, thread->timed_pollers.pollers_count, SPDK_TIMED_POLLER, thread,
			     &count, pollers);
		copy_pollers(&thread->paused_pollers, thread->paused_pollers.pollers_count, SPDK_PAUSED_POLLER,
			     thread, &count, pollers);
	}

	/* Clear screen if number of pollers changed */
	if (g_last_pollers_count != count) {
		for (i = TABS_DATA_START_ROW; i < g_data_win_size; i++) {
			for (j = 1; j < (uint64_t)g_max_col - 1; j++) {
				mvwprintw(g_tabs[POLLERS_TAB], i, j, " ");
			}
		}

		g_last_pollers_count = count;
	}

	/* Timed pollers can switch their position on a list because of how they work.
	 * Let's sort them by name first so that they won't switch on data refresh */
	sorting = BY_NAME;
	qsort_r(pollers, count, sizeof(pollers[0]), sort_pollers, (void *)&sorting);
	sorting = USE_GLOBAL;
	qsort_r(pollers, count, sizeof(pollers[0]), sort_pollers, (void *)&sorting);

	/* Display info */
	for (i = 0; i < count; i++) {
		col = TABS_DATA_START_COL;

		if (!col_desc[0].disabled) {
			print_max_len(g_tabs[POLLERS_TAB], TABS_DATA_START_ROW + i, col, 0, pollers[i]->name);
		}

		col += MAX_POLLER_NAME_DISP;

		if (!col_desc[1].disabled) {
			print_max_len(g_tabs[POLLERS_TAB], TABS_DATA_START_ROW + i, col, 0,
				      poller_type_str[pollers[i]->type]);
		}

		col += MAX_POLLER_TYPE_DISP;

		if (!col_desc[2].disabled) {
			print_max_len(g_tabs[POLLERS_TAB], TABS_DATA_START_ROW + i, col, 0, pollers[i]->thread_name);
		}
	}
}

static void
refresh_cores_tab(void)
{

}

static void
refresh_tab(enum tabs tab)
{
	void (*refresh_function[NUMBER_OF_TABS])(void) = {refresh_threads_tab, refresh_pollers_tab, refresh_cores_tab};
	int color_pair[NUMBER_OF_TABS] = {COLOR_PAIR(2), COLOR_PAIR(2), COLOR_PAIR(2)};
	int i;

	color_pair[tab] = COLOR_PAIR(1);

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		wbkgd(g_tab_win[i], color_pair[i]);
	}

	(*refresh_function[tab])();
	refresh();

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		wrefresh(g_tab_win[i]);
	}
}

static void
print_in_middle(WINDOW *win, int starty, int startx, int width, char *string, chtype color)
{
	int length, temp;

	length = strlen(string);
	temp = (width - length) / 2;
	wattron(win, color);
	mvwprintw(win, starty, startx + temp, "%s", string);
	wattroff(win, color);
	refresh();
}

static void
apply_filters(enum tabs tab)
{
	wclear(g_tabs[tab]);
	draw_tabs(tab, g_current_sort_col[tab]);
}

static ITEM **
draw_filtering_menu(uint8_t position, WINDOW *filter_win, uint8_t tab, MENU **my_menu)
{
	const int ADDITIONAL_ELEMENTS = 3;
	const int ROW_PADDING = 6;
	const int WINDOW_START_X = 1;
	const int WINDOW_START_Y = 3;
	const int WINDOW_COLUMNS = 2;
	struct col_desc *col_desc = g_col_desc[tab];
	ITEM **my_items;
	MENU *menu;
	int i, elements;
	uint8_t len = 0;

	for (i = 0; col_desc[i].name != NULL; ++i) {
		len = spdk_max(col_desc[i].name_len, len);
	}

	elements = i;

	my_items = (ITEM **)calloc(elements * WINDOW_COLUMNS + ADDITIONAL_ELEMENTS, sizeof(ITEM *));

	for (i = 0; i < elements * 2; i++) {
		my_items[i] = new_item(col_desc[i / WINDOW_COLUMNS].name, NULL);
		i++;
		my_items[i] = new_item(col_desc[i / WINDOW_COLUMNS].disabled ? "[ ]" : "[*]", NULL);
	}

	my_items[i] = new_item("CLOSE", NULL);
	set_item_userptr(my_items[i], apply_filters);

	menu = new_menu((ITEM **)my_items);

	menu_opts_off(menu, O_SHOWDESC);
	set_menu_format(menu, elements + 1, WINDOW_COLUMNS);

	set_menu_win(menu, filter_win);
	set_menu_sub(menu, derwin(filter_win, elements + 1, len + ROW_PADDING, WINDOW_START_Y,
				  WINDOW_START_X));

	*my_menu = menu;

	post_menu(menu);
	refresh();
	wrefresh(filter_win);

	for (i = 0; i < position / WINDOW_COLUMNS; i++) {
		menu_driver(menu, REQ_DOWN_ITEM);
	}

	return my_items;
}

static void
delete_filtering_menu(MENU *my_menu, ITEM **my_items, uint8_t elements)
{
	int i;

	unpost_menu(my_menu);
	free_menu(my_menu);
	for (i = 0; i < elements * 2 + 2; ++i) {
		free_item(my_items[i]);
	}
	free(my_items);
}

static ITEM **
refresh_filtering_menu(MENU **my_menu, WINDOW *filter_win, uint8_t tab, ITEM **my_items,
		       uint8_t elements, uint8_t position)
{
	delete_filtering_menu(*my_menu, my_items, elements);
	return draw_filtering_menu(position, filter_win, tab, my_menu);
}

static void
filter_columns(uint8_t tab)
{
	const int WINDOW_HEADER_LEN = 5;
	const int WINDOW_BORDER_LEN = 8;
	const int WINDOW_HEADER_END_LINE = 2;
	const int WINDOW_COLUMNS = 2;
	struct col_desc *col_desc = g_col_desc[tab];
	PANEL *filter_panel;
	WINDOW *filter_win;
	ITEM **my_items;
	MENU *my_menu;
	int i, c, elements;
	bool stop_loop = false;
	ITEM *cur;
	void (*p)(enum tabs tab);
	uint8_t current_index, len = 0;

	for (i = 0; col_desc[i].name != NULL; ++i) {
		len = spdk_max(col_desc[i].name_len, len);
	}

	elements = i;

	filter_win = newwin(elements + WINDOW_HEADER_LEN, len + WINDOW_BORDER_LEN,
			    (g_max_row - elements - 1) / 2, (g_max_col - len) / 2);
	keypad(filter_win, TRUE);
	filter_panel = new_panel(filter_win);

	top_panel(filter_panel);
	update_panels();
	doupdate();

	box(filter_win, 0, 0);

	print_in_middle(filter_win, 1, 0, len + WINDOW_BORDER_LEN, "Filtering", COLOR_PAIR(3));
	mvwaddch(filter_win, WINDOW_HEADER_END_LINE, 0, ACS_LTEE);
	mvwhline(filter_win, WINDOW_HEADER_END_LINE, 1, ACS_HLINE, len + WINDOW_BORDER_LEN - 2);
	mvwaddch(filter_win, WINDOW_HEADER_END_LINE, len + WINDOW_BORDER_LEN - 1, ACS_RTEE);

	my_items = draw_filtering_menu(0, filter_win, tab, &my_menu);

	while (!stop_loop) {
		c = wgetch(filter_win);

		switch (c) {
		case KEY_DOWN:
			menu_driver(my_menu, REQ_DOWN_ITEM);
			break;
		case KEY_UP:
			menu_driver(my_menu, REQ_UP_ITEM);
			break;
		case 27: /* ESC */
		case 'q':
			stop_loop = true;
			break;
		case ' ': /* Space */
			cur = current_item(my_menu);
			current_index = item_index(cur) / WINDOW_COLUMNS;
			col_desc[current_index].disabled = !col_desc[current_index].disabled;
			my_items = refresh_filtering_menu(&my_menu, filter_win, tab, my_items, elements,
							  item_index(cur) + 1);
			break;
		case 10: /* Enter */
			cur = current_item(my_menu);
			current_index = item_index(cur) / WINDOW_COLUMNS;
			if (current_index == elements) {
				stop_loop = true;
				p = item_userptr(cur);
				p(tab);
			} else {
				col_desc[current_index].disabled = !col_desc[current_index].disabled;
				my_items = refresh_filtering_menu(&my_menu, filter_win, tab, my_items, elements,
								  item_index(cur) + 1);
			}
			break;
		}
		wrefresh(filter_win);
	}

	delete_filtering_menu(my_menu, my_items, elements);

	del_panel(filter_panel);
	delwin(filter_win);

	wclear(g_menu_win);
	draw_menu_win();
}

static void
sort_type(enum tabs tab, int item_index)
{
	g_current_sort_col[tab] = item_index;
	wclear(g_tabs[tab]);
	draw_tabs(tab, g_current_sort_col[tab]);
}

static void
change_sorting(uint8_t tab)
{
	const int WINDOW_HEADER_LEN = 4;
	const int WINDOW_BORDER_LEN = 3;
	const int WINDOW_START_X = 1;
	const int WINDOW_START_Y = 3;
	const int WINDOW_HEADER_END_LINE = 2;
	PANEL *sort_panel;
	WINDOW *sort_win;
	ITEM **my_items;
	MENU *my_menu;
	int i, c, elements;
	bool stop_loop = false;
	ITEM *cur;
	void (*p)(enum tabs tab, int item_index);
	uint8_t len = 0;

	for (i = 0; g_col_desc[tab][i].name != NULL; ++i) {
		len = spdk_max(len, g_col_desc[tab][i].name_len);
	}

	elements = i;

	my_items = (ITEM **)calloc(elements + 1, sizeof(ITEM *));

	for (i = 0; i < elements; ++i) {
		my_items[i] = new_item(g_col_desc[tab][i].name, NULL);
		set_item_userptr(my_items[i], sort_type);
	}

	my_menu = new_menu((ITEM **)my_items);

	menu_opts_off(my_menu, O_SHOWDESC);

	sort_win = newwin(elements + WINDOW_HEADER_LEN, len + WINDOW_BORDER_LEN, (g_max_row - elements) / 2,
			  (g_max_col - len) / 2);
	keypad(sort_win, TRUE);
	sort_panel = new_panel(sort_win);

	top_panel(sort_panel);
	update_panels();
	doupdate();

	set_menu_win(my_menu, sort_win);
	set_menu_sub(my_menu, derwin(sort_win, elements, len + 1, WINDOW_START_Y, WINDOW_START_X));
	box(sort_win, 0, 0);

	print_in_middle(sort_win, 1, 0, len + WINDOW_BORDER_LEN, "Sorting", COLOR_PAIR(3));
	mvwaddch(sort_win, WINDOW_HEADER_END_LINE, 0, ACS_LTEE);
	mvwhline(sort_win, WINDOW_HEADER_END_LINE, 1, ACS_HLINE, len + 1);
	mvwaddch(sort_win, WINDOW_HEADER_END_LINE, len + WINDOW_BORDER_LEN - 1, ACS_RTEE);

	post_menu(my_menu);
	refresh();
	wrefresh(sort_win);

	while (!stop_loop) {
		c = wgetch(sort_win);

		switch (c) {
		case KEY_DOWN:
			menu_driver(my_menu, REQ_DOWN_ITEM);
			break;
		case KEY_UP:
			menu_driver(my_menu, REQ_UP_ITEM);
			break;
		case 27: /* ESC */
			stop_loop = true;
			break;
		case 10: /* Enter */
			stop_loop = true;
			cur = current_item(my_menu);
			p = item_userptr(cur);
			p(tab, item_index(cur));
			break;
		}
		wrefresh(sort_win);
	}

	unpost_menu(my_menu);
	free_menu(my_menu);

	for (i = 0; i < elements; ++i) {
		free_item(my_items[i]);
	}

	free(my_items);

	del_panel(sort_panel);
	delwin(sort_win);

	wclear(g_menu_win);
	draw_menu_win();
}

static void
show_stats(void)
{
	const char *refresh_error = "ERROR occurred while getting data";
	int c, rc;
	int max_row, max_col;
	uint8_t active_tab = THREADS_TAB;

	switch_tab(THREADS_TAB);

	while (1) {
		/* Check if interface has to be resized (terminal size changed) */
		getmaxyx(stdscr, max_row, max_col);

		if (max_row != g_max_row || max_col != g_max_col) {
			g_max_row = max_row;
			g_max_col = max_col;
			g_data_win_size = g_max_row - MENU_WIN_HEIGHT - TAB_WIN_HEIGHT - TABS_DATA_START_ROW;
			resize_interface(active_tab);
		}

		c = getch();
		if (c == 'q') {
			break;
		}

		switch (c) {
		case '1':
		case '2':
		case '3':
			active_tab = c - '1';
			switch_tab(active_tab);
			break;
		case 's':
			change_sorting(active_tab);
			break;
		case 'c':
			filter_columns(active_tab);
			break;
		default:
			break;
		}

		rc = get_data();
		if (rc) {
			mvprintw(g_max_row - 1, g_max_col - strlen(refresh_error) - 2, refresh_error);
		}

		refresh_tab(active_tab);

		free_data();

		refresh();
	}
}

static void
draw_interface(void)
{
	int i;

	getmaxyx(stdscr, g_max_row, g_max_col);
	g_data_win_size = g_max_row - MENU_WIN_HEIGHT - TAB_WIN_HEIGHT - TABS_DATA_START_ROW;

	g_menu_win = newwin(MENU_WIN_HEIGHT, g_max_col, g_max_row - MENU_WIN_HEIGHT - 1,
			    MENU_WIN_LOCATION_COL);
	draw_menu_win();

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		g_tab_win[i] = newwin(TAB_WIN_HEIGHT, g_max_col / NUMBER_OF_TABS - TABS_SPACING,
				      TAB_WIN_LOCATION_ROW, g_max_col / NUMBER_OF_TABS * i + 1);
		draw_tab_win(i);

		g_tabs[i] = newwin(g_max_row - MENU_WIN_HEIGHT - TAB_WIN_HEIGHT - 2, g_max_col, TABS_LOCATION_ROW,
				   TABS_LOCATION_COL);
		draw_tabs(i, g_current_sort_col[i]);
		g_panels[i] = new_panel(g_tabs[i]);
	}

	update_panels();
	doupdate();
}

static void
setup_ncurses(void)
{
	clear();
	noecho();
	halfdelay(1);
	curs_set(0);
	start_color();
	init_pair(1, COLOR_BLACK, COLOR_GREEN);
	init_pair(2, COLOR_BLACK, COLOR_WHITE);
	init_pair(3, COLOR_YELLOW, COLOR_BLACK);
	init_pair(4, COLOR_BLACK, COLOR_YELLOW);

	if (has_colors() == FALSE) {
		endwin();
		printf("Your terminal does not support color\n");
		exit(1);
	}
}

static void
usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\n");
	printf("options:\n");
	printf(" -r <path>  RPC listen address (default: /var/tmp/spdk.sock\n");
	printf(" -h         show this usage\n");
}

int main(int argc, char **argv)
{
	int op;
	char *socket = SPDK_DEFAULT_RPC_ADDR;

	while ((op = getopt(argc, argv, "r:h")) != -1) {
		switch (op) {
		case 'r':
			socket = optarg;
			break;
		case 'H':
		default:
			usage(argv[0]);
			return 1;
		}
	}

	g_rpc_client = spdk_jsonrpc_client_connect(socket, AF_UNIX);
	if (!g_rpc_client) {
		fprintf(stderr, "spdk_jsonrpc_client_connect() failed: %d\n", errno);
		return 1;
	}

	initscr();
	init_str_len();
	setup_ncurses();
	draw_interface();
	show_stats();

	/* End curses mode */
	endwin();

	spdk_jsonrpc_client_close(g_rpc_client);

	return (0);
}
