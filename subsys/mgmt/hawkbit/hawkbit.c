/*
 * Copyright (c) 2016-2017 Linaro Limited
 * Copyright (c) 2018 Open Source Foundries Limited
 * Copyright (c) 2018 Foundries.io
 * Copyright (c) 2020 Linumiz
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>

LOG_MODULE_REGISTER(hawkbit);

#include <stdio.h>
#include <zephyr.h>
#include <string.h>
#include <stdlib.h>
#include <fs/nvs.h>
#include <data/json.h>
#include <net/net_ip.h>
#include <net/socket.h>
#include <net/net_mgmt.h>
#include <power/reboot.h>
#include <drivers/flash.h>
#include <net/http_client.h>
#include <net/dns_resolve.h>
#include <logging/log_ctrl.h>
#include <storage/flash_map.h>

#include "hawkbit_priv.h"
#include "hawkbit_device.h"
#include "mgmt/hawkbit.h"
#include "hawkbit_firmware.h"

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
#define CA_CERTIFICATE_TAG 1
#include <net/tls_credentials.h>
#endif

#define ADDRESS_ID 1

#define CANCEL_BASE_SIZE 50
#define RECV_BUFFER_SIZE 640
#define URL_BUFFER_SIZE 300
#define STATUS_BUFFER_SIZE 200
#define DOWNLOAD_HTTP_SIZE 200
#define DEPLOYMENT_BASE_SIZE 50
#define RESPONSE_BUFFER_SIZE 1100
#define NETWORK_TIMEOUT (2 * MSEC_PER_SEC)
#define HAWKBIT_RECV_TIMEOUT (300 * MSEC_PER_SEC)

#define SLOT1_SIZE FLASH_AREA_SIZE(image_1)
#define HTTP_HEADER_CONTENT_TYPE_JSON "application/json;charset=UTF-8"

#if ((CONFIG_HAWKBIT_POLL_INTERVAL > 1)	\
	&& (CONFIG_HAWKBIT_POLL_INTERVAL < 43200))
static uint32_t poll_sleep = (CONFIG_HAWKBIT_POLL_INTERVAL * 60 * MSEC_PER_SEC);
#else
static uint32_t poll_sleep = (300 * MSEC_PER_SEC);
#endif

static struct nvs_fs fs;

struct hawkbit_download {
	int download_status;
	int download_progress;
	size_t downloaded_size;
	size_t http_content_size;
};

static struct hawkbit_context {
	int sock;
	int32_t action_id;
	uint8_t *response_data;
	int32_t json_action_id;
	struct k_sem semaphore;
	size_t url_buffer_size;
	size_t status_buffer_size;
	struct hawkbit_download dl;
	struct http_request http_req;
	struct flash_img_context flash_ctx;
	uint8_t url_buffer[URL_BUFFER_SIZE];
	uint8_t status_buffer[STATUS_BUFFER_SIZE];
	uint8_t recv_buf_tcp[RECV_BUFFER_SIZE];
	enum hawkbit_response code_status;
} hb_context;

static union {
	struct hawkbit_dep_res dep;
	struct hawkbit_ctl_res base;
	struct hawkbit_cancel cancel;
} hawkbit_results;

static struct k_work_delayable hawkbit_work_handle;

static const struct json_obj_descr json_href_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_href, href, JSON_TOK_STRING),
};

static const struct json_obj_descr json_status_result_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_status_result, finished,
			    JSON_TOK_STRING),
};

static const struct json_obj_descr json_status_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_status, execution, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_status, result,
			      json_status_result_descr),
};

static const struct json_obj_descr json_ctl_res_sleep_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_ctl_res_sleep, sleep,
			    JSON_TOK_STRING),
};

static const struct json_obj_descr json_ctl_res_polling_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res_polling, polling,
			      json_ctl_res_sleep_descr),
};

static const struct json_obj_descr json_ctl_res_links_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res_links, deploymentBase,
			      json_href_descr),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res_links, cancelAction,
			      json_href_descr),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res_links, configData,
			      json_href_descr),
};

static const struct json_obj_descr json_ctl_res_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res, config,
			      json_ctl_res_polling_descr),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_ctl_res, _links,
			      json_ctl_res_links_descr),
};

static const struct json_obj_descr json_cfg_data_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_cfg_data, VIN, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_cfg_data, hwRevision,
			    JSON_TOK_STRING),
};

static const struct json_obj_descr json_cfg_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_cfg, mode, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_cfg, data, json_cfg_data_descr),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_cfg, id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_cfg, time, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_cfg, status, json_status_descr),
};

static const struct json_obj_descr json_close_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_close, id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_close, time, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_close, status, json_status_descr),
};

static const struct json_obj_descr json_dep_res_hashes_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_hashes, sha1,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_hashes, md5,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_hashes, sha256,
			    JSON_TOK_STRING),
};

static const struct json_obj_descr json_dep_res_links_descr[] = {
	JSON_OBJ_DESCR_OBJECT_NAMED(struct hawkbit_dep_res_links,
				    "download-http", download_http,
				    json_href_descr),
	JSON_OBJ_DESCR_OBJECT_NAMED(struct hawkbit_dep_res_links, "md5sum-http",
				    md5sum_http, json_href_descr),
};

static const struct json_obj_descr json_dep_res_arts_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_arts, filename,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_res_arts, hashes,
			      json_dep_res_hashes_descr),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_arts, size, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_res_arts, _links,
			      json_dep_res_links_descr),
};

static const struct json_obj_descr json_dep_res_chunk_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_chunk, part,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_chunk, version,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_chunk, name,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJ_ARRAY(struct hawkbit_dep_res_chunk, artifacts,
				 HAWKBIT_DEP_MAX_CHUNK_ARTS, num_artifacts,
				 json_dep_res_arts_descr,
				 ARRAY_SIZE(json_dep_res_arts_descr)),
};

static const struct json_obj_descr json_dep_res_deploy_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_deploy, download,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res_deploy, update,
			    JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJ_ARRAY(struct hawkbit_dep_res_deploy, chunks,
				 HAWKBIT_DEP_MAX_CHUNKS, num_chunks,
				 json_dep_res_chunk_descr,
				 ARRAY_SIZE(json_dep_res_chunk_descr)),
};

static const struct json_obj_descr json_dep_res_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_res, id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_res, deployment,
			      json_dep_res_deploy_descr),
};

static const struct json_obj_descr json_dep_fbk_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct hawkbit_dep_fbk, id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT(struct hawkbit_dep_fbk, status,
			      json_status_descr),
};

static bool start_http_client(void)
{
	int ret = -1;
	struct addrinfo *addr;
	struct addrinfo hints;
	int resolve_attempts = 10;

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
	int protocol = IPPROTO_TLS_1_2;
#else
	int protocol = IPPROTO_TCP;
#endif

	if (IS_ENABLED(CONFIG_NET_IPV6)) {
		hints.ai_family = AF_INET6;
	} else if (IS_ENABLED(CONFIG_NET_IPV4)) {
		hints.ai_family = AF_INET;
	}

	hints.ai_socktype = SOCK_STREAM;

	while (resolve_attempts--) {
		ret = getaddrinfo(CONFIG_HAWKBIT_SERVER, CONFIG_HAWKBIT_PORT,
				  &hints, &addr);
		if (ret == 0) {
			break;
		}

		k_sleep(K_MSEC(1));
	}

	if (ret != 0) {
		LOG_ERR("Could not resolve dns");
		return false;
	}

	hb_context.sock = socket(addr->ai_family, SOCK_STREAM, protocol);
	if (hb_context.sock < 0) {
		LOG_ERR("Failed to create TCP socket");
		goto err;
	}

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
	sec_tag_t sec_tag_opt[] = {
		CA_CERTIFICATE_TAG,
	};

	if (setsockopt(hb_context.sock, SOL_TLS, TLS_SEC_TAG_LIST, sec_tag_opt,
		       sizeof(sec_tag_opt)) < 0) {
		LOG_ERR("Failed to set TLS_TAG option");
		goto err_sock;
	}

	if (setsockopt(hb_context.sock, SOL_TLS, TLS_HOSTNAME,
		       CONFIG_HAWKBIT_SERVER,
		       sizeof(CONFIG_HAWKBIT_SERVER)) < 0) {
		goto err_sock;
	}
#endif

	if (connect(hb_context.sock, addr->ai_addr, addr->ai_addrlen) < 0) {
		LOG_ERR("Failed to connect to Server");
		goto err_sock;
	}

	freeaddrinfo(addr);
	return true;

err_sock:
	close(hb_context.sock);
err:
	freeaddrinfo(addr);
	return false;
}

static void cleanup_connection(void)
{
	if (close(hb_context.sock) < 0) {
		LOG_ERR("Could not close the socket");
	}
}

static int hawkbit_time2sec(const char *s)
{
	int sec;

	/* Time: HH:MM:SS */
	sec = strtol(s, NULL, 10) * (60 * 60);
	sec += strtol(s + 3, NULL, 10) * 60;
	sec += strtol(s + 6, NULL, 10);

	if (sec < 0) {
		return -1;
	} else {
		return sec;
	}
}

static const char *hawkbit_status_finished(enum hawkbit_status_fini f)
{
	switch (f) {
	case HAWKBIT_STATUS_FINISHED_SUCCESS:
		return "success";
	case HAWKBIT_STATUS_FINISHED_FAILURE:
		return "failure";
	case HAWKBIT_STATUS_FINISHED_NONE:
		return "none";
	default:
		LOG_ERR("%d is invalid", (int)f);
		return NULL;
	}
}

static const char *hawkbit_status_execution(enum hawkbit_status_exec e)
{
	switch (e) {
	case HAWKBIT_STATUS_EXEC_CLOSED:
		return "closed";
	case HAWKBIT_STATUS_EXEC_PROCEEDING:
		return "proceeding";
	case HAWKBIT_STATUS_EXEC_CANCELED:
		return "canceled";
	case HAWKBIT_STATUS_EXEC_SCHEDULED:
		return "scheduled";
	case HAWKBIT_STATUS_EXEC_REJECTED:
		return "rejected";
	case HAWKBIT_STATUS_EXEC_RESUMED:
		return "resumed";
	case HAWKBIT_STATUS_EXEC_NONE:
		return "none";
	default:
		LOG_ERR("%d is invalid", (int)e);
		return NULL;
	}
}

static int hawkbit_device_acid_update(int32_t new_value)
{
	int ret;

	ret = nvs_write(&fs, ADDRESS_ID, &new_value,
			sizeof(new_value));
	if (ret < 0) {
		LOG_ERR("Failed to write device id");
		return -EIO;
	}

	return 0;
}

/*
 * Update sleep interval, based on results from hawkbit base polling
 * resource
 */
static void hawkbit_update_sleep(struct hawkbit_ctl_res *hawkbit_res)
{
	uint32_t sleep_time;
	const char *sleep = hawkbit_res->config.polling.sleep;

	if (strlen(sleep) != HAWKBIT_SLEEP_LENGTH) {
		LOG_ERR("Invalid poll sleep: %s", sleep);
	} else {
		sleep_time = hawkbit_time2sec(sleep);
		if (sleep_time > 0 && poll_sleep !=
		    (MSEC_PER_SEC * sleep_time)) {
			LOG_DBG("New poll sleep %d seconds", sleep_time);
			poll_sleep = sleep_time * MSEC_PER_SEC;
		}
	}
}

/*
 * Find URL component for the device cancel operation and action id
 */
static int hawkbit_find_cancelAction_base(struct hawkbit_ctl_res *res,
					  char *cancel_base)
{
	size_t len;
	const char *href;
	char *helper, *endptr;

	href = res->_links.cancelAction.href;
	if (!href) {
		*cancel_base = '\0';
		return 0;
	}

	helper = strstr(href, "cancelAction/");
	if (!helper) {
		/* A badly formatted cancel base is a server error */
		LOG_ERR("missing cancelBase/ in href %s", href);
		return -EINVAL;
	}

	len = strlen(helper);
	if (len > CANCEL_BASE_SIZE - 1) {
		/* Lack of memory is an application error */
		LOG_ERR("cancelBase %s is too big (len %zu, max %zu)", helper,
			len, CANCEL_BASE_SIZE - 1);
		return -ENOMEM;
	}

	strncpy(cancel_base, helper, CANCEL_BASE_SIZE);

	helper = strtok(helper, "/");
	if (helper == 0) {
		return -EINVAL;
	}

	helper = strtok(NULL, "/");
	if (helper == 0) {
		return -EINVAL;
	}

	hb_context.action_id = strtol(helper, &endptr, 10);
	if (hb_context.action_id <= 0) {
		LOG_ERR("Invalid action ID: %d", hb_context.action_id);
		return -EINVAL;
	}

	return 0;
}

/*
 * Find URL component for the device's deployment operations
 * resource
 */
static int hawkbit_find_deployment_base(struct hawkbit_ctl_res *res,
					char *deployment_base)
{
	const char *href;
	const char *helper;
	size_t len;

	href = res->_links.deploymentBase.href;
	if (!href) {
		*deployment_base = '\0';
		return 0;
	}

	helper = strstr(href, "deploymentBase/");
	if (!helper) {
		/* A badly formatted deployment base is a server error */
		LOG_ERR("missing deploymentBase/ in href %s", href);
		return -EINVAL;
	}

	len = strlen(helper);
	if (len > DEPLOYMENT_BASE_SIZE - 1) {
		/* Lack of memory is an application error */
		LOG_ERR("deploymentBase %s is too big (len %zu, max %zu)",
			helper, len, DEPLOYMENT_BASE_SIZE - 1);
		return -ENOMEM;
	}

	strncpy(deployment_base, helper, DEPLOYMENT_BASE_SIZE);
	return 0;
}

/*
 * Find URL component for this device's deployment operations
 * resource.
 */
static int hawkbit_parse_deployment(struct hawkbit_dep_res *res,
				    int32_t *json_action_id,
				    char *download_http,
				    int32_t *file_size)
{
	int32_t size;
	char *endptr;
	const char *href;
	const char *helper;
	struct hawkbit_dep_res_chunk *chunk;
	size_t len, num_chunks, num_artifacts;
	struct hawkbit_dep_res_arts *artifact;

	hb_context.action_id = strtol(res->id, &endptr, 10);
	if (hb_context.action_id < 0) {
		LOG_ERR("negative action ID: %d", hb_context.action_id);
		return -EINVAL;
	}

	*json_action_id = hb_context.action_id;

	num_chunks = res->deployment.num_chunks;
	if (num_chunks != 1) {
		LOG_ERR("expecting one chunk (got %d)", num_chunks);
		return -ENOSPC;
	}

	chunk = &res->deployment.chunks[0];
	if (strcmp("bApp", chunk->part)) {
		LOG_ERR("only part 'bApp' is supported; got %s", chunk->part);
		return -EINVAL;
	}

	num_artifacts = chunk->num_artifacts;
	if (num_artifacts != 1) {
		LOG_ERR("expecting one artifact (got %d)", num_artifacts);
		return -EINVAL;
	}

	artifact = &chunk->artifacts[0];
	size = artifact->size;

	if (size > SLOT1_SIZE) {
		LOG_ERR("artifact file size too big (got %d, max is %d)", size,
			SLOT1_SIZE);
		return -ENOSPC;
	}

	/*
	 * Find the download-http href. We only support the DEFAULT
	 * tenant on the same hawkbit server.
	 */
	href = artifact->_links.download_http.href;
	if (!href) {
		LOG_ERR("missing expected download-http href");
		return -EINVAL;
	}

	helper = strstr(href, "/DEFAULT/controller/v1");
	if (!helper) {
		LOG_ERR("unexpected download-http href format: %s", helper);
		return -EINVAL;
	}

	len = strlen(helper);
	if (len == 0) {
		LOG_ERR("empty download-http");
		return -EINVAL;
	} else if (len > DOWNLOAD_HTTP_SIZE - 1) {
		LOG_ERR("download-http %s is too big (len: %zu, max: %zu)",
			helper, len, DOWNLOAD_HTTP_SIZE - 1);
		return -ENOMEM;
	}

	/* Success. */
	strncpy(download_http, helper, DOWNLOAD_HTTP_SIZE);
	*file_size = size;
	return 0;
}

static void hawkbit_dump_base(struct hawkbit_ctl_res *r)
{
	LOG_DBG("config.polling.sleep=%s",
		log_strdup(r->config.polling.sleep));
	LOG_DBG("_links.deploymentBase.href=%s",
		log_strdup(r->_links.deploymentBase.href));
	LOG_DBG("_links.configData.href=%s",
		log_strdup(r->_links.configData.href));
	LOG_DBG("_links.cancelAction.href=%s",
		log_strdup(r->_links.cancelAction.href));
}

static void hawkbit_dump_deployment(struct hawkbit_dep_res *d)
{
	struct hawkbit_dep_res_chunk *c = &d->deployment.chunks[0];
	struct hawkbit_dep_res_arts *a = &c->artifacts[0];
	struct hawkbit_dep_res_links *l = &a->_links;

	LOG_DBG("id=%s", log_strdup(d->id));
	LOG_DBG("download=%s", log_strdup(d->deployment.download));
	LOG_DBG("update=%s", log_strdup(d->deployment.update));
	LOG_DBG("chunks[0].part=%s", log_strdup(c->part));
	LOG_DBG("chunks[0].name=%s", log_strdup(c->name));
	LOG_DBG("chunks[0].version=%s", log_strdup(c->version));
	LOG_DBG("chunks[0].artifacts[0].filename=%s",
		log_strdup(a->filename));
	LOG_DBG("chunks[0].artifacts[0].hashes.sha1=%s",
		log_strdup(a->hashes.sha1));
	LOG_DBG("chunks[0].artifacts[0].hashes.md5=%s",
		log_strdup(a->hashes.md5));
	LOG_DBG("chunks[0].artifacts[0].hashes.sha256=%s",
		log_strdup(a->hashes.sha256));
	LOG_DBG("chunks[0].size=%d", a->size);
	LOG_DBG("download-http=%s",
		log_strdup(l->download_http.href));
	LOG_DBG("md5sum =%s", log_strdup(l->md5sum_http.href));
}

int hawkbit_init(void)
{
	bool image_ok;
	int ret = 0, rc = 0;
	struct flash_pages_info info;
	int32_t action_id;

	fs.offset = FLASH_AREA_OFFSET(storage);
	rc = flash_get_page_info_by_offs(
		device_get_binding(DT_CHOSEN_ZEPHYR_FLASH_CONTROLLER_LABEL),
		fs.offset, &info);
	if (rc) {
		LOG_ERR("Unable to get storage page info");
		return -EIO;
	}

	fs.sector_size = info.size;
	fs.sector_count = 3U;

	rc = nvs_init(&fs, DT_CHOSEN_ZEPHYR_FLASH_CONTROLLER_LABEL);
	if (rc) {
		LOG_ERR("Storage flash Init failed");
		return -ENODEV;
	}

	rc = nvs_read(&fs, ADDRESS_ID, &action_id, sizeof(action_id));
	LOG_DBG("Action id: current %d", action_id);

	image_ok = boot_is_img_confirmed();
	LOG_INF("Image is %s confirmed OK", image_ok ? "" : " not");
	if (!image_ok) {
		ret = boot_write_img_confirmed();
		if (ret < 0) {
			LOG_ERR("Couldn't confirm this image: %d", ret);
			return ret;
		}

		LOG_DBG("Marked image as OK");
		ret = boot_erase_img_bank(FLASH_AREA_ID(image_1));
		if (ret) {
			LOG_ERR("Failed to erase second slot");
			return ret;
		}
	}

	return ret;
}

static int enum_for_http_req_string(char *userdata)
{
	int i = 0;
	char *name = http_request[i].http_req_str;

	while (name) {
		if (strcmp(name, userdata) == 0) {
			return http_request[i].n;
		}

		name = http_request[++i].http_req_str;
	}

	return 0;
}

static void response_cb(struct http_response *rsp,
			enum http_final_call final_data,
			void *userdata)
{
	static size_t body_len;
	int ret, type, downloaded;
	uint8_t *body_data = NULL, *rsp_tmp = NULL;
	static size_t response_buffer_size = RESPONSE_BUFFER_SIZE;

	type = enum_for_http_req_string(userdata);

	switch (type) {
	case HAWKBIT_PROBE:
		if (hb_context.dl.http_content_size == 0) {
			body_data = rsp->body_start;
			body_len = strlen(rsp->recv_buf);
			body_len -= (rsp->body_start - rsp->recv_buf);
			strncpy(hb_context.response_data, body_data, body_len);
			hb_context.dl.http_content_size = rsp->content_length;
		}

		if ((body_data == NULL) && (final_data == HTTP_DATA_MORE)) {
			body_data = rsp->recv_buf;
			body_len = body_len + (rsp->data_len);
			if (body_len > response_buffer_size) {
				response_buffer_size <<= 1;
				rsp_tmp = realloc(hb_context.response_data,
						  response_buffer_size);
				if (rsp_tmp == NULL) {
					LOG_ERR("Failed to realloc memory");
					hb_context.code_status =
						HAWKBIT_METADATA_ERROR;
					cleanup_connection();
					break;
				}

				hb_context.response_data = rsp_tmp;
			}

			strncat(hb_context.response_data, body_data,
					rsp->data_len);
		}

		if (final_data == HTTP_DATA_FINAL) {
			if (hb_context.dl.http_content_size != body_len) {
				LOG_ERR("HTTP response len mismatch");
				hb_context.code_status =
					HAWKBIT_METADATA_ERROR;
			}

			hb_context.response_data[body_len] = '\0';
			ret = json_obj_parse(hb_context.response_data,
					     body_len, json_ctl_res_descr,
					     ARRAY_SIZE(json_ctl_res_descr),
					     &hawkbit_results.base);
			if (ret < 0) {
				LOG_ERR("JSON parse error");
				hb_context.code_status =
					HAWKBIT_METADATA_ERROR;
			}

			body_len = 0;
		}

		break;

	case HAWKBIT_CLOSE:
	case HAWKBIT_REPORT:
	case HAWKBIT_CONFIG_DEVICE:
		if (strcmp(rsp->http_status, "OK") < 0) {
			LOG_ERR("Failed to cancel the update");
		}

		break;

	case HAWKBIT_PROBE_DEPLOYMENT_BASE:
		if (hb_context.dl.http_content_size == 0) {
			body_data = rsp->body_start;
			body_len = strlen(rsp->recv_buf);
			body_len -= (rsp->body_start - rsp->recv_buf);
			strncpy(hb_context.response_data, body_data, body_len);
			hb_context.dl.http_content_size = rsp->content_length;
		}

		if ((body_data == NULL) && (final_data == HTTP_DATA_MORE)) {
			body_data = rsp->recv_buf;
			body_len = body_len + (rsp->data_len);
			if (body_len > response_buffer_size) {
				response_buffer_size <<= 1;
				rsp_tmp = realloc(hb_context.response_data,
						  response_buffer_size);
				if (rsp_tmp == NULL) {
					LOG_ERR("Failed to relloc memory");
					hb_context.code_status =
						HAWKBIT_METADATA_ERROR;
					cleanup_connection();
					break;
				}

				hb_context.response_data = rsp_tmp;
			}

			strncat(hb_context.response_data, body_data,
				rsp->data_len);
		}

		if (final_data == HTTP_DATA_FINAL) {
			if (hb_context.dl.http_content_size != body_len) {
				LOG_ERR("HTTP response len mismatch");
				hb_context.code_status =
					HAWKBIT_METADATA_ERROR;
			}

			hb_context.response_data[body_len] = '\0';
			ret = json_obj_parse(hb_context.response_data,
					     body_len, json_dep_res_descr,
					     ARRAY_SIZE(json_dep_res_descr),
					     &hawkbit_results.dep);
			if (ret < 0) {
				LOG_ERR("DeploymentBase JSON parse error");
				hb_context.code_status =
					HAWKBIT_METADATA_ERROR;
			}

			body_len = 0;
		}

		break;

	case HAWKBIT_DOWNLOAD:
		if (hb_context.dl.http_content_size == 0) {
			body_data = rsp->body_start;
			body_len = rsp->data_len;
			body_len -= (rsp->body_start - rsp->recv_buf);
			hb_context.dl.http_content_size = rsp->content_length;
		}

		if ((rsp->body_found == 1) && (body_data == NULL)) {
			body_data = rsp->recv_buf;
			body_len = rsp->data_len;
		}

		if (body_data != NULL) {
			ret = flash_img_buffered_write(&hb_context.flash_ctx,
				body_data, body_len,
				final_data == HTTP_DATA_FINAL);
			if (ret < 0) {
				LOG_ERR("flash write error");
				hb_context.code_status = HAWKBIT_DOWNLOAD_ERROR;
			}
		}

		hb_context.dl.downloaded_size =
			flash_img_bytes_written(&hb_context.flash_ctx);

		downloaded = hb_context.dl.downloaded_size * 100 /
			     hb_context.dl.http_content_size;

		if (downloaded > hb_context.dl.download_progress) {
			hb_context.dl.download_progress = downloaded;
			LOG_DBG("Download percentage: %d%% ",
				hb_context.dl.download_progress);
		}

		if (final_data == HTTP_DATA_FINAL) {
			k_sem_give(&hb_context.semaphore);
		}

		break;
	}
}

static bool send_request(enum http_method method,
			 enum hawkbit_http_request type,
			 enum hawkbit_status_fini finished,
			 enum hawkbit_status_exec execution)
{
	int ret = 0;

	struct hawkbit_cfg cfg;
	struct hawkbit_close close;
	struct hawkbit_dep_fbk feedback;
	char acid[11];
	const char *fini = hawkbit_status_finished(finished);
	const char *exec = hawkbit_status_execution(execution);
	char device_id[DEVICE_ID_HEX_MAX_SIZE] = { 0 };

	if (!hawkbit_get_device_identity(device_id, DEVICE_ID_HEX_MAX_SIZE)) {
		hb_context.code_status = HAWKBIT_METADATA_ERROR;
	}

	memset(&hb_context.http_req, 0, sizeof(hb_context.http_req));
	memset(&hb_context.recv_buf_tcp, 0, sizeof(hb_context.recv_buf_tcp));
	hb_context.http_req.url = hb_context.url_buffer;
	hb_context.http_req.method = method;
	hb_context.http_req.host = CONFIG_HAWKBIT_SERVER;
	hb_context.http_req.port = CONFIG_HAWKBIT_PORT;
	hb_context.http_req.protocol = "HTTP/1.1";
	hb_context.http_req.response = response_cb;
	hb_context.http_req.recv_buf = hb_context.recv_buf_tcp;
	hb_context.http_req.recv_buf_len = sizeof(hb_context.recv_buf_tcp);

	switch (type) {
	case HAWKBIT_PROBE:
		ret = http_client_req(hb_context.sock, &hb_context.http_req,
				      HAWKBIT_RECV_TIMEOUT, "HAWKBIT_PROBE");
		if (ret < 0) {
			LOG_ERR("Unable to send http request");
			return false;
		}

		break;

	case HAWKBIT_CONFIG_DEVICE:
		memset(&cfg, 0, sizeof(cfg));
		cfg.mode = "merge";
		cfg.data.VIN = device_id;
		cfg.data.hwRevision = "3";
		cfg.id = "";
		cfg.time = "";
		cfg.status.execution = exec;
		cfg.status.result.finished = fini;

		ret = json_obj_encode_buf(json_cfg_descr,
					  ARRAY_SIZE(json_cfg_descr), &cfg,
					  hb_context.status_buffer,
					  hb_context.status_buffer_size - 1);
		if (ret) {
			LOG_ERR("Can't encode the json script");
			return false;
		}

		hb_context.http_req.content_type_value =
			HTTP_HEADER_CONTENT_TYPE_JSON;
		hb_context.http_req.payload = hb_context.status_buffer;
		hb_context.http_req.payload_len =
			strlen(hb_context.status_buffer);

		ret = http_client_req(hb_context.sock, &hb_context.http_req,
				      HAWKBIT_RECV_TIMEOUT,
				      "HAWKBIT_CONFIG_DEVICE");
		if (ret < 0) {
			LOG_ERR("Unable to send http request");
			return false;
		}

		break;

	case HAWKBIT_CLOSE:
		memset(&close, 0, sizeof(close));
		memset(&hb_context.status_buffer, 0,
		       sizeof(hb_context.status_buffer));
		snprintk(acid, sizeof(acid), "%d", hb_context.action_id);
		close.id = acid;
		close.time = "";
		close.status.execution = exec;
		close.status.result.finished = fini;

		ret = json_obj_encode_buf(json_close_descr,
					  ARRAY_SIZE(json_close_descr), &close,
					  hb_context.status_buffer,
					  hb_context.status_buffer_size - 1);
		if (ret) {
			LOG_ERR("Can't encode the json script");
			return false;
		}

		hb_context.http_req.content_type_value =
			HTTP_HEADER_CONTENT_TYPE_JSON;
		hb_context.http_req.payload = hb_context.status_buffer;
		hb_context.http_req.payload_len =
			strlen(hb_context.status_buffer);

		ret = http_client_req(hb_context.sock, &hb_context.http_req,
				      HAWKBIT_RECV_TIMEOUT, "HAWKBIT_CLOSE");
		if (ret < 0) {
			LOG_ERR("Unable to send http request");
			return false;
		}

		break;

	case HAWKBIT_PROBE_DEPLOYMENT_BASE:
		hb_context.http_req.content_type_value = NULL;
		ret = http_client_req(hb_context.sock, &hb_context.http_req,
				      HAWKBIT_RECV_TIMEOUT,
				      "HAWKBIT_PROBE_DEPLOYMENT_BASE");
		if (ret < 0) {
			LOG_ERR("Unable to send http request");
			return false;
		}

		break;

	case HAWKBIT_REPORT:
		if (!fini || !exec) {
			return -EINVAL;
		}

		LOG_INF("Reporting deployment feedback %s (%s) for action %d",
			fini, exec, hb_context.json_action_id);
		/* Build JSON */
		memset(&feedback, 0, sizeof(feedback));
		snprintk(acid, sizeof(acid), "%d", hb_context.json_action_id);
		feedback.id = acid;
		feedback.status.result.finished = fini;
		feedback.status.execution = exec;

		ret = json_obj_encode_buf(json_dep_fbk_descr,
					  ARRAY_SIZE(json_dep_fbk_descr),
					  &feedback, hb_context.status_buffer,
					  hb_context.status_buffer_size - 1);
		if (ret) {
			LOG_ERR("Can't encode response: %d", ret);
			return ret;
		}

		hb_context.http_req.content_type_value =
			HTTP_HEADER_CONTENT_TYPE_JSON;
		hb_context.http_req.payload = hb_context.status_buffer;
		hb_context.http_req.payload_len =
			strlen(hb_context.status_buffer);

		ret = http_client_req(hb_context.sock, &hb_context.http_req,
				      HAWKBIT_RECV_TIMEOUT, "HAWKBIT_REPORT");
		if (ret < 0) {
			LOG_ERR("Unable to send http request");
			return false;
		}

		break;

	case HAWKBIT_DOWNLOAD:
		ret = http_client_req(hb_context.sock, &hb_context.http_req,
				      HAWKBIT_RECV_TIMEOUT, "HAWKBIT_DOWNLOAD");
		if (ret < 0) {
			LOG_ERR("Unable to send image download request");
			return false;
		}

		break;
	}

	return true;
}

enum hawkbit_response hawkbit_probe(void)
{
	int ret;
	int32_t action_id;
	int32_t file_size = 0;
	char device_id[DEVICE_ID_HEX_MAX_SIZE] = { 0 },
	     cancel_base[CANCEL_BASE_SIZE] = { 0 },
	     download_http[DOWNLOAD_HTTP_SIZE] = { 0 },
	     deployment_base[DEPLOYMENT_BASE_SIZE] = { 0 },
	     firmware_version[BOOT_IMG_VER_STRLEN_MAX] = { 0 };

	memset(&hb_context, 0, sizeof(hb_context));
	hb_context.response_data = malloc(RESPONSE_BUFFER_SIZE);
	k_sem_init(&hb_context.semaphore, 0, 1);

	if (!boot_is_img_confirmed()) {
		LOG_ERR("The current image is not confirmed");
		hb_context.code_status = HAWKBIT_UNCONFIRMED_IMAGE;
		goto error;
	}

	if (!hawkbit_get_firmware_version(firmware_version,
					  BOOT_IMG_VER_STRLEN_MAX)) {
		hb_context.code_status = HAWKBIT_METADATA_ERROR;
		goto error;
	}

	if (!hawkbit_get_device_identity(device_id, DEVICE_ID_HEX_MAX_SIZE)) {
		hb_context.code_status = HAWKBIT_METADATA_ERROR;
		goto error;
	}

	if (!start_http_client()) {
		hb_context.code_status = HAWKBIT_NETWORKING_ERROR;
		goto error;
	}

	/*
	 * Query the hawkbit base polling resource.
	 */
	LOG_INF("Polling target data from Hawkbit");

	memset(hb_context.url_buffer, 0, sizeof(hb_context.url_buffer));
	hb_context.dl.http_content_size = 0;
	hb_context.url_buffer_size = URL_BUFFER_SIZE;
	snprintk(hb_context.url_buffer, hb_context.url_buffer_size, "%s/%s-%s",
		 HAWKBIT_JSON_URL, CONFIG_BOARD, device_id);
	memset(&hawkbit_results.base, 0, sizeof(hawkbit_results.base));

	if (!send_request(HTTP_GET, HAWKBIT_PROBE, HAWKBIT_STATUS_FINISHED_NONE,
			  HAWKBIT_STATUS_EXEC_NONE)) {
		LOG_ERR("Send request failed");
		hb_context.code_status = HAWKBIT_NETWORKING_ERROR;
		goto cleanup;
	}

	if (hb_context.code_status == HAWKBIT_METADATA_ERROR) {
		goto error;
	}

	if (hawkbit_results.base.config.polling.sleep) {
		/* Update the sleep time. */
		hawkbit_update_sleep(&hawkbit_results.base);
	}

	hawkbit_dump_base(&hawkbit_results.base);

	if (hawkbit_results.base._links.cancelAction.href) {
		ret = hawkbit_find_cancelAction_base(&hawkbit_results.base,
						     cancel_base);
		memset(hb_context.url_buffer, 0, sizeof(hb_context.url_buffer));
		hb_context.dl.http_content_size = 0;
		hb_context.url_buffer_size = URL_BUFFER_SIZE;
		snprintk(hb_context.url_buffer, hb_context.url_buffer_size,
			 "%s/%s-%s/%s/feedback", HAWKBIT_JSON_URL, CONFIG_BOARD,
			 device_id, cancel_base);
		memset(&hawkbit_results.cancel, 0,
		       sizeof(hawkbit_results.cancel));

		if (!send_request(HTTP_POST, HAWKBIT_CLOSE,
				  HAWKBIT_STATUS_FINISHED_SUCCESS,
				  HAWKBIT_STATUS_EXEC_CLOSED)) {
			LOG_ERR("Send request failed");
			hb_context.code_status = HAWKBIT_NETWORKING_ERROR;
			goto cleanup;
		}

		hb_context.code_status = HAWKBIT_CANCEL_UPDATE;
		goto cleanup;
	}

	if (hawkbit_results.base._links.configData.href) {
		memset(hb_context.url_buffer, 0, sizeof(hb_context.url_buffer));
		hb_context.dl.http_content_size = 0;
		hb_context.url_buffer_size = URL_BUFFER_SIZE;
		snprintk(hb_context.url_buffer, hb_context.url_buffer_size,
			 "%s/%s-%s/configData", HAWKBIT_JSON_URL, CONFIG_BOARD,
			 device_id);

		if (!send_request(HTTP_PUT, HAWKBIT_CONFIG_DEVICE,
				  HAWKBIT_STATUS_FINISHED_SUCCESS,
				  HAWKBIT_STATUS_EXEC_CLOSED)) {
			LOG_ERR("Unable to send controller attributes");
			hb_context.code_status = HAWKBIT_NETWORKING_ERROR;
			goto cleanup;
		}
	}

	ret = hawkbit_find_deployment_base(&hawkbit_results.base,
					   deployment_base);
	if (ret < 0) {
		hb_context.code_status = HAWKBIT_METADATA_ERROR;
		goto cleanup;
	}

	if (strlen(deployment_base) == 0) {
		hb_context.code_status = HAWKBIT_NO_UPDATE;
		goto cleanup;
	}

	memset(hb_context.url_buffer, 0, sizeof(hb_context.url_buffer));
	hb_context.dl.http_content_size = 0;
	hb_context.url_buffer_size = URL_BUFFER_SIZE;
	snprintk(hb_context.url_buffer, hb_context.url_buffer_size,
		 "%s/%s-%s/%s", HAWKBIT_JSON_URL, CONFIG_BOARD, device_id,
		 deployment_base);
	memset(&hawkbit_results.dep, 0, sizeof(hawkbit_results.dep));
	memset(hb_context.response_data, 0, RESPONSE_BUFFER_SIZE);

	if (!send_request(HTTP_GET, HAWKBIT_PROBE_DEPLOYMENT_BASE,
			  HAWKBIT_STATUS_FINISHED_NONE,
			  HAWKBIT_STATUS_EXEC_NONE)) {
		LOG_ERR("Send request failed");
		hb_context.code_status = HAWKBIT_NETWORKING_ERROR;
		goto cleanup;
	}

	if (hb_context.code_status == HAWKBIT_METADATA_ERROR) {
		goto error;
	}

	hawkbit_dump_deployment(&hawkbit_results.dep);

	hb_context.dl.http_content_size = 0;
	ret = hawkbit_parse_deployment(&hawkbit_results.dep,
				       &hb_context.json_action_id,
				       download_http, &file_size);
	if (ret < 0) {
		LOG_ERR("Unable to parse deployment base");
		goto cleanup;
	}

	nvs_read(&fs, ADDRESS_ID, &action_id, sizeof(action_id));

	if (action_id == (int32_t)hb_context.json_action_id) {
		LOG_INF("Preventing repeated attempt to install %d",
			hb_context.json_action_id);
		hb_context.dl.http_content_size = 0;
		memset(hb_context.url_buffer, 0, sizeof(hb_context.url_buffer));
		hb_context.url_buffer_size = URL_BUFFER_SIZE;
		snprintk(hb_context.url_buffer, hb_context.url_buffer_size,
			 "%s/%s-%s/deploymentBase/%d/feedback",
			 HAWKBIT_JSON_URL, CONFIG_BOARD, device_id,
			 hb_context.json_action_id);

		if (!send_request(HTTP_POST, HAWKBIT_REPORT,
				  HAWKBIT_STATUS_FINISHED_SUCCESS,
				  HAWKBIT_STATUS_EXEC_CLOSED)) {
			LOG_ERR("Error when querying from hawkbit");
			hb_context.code_status = HAWKBIT_NETWORKING_ERROR;
			goto cleanup;
		}

		hb_context.code_status = HAWKBIT_OK;
		goto cleanup;
	}

	LOG_INF("Ready to install update");

	hb_context.dl.http_content_size = 0;
	memset(hb_context.url_buffer, 0, sizeof(hb_context.url_buffer));
	hb_context.url_buffer_size = URL_BUFFER_SIZE;

	snprintk(hb_context.url_buffer, hb_context.url_buffer_size, "%s",
		 download_http);

	flash_img_init(&hb_context.flash_ctx);

	if (!send_request(HTTP_GET, HAWKBIT_DOWNLOAD,
			  HAWKBIT_STATUS_FINISHED_NONE,
			  HAWKBIT_STATUS_EXEC_NONE)) {
		LOG_ERR("Send request failed");
		hb_context.code_status = HAWKBIT_NETWORKING_ERROR;
		goto cleanup;
	}

	if (hb_context.code_status == HAWKBIT_DOWNLOAD_ERROR) {
		goto cleanup;
	}

	if (boot_request_upgrade(BOOT_UPGRADE_TEST)) {
		LOG_ERR("Download failed");
		hb_context.code_status = HAWKBIT_DOWNLOAD_ERROR;
	} else {
		hb_context.code_status = HAWKBIT_UPDATE_INSTALLED;
		hawkbit_device_acid_update(hb_context.json_action_id);
	}

	hb_context.dl.http_content_size = 0;

cleanup:
	cleanup_connection();

error:
	free(hb_context.response_data);
	return hb_context.code_status;
}

static void autohandler(struct k_work *work)
{
	switch (hawkbit_probe()) {
	case HAWKBIT_UNCONFIRMED_IMAGE:
		LOG_ERR("Image is unconfirmed");
		LOG_ERR("Rebooting to previous confirmed image.");
		sys_reboot(SYS_REBOOT_WARM);
		break;

	case HAWKBIT_NO_UPDATE:
		LOG_INF("No update found");
		break;

	case HAWKBIT_CANCEL_UPDATE:
		LOG_INF("Hawkbit update Cancelled from server");
		break;

	case HAWKBIT_OK:
		LOG_INF("Image is already updated");
		break;

	case HAWKBIT_UPDATE_INSTALLED:
		LOG_INF("Update Installed. Please Reboot");
		break;

	case HAWKBIT_DOWNLOAD_ERROR:
		LOG_INF("Update Failed");
		break;

	case HAWKBIT_NETWORKING_ERROR:
		LOG_INF("Network Error");
		break;

	case HAWKBIT_METADATA_ERROR:
		LOG_INF("Metadata error");
		break;
	}

	k_work_reschedule(&hawkbit_work_handle, K_MSEC(poll_sleep));
}

void hawkbit_autohandler(void)
{
	k_work_init_delayable(&hawkbit_work_handle, autohandler);
	k_work_reschedule(&hawkbit_work_handle, K_NO_WAIT);
}
