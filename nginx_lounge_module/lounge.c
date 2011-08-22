/*
Copyright 2009 Meebo, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <json.h>

#include <time.h>
#include <math.h>

#define LOUNGE_PROXY_NODES_KEY "nodes"
#define LOUNGE_PROXY_SHARDMAP_KEY "shard_map"

#define FAILED_NODE_MAX_RETRY 60 * 10

#define SHARD_SIZE(n_shards) ((2LL << 31) / n_shards)

enum {
	LOUNGE_PROXY_HOST_INDEX = 0,
	LOUNGE_PROXY_PORT_INDEX
};

typedef struct {
	ngx_array_t   *codes;            /* uintptr_t */

	ngx_uint_t    captures;
	ngx_uint_t    stack_size;

	ngx_flag_t    log;
} lounge_loc_conf_t;

typedef struct {
	ngx_uint_t uri_sharded;
	int        shard_id;
} lounge_req_ctx_t;

typedef struct {
	ngx_peer_addr_t      peer;
	time_t               fail_retry_time;
	ngx_uint_t           fail_count;
} lounge_peer_t;

typedef struct {
	ngx_uint_t           shard_id;	
	ngx_uint_t           current_host_id;
	ngx_uint_t           try_i;
	lounge_peer_t        **addrs;
	ngx_uint_t           num_peers;
	ngx_uint_t           failed_peers;
	time_t               start_time;
} lounge_proxy_peer_data_t;

typedef struct {
	ngx_uint_t 						*num_peers;
	time_t  						*fail_times;
	ngx_uint_t 						*fail_counts;
	lounge_peer_t					***shard_id_peers;
} lounge_shard_lookup_table_t;

typedef struct {
	ngx_int_t                    num_shards;
	uint32_t                     *shard_range_table;
	ngx_str_t                    json_filename;
	lounge_peer_t                *couch_nodes;
	ngx_uint_t                   num_couch_nodes;
	lounge_shard_lookup_table_t  lookup_table;
} lounge_main_conf_t;

static void *lounge_create_loc_conf(ngx_conf_t *cf);
static char *lounge_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static char *lounge_shard_rewrite(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t lounge_init(ngx_conf_t *cf);
static char *lounge(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_upstream_init_couch_proxy_peer(ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_upstream_get_couch_proxy_peer(ngx_peer_connection_t *pc, void *data);
static void ngx_http_upstream_free_couch_proxy_peer(ngx_peer_connection_t *pc, void *data, ngx_uint_t state);
static char *lounge_proxy(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *lounge_proxy_again(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_upstream_init_hash(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us);
static ngx_uint_t lounge_proxy_crc32(u_char *keydata, size_t keylen);
static void * lounge_proxy_create_main(ngx_conf_t *cf);
static ngx_int_t lounge_proxy_init_peer(ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *us);
static ngx_int_t lounge_proxy_get_peer(ngx_peer_connection_t *pc, void *data);
static void lounge_proxy_free_peer(ngx_peer_connection_t *pc, void *data, ngx_uint_t state);

static ngx_command_t lounge_commands[] = {

	{   ngx_string("lounge-proxy"),
		NGX_HTTP_UPS_CONF | NGX_CONF_NOARGS,
		lounge_proxy,
		0,
		0,
		NULL },

	{   ngx_string("lounge-replication-map"),
		NGX_HTTP_MAIN_CONF | NGX_CONF_TAKE1,
		ngx_conf_set_str_slot,
		NGX_HTTP_MAIN_CONF_OFFSET,
		offsetof(lounge_main_conf_t, json_filename),
		NULL },

	{ 	ngx_string("lounge-shard-rewrite"),
		NGX_HTTP_LOC_CONF | NGX_HTTP_SRV_CONF | NGX_CONF_TAKE2,
		lounge_shard_rewrite,
		NGX_HTTP_LOC_CONF_OFFSET,
		0,
		NULL },

	{   ngx_string("rewrite_log"),
		 NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_SIF_CONF|NGX_HTTP_LOC_CONF
                        |NGX_HTTP_LIF_CONF|NGX_CONF_FLAG,
		ngx_conf_set_flag_slot,
		NGX_HTTP_LOC_CONF_OFFSET,
		offsetof(lounge_loc_conf_t, log),
		NULL },

	ngx_null_command
};


static ngx_http_module_t lounge_module_ctx = {
    NULL,                              		/* preconfiguration */
    lounge_init,                 			/* postconfiguration */
    lounge_proxy_create_main,  				/* create main configuration */
    NULL,                                  	/* init main configuration */
    NULL,                                	/* create server configuration */
    NULL,                                	/* merge server configuration */
    lounge_create_loc_conf,      			/* create location configration */
    lounge_merge_loc_conf        			/* merge location configration */
};


ngx_module_t lounge_module = {
    NGX_MODULE_V1,
    &lounge_module_ctx,          			/* module context */
    lounge_commands,             			/* module directives */
    NGX_HTTP_MODULE,                       	/* module type */
    NULL,                                  	/* init master */
    NULL,                                  	/* init module */
    NULL,                                  	/* init process */
    NULL,                                  	/* init thread */
    NULL,                                  	/* exit thread */
    NULL,                                  	/* exit process */
    NULL,                                  	/* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t
lounge_handler(ngx_http_request_t *r)
{
	const size_t        buffer_size = 1024;
	lounge_main_conf_t *lmcf;
	lounge_loc_conf_t  *rlcf;
	lounge_req_ctx_t   *ctx;
	int                 shard_id, n, i;
	char                db[buffer_size], 
	                    key[buffer_size], 
	                    extra[buffer_size];
	u_char             *uri,
                       *unescaped_key,
                       *unescaped_key_end;
	ngx_str_t           request_uri = ngx_string("request_uri");
	ngx_int_t           request_uri_key;
	ngx_str_t          *rewritten_uri;

	ngx_http_script_code_pt code;
	ngx_http_script_engine_t *e;

	ctx = ngx_http_get_module_ctx(r, lounge_module);
	if (!ctx) {
		ctx = ngx_pcalloc(r->pool, sizeof(lounge_req_ctx_t));
		if (!ctx) return NGX_ERROR;
		ngx_http_set_ctx(r, ctx, lounge_module);
	}

	/* we've already seen this request and rewritten the uri */
	if (ctx->uri_sharded) return NGX_DECLINED;


	/* execute the rewrite regex */
	rlcf = ngx_http_get_module_loc_conf(r, lounge_module);
	if (rlcf->codes == NULL) {
		return NGX_DECLINED;
	}

	e = ngx_pcalloc(r->pool, sizeof(ngx_http_script_engine_t));
	if (e == NULL) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	e->sp = ngx_pcalloc(r->pool,
	                    rlcf->stack_size * sizeof(ngx_http_variable_value_t));
	if (e->sp == NULL) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	request_uri_key = ngx_hash_key(request_uri.data,
	                               request_uri.len);

	*e->sp++ = *ngx_http_get_variable(r, &request_uri,
	                                 request_uri_key,
	                                 NGX_HTTP_VAR_INDEXED);

	if (rlcf->captures) {
		e->captures = ngx_pcalloc(r->pool, rlcf->captures * sizeof(int));
		if (e->captures == NULL) {
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
		}
	} else {
		e->captures = NULL;
	}

	e->ip = rlcf->codes->elts;
	e->request = r;
	e->quote = 1;
	e->log = rlcf->log;
	e->status = NGX_DECLINED;

	while (*(uintptr_t *) e->ip) {
		code = *(ngx_http_script_code_pt *) e->ip;
		code(e);
	}

	if (e->buf.len) {
		rewritten_uri = &e->buf;
	} else {
		rewritten_uri = &r->unparsed_uri;
	}

	/* allocate enough room for a null-termed uri */
	uri = ngx_palloc(r->pool, rewritten_uri->len + 1);
	
	/* null term so we can use sscanf */
	ngx_cpystrn(uri, rewritten_uri->data, rewritten_uri->len + 1);

	lmcf = ngx_http_get_module_main_conf(r, lounge_module);

	ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"r->unparsed_uri.data: %s, r->unparsed_uri.len: %d", 
			r->unparsed_uri.data, (int)r->unparsed_uri.len);
	
	/* We're expecting a URI that looks something like:
	 * /<DATABASE>/<KEY>[/<ATTACHMENT>][?<QUERYSTRING>]
	 * e.g. 	/targeting/some_key
	 *  		/targeting/some_key?vijay=hobbit
	 */
	*db = *key = *extra = '\0';
	n = sscanf((char*)uri, "/%1024[^/]/%1024[^?/]%1024[^?\n]", db, key, extra);

	ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"db: %s\nkey: %s\n", db, key);

	if (n < 2) {
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"sscanf only matched %d (should have been 2-3) -- declining to rewrite this url.", n);
		return NGX_DECLINED;
	}

	/* allocate some space for the new uri */
	size_t new_uri_len = r->unparsed_uri.len + 30;  /* this gives room for the db name prefix of "shards/XXXXXXXX-XXXXXXX/" with url-encoded slashes (%2F)  */
	r->uri.data = ngx_pcalloc(r->pool, new_uri_len);
	if (!r->uri.data) {
		return NGX_ERROR;
	}

	/* unescape the uri and hash it */
	u_char* unparsed_key;
	unescaped_key = ngx_pcalloc(r->pool, strlen(key) + 1);
	if (!unescaped_key) {
		return NGX_ERROR;
	}
	unescaped_key_end = unescaped_key;
	unparsed_key = (u_char *) key;
	ngx_unescape_uri(&unescaped_key_end, &unparsed_key,
	                 ngx_strlen(unparsed_key),
	                 NGX_UNESCAPE_URI);
	uint32_t crc32 = ngx_crc32_short(unescaped_key, 
	                                 unescaped_key_end - unescaped_key);

	for (shard_id = 0, i = 0; i < lmcf->num_shards; i++) {
		if(crc32 >= lmcf->shard_range_table[i]) {
			shard_id = i;
		}
	}
	ctx->shard_id = shard_id;

	uint32_t range_low = lmcf->shard_range_table[shard_id];
	uint32_t range_high = range_low + (SHARD_SIZE(lmcf->num_shards) - 1);
	if (shard_id == lmcf->num_shards - 1) {
		range_high = UINT32_MAX;
	}

	r->uri.len = snprintf((char*)r->uri.data, new_uri_len,
	                      "/shards%%2F%08x-%08x%%2F%s/%s%s",
	                      range_low, range_high, db, key, extra);

	if (r->uri.len >= new_uri_len) {
		return NGX_ERROR;
	}

	/* setting these flags to zero prevents nginx from re-escaping the uri
	 * (e.g. %2F ==> %252F)
	 */
	r->internal = 0;
	r->quoted_uri = 0;

	/* these flags indicate that we have a new uri and need to run it through
	 * the location phase chain again
	 */
	r->valid_unparsed_uri = 0;
	r->uri_changed = 1;

	/* this flag tells us we've already seen this request and rewritten the
	 * sharded uri
	 */
	ctx->uri_sharded = 1;

    return NGX_OK;
}


static void *
lounge_create_loc_conf(ngx_conf_t *cf)
{
    lounge_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(lounge_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    conf->stack_size = NGX_CONF_UNSET_UINT;
    conf->log = NGX_CONF_UNSET;

    return conf;
}


static char *
lounge_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    lounge_loc_conf_t *prev = parent;
    lounge_loc_conf_t *conf = child;

    uintptr_t *code;

    ngx_conf_merge_value(conf->log, prev->log, 0);
    ngx_conf_merge_uint_value(conf->stack_size, prev->stack_size, 10);

    if (conf->codes == NULL) {
	    return NGX_CONF_OK;
    }

    if (conf->codes == prev->codes) {
	    return NGX_CONF_OK;
    }

    code = ngx_array_push_n(conf->codes, sizeof(uintptr_t));
    if (code == NULL) {
	    return NGX_CONF_ERROR;
    }

    *code = (uintptr_t) NULL;

    return NGX_CONF_OK;
}

static char *
lounge_shard_rewrite(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    lounge_loc_conf_t  *lcf = conf;

    ngx_int_t                           n;
    ngx_str_t                           *value, err;
    ngx_http_script_code_pt             *code;
    ngx_http_script_compile_t           sc;
    ngx_http_script_regex_code_t        *regex;
    ngx_http_script_regex_end_code_t    *regex_end;
    u_char                              errstr[NGX_MAX_CONF_ERRSTR];

    regex = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                       sizeof(ngx_http_script_regex_code_t));
    if (regex == NULL) {
	    return NGX_CONF_ERROR;
    }

    ngx_memzero(regex, sizeof(ngx_http_script_regex_code_t));

    value = cf->args->elts;

    err.len = NGX_MAX_CONF_ERRSTR;
    err.data = errstr;

    regex->regex = ngx_regex_compile(&value[1], 0, cf->pool, &err);

    if (regex->regex == NULL) {
	    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "%s", err.data);
	    return NGX_CONF_ERROR;
    }

    regex->code = ngx_http_script_regex_start_code;
    /*regex->uri = 1; Don't parse the uri. We'll use a variable. */
    regex->name = value[1];

    if (value[2].data[value[2].len - 1] == '?') {

	    /* the last "?" drops the original arguments */
	    value[2].len--;

    } else {
	    regex->add_args = 1;
    }

    regex->status = NGX_OK;

    ngx_memzero(&sc, sizeof(ngx_http_script_compile_t));

    sc.cf = cf;
    sc.source = &value[2];
    sc.lengths = &regex->lengths;
    sc.values = &lcf->codes;
    sc.variables = ngx_http_script_variables_count(&value[2]);
    sc.main = regex;
    sc.complete_lengths = 1;
    sc.compile_args = !regex->redirect;

    if (ngx_http_script_compile(&sc) != NGX_OK) {
	    return NGX_CONF_ERROR;
    }

    regex = sc.main;

    regex->ncaptures = sc.ncaptures;
    regex->size = sc.size;
    regex->args = sc.args;

    if (sc.variables == 0 && !sc.dup_capture) {
	    regex->lengths = NULL;
    }

    n = ngx_regex_capture_count(regex->regex);

    if (n < 0) {
	    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
	                       ngx_regex_capture_count_n " failed for "
	                       "pattern \"%V\"", &value[1]);
	    return NGX_CONF_ERROR;
    }

    if (regex->ncaptures > (ngx_uint_t) n) {
	    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
	                       "pattern \"%V\" has less captures "
	                       "than referrenced in substitution \"%v\"",
	                       &value[1], &value[2]);
	    return NGX_CONF_ERROR;
    }

    if (regex->ncaptures < (ngx_uint_t) n) {
	    regex->ncaptures = (ngx_uint_t) n;
    }

    if (regex->ncaptures) {
	    regex->ncaptures = (regex->ncaptures + 1) * 3;

	    if (lcf->captures < regex->ncaptures) {
		    lcf->captures = regex->ncaptures;
	    }
    }

    regex_end = ngx_http_script_add_code(lcf->codes,
                                         sizeof(ngx_http_script_regex_end_code_t),
                                         &regex);

    if (regex_end == NULL) {
	    return NGX_CONF_ERROR;
    }

    regex_end->code = ngx_http_script_regex_end_code;
    regex_end->uri = regex->uri;
    regex_end->args = regex->args;
    regex_end->add_args = regex->add_args;
    regex_end->redirect = regex->redirect;

    code = ngx_http_script_add_code(lcf->codes, sizeof(uintptr_t), &regex);
    if (code == NULL) {
	    return NGX_CONF_ERROR;
    }

    *code = NULL;

    regex->next = (u_char *) lcf->codes->elts + lcf->codes->nelts
                                              - (u_char *) regex;

    return NGX_CONF_OK;
}

static ngx_int_t
lounge_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = lounge_handler;

    return NGX_OK;
}


/**
 * Upstream proxying related functions
 */

static struct json_object*
lounge_proxy_parse_config(const char *filename)
{
	struct json_tokener *tok;
	struct json_object *obj;
	FILE *json_file = fopen(filename, "r");
	if (!json_file) {
		fprintf(stderr, "Couldn't open %s: %s\n", filename, strerror(errno));
		return NULL;
	}
	const size_t buf_size = 1024;
	char buffer[buf_size];
	tok = json_tokener_new();
	size_t n_read;
	do {
		n_read = fread(buffer, sizeof(char), buf_size, json_file);
		obj = json_tokener_parse_ex(tok, buffer, n_read);
	} while (!obj && (n_read == buf_size));
	json_tokener_free(tok);
	fclose(json_file);
	return obj;
}

static ngx_int_t
lounge_proxy_build_lookup_table(ngx_conf_t *cf, lounge_main_conf_t *lmcf,
		struct json_object *lounge_proxy_conf)
{
	/* Example json file looks like this: 	
	 * {
	 * 	"shard_map": [[0,1], [1,0]],
	 * 	"nodes": [ ["localhost", 5984], ["localhost", 5984] ]
	 * }
	 */

	/* TODO:  Write the lookup table to have everything in a contiguous block of memory,
	 * avoiding pointer lookups and maximizing cache hits.
	 */
	struct json_object 					*shard_map, *shard_host_array, *host;
	ngx_uint_t 							num_shards;	
	int 								host_id;
	unsigned int						i,j;
	lounge_shard_lookup_table_t 		*lounge_lookup_table;

	shard_map = json_object_object_get(lounge_proxy_conf, "shard_map");
	shard_map = json_object_object_get(lounge_proxy_conf, LOUNGE_PROXY_SHARDMAP_KEY);
	num_shards = json_object_array_length(shard_map);
	lmcf->num_shards = num_shards;

	lounge_lookup_table = ngx_pcalloc(cf->pool, sizeof(lounge_shard_lookup_table_t));
	if (!lounge_lookup_table) {
		return NGX_ERROR;
	}
	lounge_lookup_table->num_peers = ngx_pcalloc(cf->pool, sizeof(ngx_uint_t) * num_shards);
	lounge_lookup_table->shard_id_peers = ngx_pcalloc(cf->pool, 
			sizeof(lounge_peer_t **) * num_shards);

	for (i = 0; i < num_shards; i++) {
		unsigned int num_hosts;
		shard_host_array = json_object_array_get_idx(shard_map, i);
		if (!shard_host_array) {
			return NGX_ERROR;
		}
		num_hosts = json_object_array_length(shard_host_array);
		lounge_lookup_table->num_peers[i] = num_hosts;

		lounge_lookup_table->shard_id_peers[i] = ngx_pcalloc(cf->pool, 
			sizeof(lounge_peer_t *) * num_hosts);

		for (j = 0; j < num_hosts; j++) {
			host = json_object_array_get_idx(shard_host_array, j);
			if (!host) {
				return NGX_ERROR;
			}
			host_id = json_object_get_int(host);
			lounge_lookup_table->shard_id_peers[i][j] = &lmcf->couch_nodes[host_id];
		}
	}
	lmcf->lookup_table = *lounge_lookup_table;
	return NGX_OK;
}

static ngx_int_t
lounge_proxy_build_peer_list(ngx_conf_t *cf, lounge_main_conf_t *lmcf, struct json_object *couch_proxy_conf)
{
	struct json_object *couch_proxies;
	int num_proxies, proxy_index;

	couch_proxies = json_object_object_get(couch_proxy_conf, LOUNGE_PROXY_NODES_KEY);
	num_proxies = json_object_array_length(couch_proxies);

    lmcf->couch_nodes = ngx_pcalloc(cf->pool, sizeof(lounge_peer_t) * num_proxies);
    if (lmcf->couch_nodes == NULL) {
        return NGX_ERROR;
    }
	lmcf->num_couch_nodes = num_proxies;

	/* iterate through the available hosts, adding them to the peers array */
	for (proxy_index = 0; proxy_index < num_proxies; proxy_index++) {
		struct json_object *node, *_port;
		const char *host;
		short port;
		ngx_url_t *u;

		u = ngx_pcalloc(cf->pool, sizeof(ngx_url_t));
		
		/* Extract the host and port from the json config */
		node = json_object_array_get_idx(couch_proxies, proxy_index);
		host = json_object_get_string(json_object_array_get_idx(node, LOUNGE_PROXY_HOST_INDEX));

		_port = json_object_array_get_idx(node, LOUNGE_PROXY_PORT_INDEX); 
		if (!_port) {
			return NGX_ERROR;
		}

		port = (short)json_object_get_int(_port);
		if (! (host && port) ) {
			return NGX_ERROR;
		}

		/* Use nginx's built in url parsing mechanisms */
		u->url.data = (u_char *)host;
		u->url.len = strlen(host);
		u->default_port = port;

		if (ngx_parse_url(cf->pool, u) != NGX_OK) {
			if (u->err) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
						"%s in upstream \"%V\"", u->err, &u->url);
			}
			return NGX_ERROR;
		}

		lmcf->couch_nodes[proxy_index].peer = *u->addrs;
	}
	return NGX_OK;
}

static ngx_int_t
lounge_proxy_init(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us)
{
	lounge_main_conf_t *conf;
	struct json_object *lounge_proxy_conf;
	u_char *json_filename;
	uint32_t *shard_range_table;
	int i;

	conf = ngx_http_conf_get_module_main_conf(cf, lounge_module);
	
	json_filename = ngx_pcalloc(cf->pool, conf->json_filename.len+1);
	ngx_memcpy(json_filename, conf->json_filename.data, conf->json_filename.len);

	lounge_proxy_conf = lounge_proxy_parse_config((char*)json_filename);
	if (!lounge_proxy_conf) {
		return NGX_ERROR;
	}

	if (lounge_proxy_build_peer_list(cf, conf, lounge_proxy_conf) != NGX_OK) {
		json_object_put(lounge_proxy_conf);
		return NGX_ERROR;
	}

	if (!lounge_proxy_build_lookup_table(cf, conf, lounge_proxy_conf) == NGX_OK) {
		json_object_put(lounge_proxy_conf);
		return NGX_ERROR;
	}

	json_object_put(lounge_proxy_conf);

	/* calculate the start of each hashed key range
	 *
	 * for now this is a function of the number of shards
	 * future configurations may let these key ranges be made explicit
	 * to allow for non-uniform splitting of the key space
	 */
	shard_range_table = ngx_pcalloc(cf->pool, sizeof(uint32_t) * conf->num_shards);
	for (i = 0; i < conf->num_shards; i++) {
		shard_range_table[i] = i * SHARD_SIZE(conf->num_shards);
	}
	conf->shard_range_table = shard_range_table;

	us->peer.data = conf->couch_nodes;

	/* Callback for request initialization */
    us->peer.init = lounge_proxy_init_peer;
	return NGX_OK;
}


static ngx_int_t
lounge_proxy_init_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us)
{
    lounge_proxy_peer_data_t     	*lpd;
	lounge_main_conf_t 				*lmcf;	
	lounge_req_ctx_t 				*ctx;
	int                             i;

	ctx = ngx_http_get_module_ctx(r, lounge_module);
	if (!ctx) return NGX_ERROR;

	lmcf = ngx_http_get_module_main_conf(r, lounge_module);
	if (!lmcf) {
		return NGX_ERROR;
	}
    
    lpd = ngx_pcalloc(r->pool, sizeof(lounge_proxy_peer_data_t));
    if (lpd == NULL) {
        return NGX_ERROR;
    }

	lpd->shard_id = ctx->shard_id;
	lpd->current_host_id = 0;

	lpd->addrs = lmcf->lookup_table.shard_id_peers[lpd->shard_id];
	lpd->num_peers = lmcf->lookup_table.num_peers[lpd->shard_id];

	lpd->failed_peers = 0;
	lpd->start_time = time(NULL);

	/* setup callbacks */
    r->upstream->peer.free = lounge_proxy_free_peer;
    r->upstream->peer.get = lounge_proxy_get_peer;

	/* set maximum number of retries to the number of peers */
    r->upstream->peer.tries = lpd->num_peers;

    r->upstream->peer.data = lpd;
    return NGX_OK;
}


static ngx_int_t
lounge_proxy_get_peer(ngx_peer_connection_t *pc, void *data)
{
    lounge_proxy_peer_data_t  *lpd = data;
	lounge_peer_t             *lp;
    ngx_peer_addr_t           *peer;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "upstream_couch_proxy: get upstream request "
				   "hash peer try %ui",
				   pc->tries);
    pc->cached = 0;
    pc->connection = NULL;

	/* If every peer is marked as failed we break this loop.
	 * This prevents us from blacklisting ourselves into a 503 situation.
	 * Instead, we just try the peers in order and hope one recovers.
	 */
	lp = lpd->addrs[lpd->current_host_id % lpd->num_peers];
	while (lpd->failed_peers < lpd->num_peers) {
		if (lpd->start_time >= lp->fail_retry_time) {
			/* this peer is good or the failure is old so try it */
			lp->fail_retry_time = 0;
			break;
		}
		lpd->failed_peers++;
		lpd->current_host_id++;
		lp = lpd->addrs[lpd->current_host_id % lpd->num_peers];
	}

	peer = &lp->peer;
	pc->sockaddr = peer->sockaddr;
	pc->socklen = peer->socklen;
	pc->name = &peer->name;

	return NGX_OK;
}

static void
lounge_proxy_free_peer(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state)
{
    lounge_proxy_peer_data_t  *lpd = data;
	lounge_peer_t             *lp;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0, 
            "upstream_couch_proxy: free upstream hash peer try %u", pc->tries);

	lp = lpd->addrs[lpd->current_host_id];
	if (state & (NGX_PEER_FAILED | NGX_PEER_NEXT)) {
		pc->tries--;
		lp->fail_count++;
		lpd->current_host_id++;
		lpd->failed_peers++;

		/* use a power function for a slower backoff than an exponential */
		int retry_timeout = pow(lp->fail_count, 1.5);
		/* and cap it*/
		retry_timeout = retry_timeout < FAILED_NODE_MAX_RETRY ?
			retry_timeout : FAILED_NODE_MAX_RETRY;
		lp->fail_retry_time = time(NULL) + retry_timeout;

		ngx_log_error(NGX_LOG_ALERT, pc->log, 0,
					  "[%s] host %V failed %d times, removed for %d seconds",
					  __FUNCTION__, &lp->peer.name, lp->fail_count,
					  retry_timeout);
    } else {
		/* proxy attempt was successful -- if there was a fail count, 
		 * decrement it and set it to non-failed
		 */
		if (lp->fail_count) {
			lp->fail_count--;
		}
		lp->fail_retry_time = 0; /* optimistic recovery */
	}
}

static char *
lounge_proxy(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstream_srv_conf_t *uscf;
    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
	uscf->peer.init_upstream = lounge_proxy_init;
    return NGX_CONF_OK;
}

static void *
lounge_proxy_create_main(ngx_conf_t *cf)
{
	lounge_main_conf_t *conf;

	conf = ngx_pcalloc(cf->pool, sizeof(lounge_main_conf_t));
	if (conf == NULL) {
		return NGX_CONF_ERROR;
	}

	conf->json_filename = (ngx_str_t)ngx_string(NULL);
	return conf;
}


