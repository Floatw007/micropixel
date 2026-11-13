#!/usr/bin/env python3
"""Apply the P4-specific synchronous RPC handoff fix to ESP-Hosted 2.12.13."""

from pathlib import Path
import re
import sys


def replace_once(source: str, old: str, new: str, label: str) -> str:
    count = source.count(old)
    if count != 1:
        raise RuntimeError(f"expected one {label} match, found {count}")
    return source.replace(old, new, 1)


PATCH_MARKER = "MICROPIXEL_ESP_HOSTED_SYNC_RPC_RESPONSE_LIFETIME_V2"
POST_SYNC_RESPONSE_V2 = """/* MICROPIXEL_ESP_HOSTED_SYNC_RPC_RESPONSE_LIFETIME_V2: keep the waiter semaphore alive through wakeup. */
static bool post_sync_response(ctrl_cmd_t *app_resp)
{
	int i;
	bool found = false;
	bool duplicate = false;
	bool wrong_msg_id = false;
	bool sem_post_failed = false;

	if (!app_resp || app_resp->msg_id < RPC_ID__Resp_Base || app_resp->msg_id >= RPC_ID__Resp_Max) {
		return false;
	}

	g_h.funcs->_h_lock_mutex(sync_rsp_table_mutex, HOSTED_BLOCK_MAX);
	for (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
		if (sync_rsp_table[i].uid != app_resp->uid || !sync_rsp_table[i].sem) {
			continue;
		}
		found = true;
		if (sync_rsp_table[i].response) {
			duplicate = true;
		} else {
			wrong_msg_id = app_resp->msg_id != sync_rsp_table[i].expected_msg_id;
			if (wrong_msg_id) {
				/* Keep the actual ID so cleanup matches the parsed payload union. */
				app_resp->resp_event_status = RPC_ERR_PROTOBUF_DECODE;
			}
			sync_rsp_table[i].response = app_resp;
			sem_post_failed = g_h.funcs->_h_post_semaphore(sync_rsp_table[i].sem) != 0;
		}
		break;
	}
	g_h.funcs->_h_unlock_mutex(sync_rsp_table_mutex);

	if (!found) {
		return false;
	}
	if (duplicate) {
		ESP_LOGW(TAG, "Dropping duplicate sync response uid=%" PRIu32, app_resp->uid);
		return false;
	}
	if (wrong_msg_id) {
		ESP_LOGE(TAG, "RPC response type mismatch for uid=%" PRIu32 "; reporting decode error", app_resp->uid);
	}
	if (sem_post_failed) {
		ESP_LOGW(TAG, "Could not post sync response semaphore for uid=%" PRIu32, app_resp->uid);
	}
	return true;
}"""


def replace_post_sync_response(source: str) -> str:
    pattern = re.compile(r"static bool post_sync_response\(ctrl_cmd_t \*app_resp\)\n\{.*?\n\}", re.S)
    matches = list(pattern.finditer(source))
    if len(matches) != 1:
        raise RuntimeError(f"expected one sync response handler, found {len(matches)}")
    match = matches[0]
    return source[:match.start()] + POST_SYNC_RESPONSE_V2 + source[match.end():]


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_esp_hosted_sync_rpc.py <esp_hosted-component-dir>")

    component = Path(sys.argv[1]).resolve()
    manifest = (component / "idf_component.yml").read_text(encoding="utf-8")
    source_path = component / "host/drivers/rpc/core/rpc_core.c"
    source = source_path.read_text(encoding="utf-8")

    if PATCH_MARKER in source:
        print("ESP-Hosted synchronous RPC handoff patch already applied")
        return 0

    if "static bool post_sync_response(ctrl_cmd_t *app_resp)" in source:
        source = replace_post_sync_response(source)
        source_path.write_text(source, encoding="utf-8")
        print("Updated ESP-Hosted synchronous RPC semaphore lifetime fix")
        return 0

    if not re.search(r"(?m)^version:\s*2\.12\.13\s*$", manifest):
        raise RuntimeError("the checked-in RPC patch is only reviewed for ESP-Hosted 2.12.13")

    source = replace_once(
        source,
        """typedef struct {
\tuint32_t uid;
\tvoid * sem;
} sync_rsp_t;""",
        """typedef struct {
\tuint32_t uid;
\tvoid * sem;
\tuint32_t expected_msg_id;
\tctrl_cmd_t *response;
} sync_rsp_t;""",
        "sync response table fields",
    )
    source = replace_once(
        source,
        "static int is_sync_resp_sem_available(uint32_t uid);\n",
        "",
        "sync semaphore availability declaration",
    )
    source = replace_once(
        source,
        "static int post_sync_resp_sem(ctrl_cmd_t *app_resp);",
        "static bool post_sync_response(ctrl_cmd_t *app_resp);",
        "sync response handoff declaration",
    )
    source = replace_once(
        source,
        "static int wait_for_sync_response(ctrl_cmd_t *app_req);",
        "static int wait_for_sync_response(ctrl_cmd_t *app_req, ctrl_cmd_t **response_out);",
        "sync response wait declaration",
    )
    source = replace_once(
        source,
        """\tesp_queue_elem_t elem = {0};
\tctrl_cmd_t *app_resp = NULL;
\tctrl_cmd_t *app_event = NULL;""",
        """\tctrl_cmd_t *app_resp = NULL;
\tctrl_cmd_t *app_event = NULL;""",
        "receive queue local",
    )
    source = replace_once(
        source,
        """\t\t/* Is callback is available,
\t\t * progress as async response */
\t\tif (CALLBACK_AVAILABLE ==
\t\t\tis_async_resp_callback_available(app_resp)) {

\t\t\t/* User registered RPC async response callback
\t\t\t * function is available for this proto_msg,
\t\t\t * so call to that function should be done and
\t\t\t * return to select
\t\t\t */
\t\t\tcall_async_resp_callback(app_resp);
\t\t} else {

\t\t\t/* as RPC async response callback function is
\t\t\t * NOT available/registered, treat this response as
\t\t\t * synchronous response. forward this response to app
\t\t\t * using 'esp_queue' and help of semaphore
\t\t\t **/


\t\t\t/* User is RESPONSIBLE to free memory from
\t\t\t * app_resp in case of async callbacks NOT provided
\t\t\t * to free memory, please refer CLEANUP_APP_MSG macro
\t\t\t **/
\t\t\telem.buf = app_resp;
\t\t\telem.buf_len = sizeof(ctrl_cmd_t);

\t\t\tif (g_h.funcs->_h_queue_item(rpc_rx_q, &elem, HOSTED_BLOCK_MAX)) {
\t\t\t\tESP_LOGE(TAG, "RPC Q put fail");
\t\t\t\tgoto free_buffers;
\t\t\t}

\t\t\t/* Call up rx ind to unblock caller */
\t\t\tif (CALLBACK_AVAILABLE == is_sync_resp_sem_available(app_resp->uid))
\t\t\t\tpost_sync_resp_sem(app_resp);
\t\t}
""",
        """\t\t/* Is callback is available,
\t\t * progress as async response */
\t\tif (CALLBACK_AVAILABLE ==
\t\t\tis_async_resp_callback_available(app_resp)) {

\t\t\tcall_async_resp_callback(app_resp);
\t\t} else if (!post_sync_response(app_resp)) {
\t\t\tESP_LOGW(TAG, "Dropping sync response without waiter: uid=%" PRIu32 " msg_id=0x%x",
\t\t\t\t\tapp_resp->uid, app_resp->msg_id);
\t\t\tgoto free_buffers;
\t\t}
""",
        "sync response receive routing",
    )
    source = replace_once(
        source,
        """\t\t// same as process_rpc_rx_msg() for failed condition
\t\tesp_queue_elem_t elem = {0};
\t\telem.buf = app_resp;
\t\telem.buf_len = sizeof(ctrl_cmd_t);

\t\tif (g_h.funcs->_h_queue_item(rpc_rx_q, &elem, HOSTED_BLOCK_MAX)) {
\t\t\tESP_LOGE(TAG, "RPC Q put fail");
\t\t} else if (CALLBACK_AVAILABLE == is_sync_resp_sem_available(app_resp->uid)) {
\t\t\tESP_LOGV(TAG, "trigger semaphore to react to failed message uid %ld", app_resp->uid);
\t\t\tpost_sync_resp_sem(app_resp);
\t\t} else {
\t\t\tESP_LOGE(TAG, "no sync resp callback to react to failed message uid %ld", app_resp->uid);
\t\t}
""",
        """\t\t/* Wake the matching synchronous caller with this transport error. */
\t\tapp_resp->uid = app_req->uid;
\t\tif (!post_sync_response(app_resp)) {
\t\t\tESP_LOGE(TAG, "could not deliver failed sync request uid=%" PRIu32, app_req->uid);
\t\t\tCLEANUP_APP_MSG(app_resp);
\t\t}
""",
        "synchronous transmit failure routing",
    )
    source = replace_once(
        source,
        """static ctrl_cmd_t * get_response(int *read_len, ctrl_cmd_t *app_req)
{
\tuint8_t * buf = NULL;
\tesp_queue_elem_t elem = {0};
\tint ret = 0;

\t/* Any problems in response, return NULL */
\tif (!read_len || !app_req) {
\t\tESP_LOGE(TAG, "Invalid input parameter");
\t\treturn NULL;
\t}


\t/* Wait for response */
\tret = wait_for_sync_response(app_req);
\tif (ret) {
\t\tif ((ret == RET_FAIL_TIMEOUT) || (errno == ETIMEDOUT))
\t\t\tESP_LOGW(TAG, "Timeout waiting for Resp for [0x%x](%s)", app_req->msg_id, rpc_id_name(app_req->msg_id));
\t\telse
\t\t\tESP_LOGE(TAG, "ERR [%u] ret[%d] for [0x%x](%s)", errno, ret, app_req->msg_id, rpc_id_name(app_req->msg_id));
\t\treturn NULL;
\t}

\t/* Fetch response from `esp_queue` */
\tif (g_h.funcs->_h_dequeue_item(rpc_rx_q, &elem, HOSTED_BLOCK_MAX)) {
\t\tESP_LOGE(TAG, "rpc Rx Q Failed to dequeue");
\t\treturn NULL;
\t}

\tif (elem.buf_len) {

\t\t*read_len = elem.buf_len;
\t\tbuf = elem.buf;
\t\treturn (ctrl_cmd_t*)buf;

\t} else {
\t\tESP_LOGE(TAG, "rpc Q empty or uninitialised");
\t\treturn NULL;
\t}

\treturn NULL;
}""",
        """static ctrl_cmd_t * get_response(int *read_len, ctrl_cmd_t *app_req)
{
\tctrl_cmd_t *response = NULL;
\tint ret = 0;

\tif (!read_len || !app_req) {
\t\tESP_LOGE(TAG, "Invalid input parameter");
\t\treturn NULL;
\t}

\tret = wait_for_sync_response(app_req, &response);
\tif (ret) {
\t\tif ((ret == RET_FAIL_TIMEOUT) || (errno == ETIMEDOUT))
\t\t\tESP_LOGW(TAG, "Timeout waiting for Resp for [0x%x](%s)", app_req->msg_id, rpc_id_name(app_req->msg_id));
\t\telse
\t\t\tESP_LOGE(TAG, "ERR [%u] ret[%d] for [0x%x](%s)", errno, ret, app_req->msg_id, rpc_id_name(app_req->msg_id));
\t\treturn NULL;
\t}

\tif (!response) {
\t\tESP_LOGE(TAG, "No UID-matched RPC response for uid=%" PRIu32, app_req->uid);
\t\treturn NULL;
\t}

\t*read_len = sizeof(*response);
\treturn response;
}""",
        "synchronous response retrieval",
    )
    source = replace_once(
        source,
        """static int post_sync_resp_sem(ctrl_cmd_t *app_resp)
{
\tint i;

\t// msg_id of RPC_ID__Resp_Base now means Invalid RPC Request
\tif ((app_resp->msg_id < RPC_ID__Resp_Base) ||
\t\t(app_resp->msg_id >= RPC_ID__Resp_Max)) {
\t\treturn MSG_ID_OUT_OF_ORDER;
\t}

\tg_h.funcs->_h_lock_mutex(sync_rsp_table_mutex, HOSTED_BLOCK_MAX);
\tfor (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
\t\tif (sync_rsp_table[i].uid == app_resp->uid && sync_rsp_table[i].sem) {
\t\t\tint ret = g_h.funcs->_h_post_semaphore(sync_rsp_table[i].sem);
\t\t\tg_h.funcs->_h_unlock_mutex(sync_rsp_table_mutex);
\t\t\treturn ret;
\t\t}
\t}
\tg_h.funcs->_h_unlock_mutex(sync_rsp_table_mutex);

\treturn CALLBACK_NOT_REGISTERED;
}""",
        """static bool post_sync_response(ctrl_cmd_t *app_resp)
{
\tint i;
\tvoid *response_sem = NULL;
\tbool duplicate = false;
\tbool wrong_msg_id = false;

\tif (!app_resp || app_resp->msg_id < RPC_ID__Resp_Base || app_resp->msg_id >= RPC_ID__Resp_Max) {
\t\treturn false;
\t}

\tg_h.funcs->_h_lock_mutex(sync_rsp_table_mutex, HOSTED_BLOCK_MAX);
\tfor (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
\t\tif (sync_rsp_table[i].uid != app_resp->uid || !sync_rsp_table[i].sem) {
\t\t\tcontinue;
\t\t}
\t\tif (sync_rsp_table[i].response) {
\t\t\tduplicate = true;
\t\t} else {
\t\t\twrong_msg_id = app_resp->msg_id != sync_rsp_table[i].expected_msg_id;
\t\t\tif (wrong_msg_id) {
\t\t\t\tapp_resp->msg_id = sync_rsp_table[i].expected_msg_id;
\t\t\t\tapp_resp->resp_event_status = RPC_ERR_PROTOBUF_DECODE;
\t\t\t}
\t\t\tsync_rsp_table[i].response = app_resp;
\t\t\tresponse_sem = sync_rsp_table[i].sem;
\t\t}
\t\tbreak;
\t}
\tg_h.funcs->_h_unlock_mutex(sync_rsp_table_mutex);

\tif (!response_sem) {
\t\tif (duplicate) {
\t\t\tESP_LOGW(TAG, "Dropping duplicate sync response uid=%" PRIu32, app_resp->uid);
\t\t}
\t\treturn false;
\t}

\tif (wrong_msg_id) {
\t\tESP_LOGE(TAG, "RPC response type mismatch for uid=%" PRIu32 "; reporting decode error", app_resp->uid);
\t}
\tif (g_h.funcs->_h_post_semaphore(response_sem)) {
\t\tESP_LOGW(TAG, "Could not post sync response semaphore for uid=%" PRIu32, app_resp->uid);
\t}
\treturn true;
}""",
        "per-UID synchronous response handoff",
    )
    source = replace_once(
        source,
        """\t\t\t\tsync_rsp_table[i].uid = app_req->uid;
\t\t\t\tsync_rsp_table[i].sem = app_req->rx_sem;""",
        """\t\t\t\tsync_rsp_table[i].uid = app_req->uid;
\t\t\t\tsync_rsp_table[i].sem = app_req->rx_sem;
\t\t\t\tsync_rsp_table[i].expected_msg_id = exp_resp_msg_id;
\t\t\t\tsync_rsp_table[i].response = NULL;""",
        "sync response registration fields",
    )
    source = replace_once(
        source,
        """\t\t\tsync_rsp_table[i].uid = 0;
\t\t\tsync_rsp_table[i].sem = NULL;
\t\t\tbreak;""",
        """\t\t\tsync_rsp_table[i].uid = 0;
\t\t\tsync_rsp_table[i].sem = NULL;
\t\t\tsync_rsp_table[i].expected_msg_id = 0;
\t\t\tsync_rsp_table[i].response = NULL;
\t\t\tbreak;""",
        "sync response cleanup fields",
    )
    source = replace_once(
        source,
        """static int wait_for_sync_response(ctrl_cmd_t *app_req)
{
\tint timeout_sec = 0;
\tint exp_resp_msg_id = 0;
\tint ret = 0;
\tint i;""",
        """static int wait_for_sync_response(ctrl_cmd_t *app_req, ctrl_cmd_t **response_out)
{
\tint timeout_sec = 0;
\tint exp_resp_msg_id = 0;
\tint ret = 0;
\tint i;""",
        "sync response wait signature",
    )
    source = replace_once(
        source,
        """static int wait_for_sync_response(ctrl_cmd_t *app_req, ctrl_cmd_t **response_out)
{
\tint timeout_sec = 0;
\tint exp_resp_msg_id = 0;
\tint ret = 0;
\tint i;

\t/* If timeout not specified, use default */
\tif (!app_req->rsp_timeout_sec)
\t\ttimeout_sec = DEFAULT_RPC_RSP_TIMEOUT;""",
        """static int wait_for_sync_response(ctrl_cmd_t *app_req, ctrl_cmd_t **response_out)
{
\tint timeout_sec = 0;
\tint exp_resp_msg_id = 0;
\tint ret = 0;
\tint i;

\tif (!app_req || !response_out) {
\t\treturn FAILURE;
\t}
\t*response_out = NULL;

\t/* If timeout not specified, use default */
\tif (!app_req->rsp_timeout_sec)
\t\ttimeout_sec = DEFAULT_RPC_RSP_TIMEOUT;""",
        "sync response wait output initialization",
    )
    source = replace_once(
        source,
        """\t\t// search the table again to make sure we clear the correct entry
\t\tfor (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
\t\t\tif (sync_rsp_table[i].uid == app_req->uid) {
\t\t\t\t// clear table entry
\t\t\t\tsync_rsp_table[i].uid = 0;
\t\t\t\tsync_rsp_table[i].sem = NULL;
\t\t\t\tentry_cleared = true;
\t\t\t\tbreak;
\t\t\t}
\t\t}
\t\tg_h.funcs->_h_unlock_mutex(sync_rsp_table_mutex);
\t\tif (entry_cleared && g_h.funcs->_h_destroy_semaphore(local_sem)) {
\t\t\tESP_LOGE(TAG, "read sem rx for resp[0x%x] destroy failed", exp_resp_msg_id);
\t\t}
\t\treturn ret;""",
        """\t\t// Take only this UID's response before releasing the table entry.
\t\tfor (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
\t\t\tif (sync_rsp_table[i].uid == app_req->uid) {
\t\t\t\t*response_out = sync_rsp_table[i].response;
\t\t\t\tsync_rsp_table[i].uid = 0;
\t\t\t\tsync_rsp_table[i].sem = NULL;
\t\t\t\tsync_rsp_table[i].expected_msg_id = 0;
\t\t\t\tsync_rsp_table[i].response = NULL;
\t\t\t\tentry_cleared = true;
\t\t\t\tbreak;
\t\t\t}
\t\t}
\t\tg_h.funcs->_h_unlock_mutex(sync_rsp_table_mutex);
\t\tif (entry_cleared && g_h.funcs->_h_destroy_semaphore(local_sem)) {
\t\t\tESP_LOGE(TAG, "read sem rx for resp[0x%x] destroy failed", exp_resp_msg_id);
\t\t}
\t\tif (*response_out) {
\t\t\treturn SUCCESS;
\t\t}
\t\treturn ret ? ret : CALLBACK_NOT_REGISTERED;""",
        "UID-matched response take and cleanup",
    )
    source = replace_once(
        source,
        "static int is_sync_resp_sem_available(uint32_t uid)\n{\n\tint i;\n\n\tg_h.funcs->_h_lock_mutex(sync_rsp_table_mutex, HOSTED_BLOCK_MAX);\n\tfor (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {\n\t\tif (sync_rsp_table[i].uid == uid) {\n\t\t\tg_h.funcs->_h_unlock_mutex(sync_rsp_table_mutex);\n\t\t\treturn CALLBACK_AVAILABLE;\n\t\t}\n\t}\n\tg_h.funcs->_h_unlock_mutex(sync_rsp_table_mutex);\n\treturn CALLBACK_NOT_REGISTERED;\n}\n\n",
        "",
        "obsolete sync semaphore lookup",
    )

    source = replace_post_sync_response(source)
    source_path.write_text(source, encoding="utf-8")
    print("Applied ESP-Hosted 2.12.13 synchronous RPC UID handoff patch")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # configure must stop if upstream source drifted
        print(f"ESP-Hosted RPC patch failed: {error}", file=sys.stderr)
        raise SystemExit(1)
