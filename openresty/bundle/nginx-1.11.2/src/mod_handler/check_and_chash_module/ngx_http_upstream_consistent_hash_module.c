
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#if(NGX_HTTP_UPSTREAM_CHECK)
#include "ngx_http_upstream_check_module.h"
#endif

#define NR_BUCKETS          521
#define NR_VIRTUAL_NODES   (1024 * 16)
#define MAX_PEER_FAILED     3


typedef struct {
    struct sockaddr                *sockaddr;
    socklen_t                       socklen;
    ngx_str_t                       name;

    ngx_int_t                       weight;

    ngx_uint_t                      fails;
    time_t                          accessed;

    ngx_uint_t                      max_fails;
    time_t                          fail_timeout;

    ngx_uint_t                      down;          /* unsigned  down:1; */
    ngx_uint_t                      index;

	//add by zdw
	ngx_http_upstream_rr_peer_t		*rr_peer;
	//
} ngx_http_upstream_chash_real_node_t;


typedef struct ngx_http_upstream_chash_virtual_node_s  ngx_http_upstream_chash_virtual_node_t;

struct ngx_http_upstream_chash_virtual_node_s
{
    ngx_uint_t                               index;
    ngx_uint_t                               point;

    ngx_http_upstream_chash_real_node_t     *real;
    ngx_http_upstream_chash_virtual_node_t  *next;
};


typedef struct
{
    ngx_str_t                               *name;

    ngx_uint_t                               r_number;
    ngx_uint_t                               v_number;

    ngx_http_upstream_chash_real_node_t     *r_nodes;
    ngx_http_upstream_chash_virtual_node_t  *v_nodes;

    ngx_http_upstream_chash_virtual_node_t  *buckets[NR_BUCKETS];
} ngx_http_upstream_chash_ring_t;


typedef struct {
	//add by lhc
	ngx_flag_t								backupos_request;

    ngx_uint_t                               current;
    ngx_uint_t                               point;

    uintptr_t                               *tried;
    uintptr_t                                data;

    ngx_http_upstream_chash_ring_t          *ring;
} ngx_http_upstream_chash_peer_data_t;


static ngx_int_t ngx_http_upstream_init_consistent_hash_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_upstream_get_consistent_hash_peer(ngx_peer_connection_t *pc,
    void *data);
static char * ngx_http_upstream_consistent_hash(ngx_conf_t *cf, ngx_command_t *cmd,
    void *data);

static ngx_command_t  ngx_http_upstream_consistent_hash_commands[] = { 

    { ngx_string("consistent_hash"),
      NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
      ngx_http_upstream_consistent_hash,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_upstream_consistent_hash_module_ctx = { 
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_upstream_consistent_hash_module = {
    NGX_MODULE_V1,
    &ngx_http_upstream_consistent_hash_module_ctx, /* module context */
    ngx_http_upstream_consistent_hash_commands,    /* module directives */
    NGX_HTTP_MODULE,                               /* module type */
    NULL,                                          /* init master */
    NULL,                                          /* init module */
    NULL,                                          /* init process */
    NULL,                                          /* init thread */
    NULL,                                          /* exit thread */
    NULL,                                          /* exit process */
    NULL,                                          /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_upstream_get_consistent_hash_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_chash_peer_data_t *chp = data;
    ngx_http_upstream_chash_ring_t      *ring = chp->ring;

    time_t                               now;
    uintptr_t                            m;
    ngx_uint_t                           n;

    ngx_http_upstream_chash_real_node_t     *rnode;
    ngx_http_upstream_chash_virtual_node_t  *vnode;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "get consistent hash peer, v_number: %ui, r_number: %ui",
                   ring->v_number, ring->r_number);

	/*
    ngx_uint_t  i;

    for (i=0; i<ring->v_number; i++) {
        ngx_log_debug5(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                       "get consistent hash peer, %ui: vnode: %ui, point: %ui, rnode: %ui, name: %s",
                       i, ring->v_nodes[i].index, ring->v_nodes[i].point,
                       ring->v_nodes[i].real->index, ring->v_nodes[i].real->name.data);
    }
	*/


    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "get consistent hash peer, try: %ui", pc->tries);

    now = ngx_time();

    pc->cached = 0;
    pc->connection = NULL;

    if (ring->r_number == 1) {
        rnode = &ring->r_nodes[0];

		if (rnode->down
#if (NGX_HTTP_UPSTREAM_CHECK)
			|| ngx_http_upstream_check_peer_down(rnode->rr_peer->check_index)
#endif
		) {
			goto failed;
		}

    } else {
        vnode = ring->buckets[chp->point % NR_BUCKETS];
        rnode = vnode->real;

        for (;;) {
            n = rnode->index / (8 * sizeof(uintptr_t));
            m = (uintptr_t) 1 << rnode->index % (8 * sizeof(uintptr_t));

            if (!(chp->tried[n] & m)) {
                ngx_log_debug3(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                               "get consistent hash peer, rnode index: %ui, ip:%s port:%ui",
                               rnode->index,
                               inet_ntoa(((struct sockaddr_in*)rnode->sockaddr)->sin_addr),
                               ntohs(((struct sockaddr_in*)rnode->sockaddr)->sin_port));

                if (!rnode->down
#if (NGX_HTTP_UPSTREAM_CHECK)
						&& !ngx_http_upstream_check_peer_down(rnode->rr_peer->check_index)
#endif
						) {

                    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                                   "get consistent hash peer, rnode not down, fails: %ui, max_fails: %ui",
                                   rnode->fails, rnode->max_fails);

                    if (rnode->max_fails == 0 || rnode->fails
                            < rnode->max_fails) {
                        break;
                    }

                    if (now - rnode->accessed > rnode->fail_timeout) {
                        ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0, "get consistent hash peer, timeout");
                        rnode->fails = 0;
                        break;
                    }
                }

				if(chp->backupos_request){
					goto failed;
				}

                chp->tried[n] |= m;
                pc->tries--;
            }

            if (pc->tries == 0) {
                goto failed;
            }

            vnode = vnode->next;
            rnode = vnode->real;
        }

        chp->tried[n] |= m;

        chp->current = rnode->index;
    }

    pc->sockaddr = rnode->sockaddr;
    pc->socklen = rnode->socklen;
    pc->name = &rnode->name;


    return NGX_OK;

failed:

    pc->name = ring->name;

    return NGX_BUSY;
}


void
ngx_http_upstream_free_consistent_hash_peer(ngx_peer_connection_t *pc, void *data, ngx_uint_t state)
{
    ngx_http_upstream_chash_peer_data_t *chp = data;

    time_t                               now;
    ngx_http_upstream_chash_real_node_t *peer;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "free consistent hash peer, pc->tries: %ui", pc->tries);

    if (state == 0 && pc->tries == 0) {
        return;
    }

    if (chp->ring->r_number == 1) {
        pc->tries = 0;
        return;
    }

    if (state & NGX_PEER_FAILED) {
        now = ngx_time();

        peer = &chp->ring->r_nodes[chp->current];
        if (pc->tries == MAX_PEER_FAILED){
            peer->fails++;
            peer->accessed = now;
        }
    }
	
	pc->cached = 0;

    if (pc->tries) {
        pc->tries--;
    }
}


static ngx_int_t
ngx_http_upstream_init_consistent_hash_peer(ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *us)
{
    ngx_str_t  evaluated_key_to_hash;
    ngx_str_t  evaluated_key_slice_to_hash;
    ngx_uint_t                            n;
    ngx_http_upstream_chash_peer_data_t  *chp;
    ngx_http_variable_value_t *v;
	ngx_uint_t key;
	u_char					*b;

    chp = r->upstream->peer.data;

    if (chp == NULL) {
        chp = ngx_palloc(r->pool, sizeof(ngx_http_upstream_chash_peer_data_t));
        if (chp == NULL) {
            return NGX_ERROR;
        }
		chp->backupos_request = 0;

        r->upstream->peer.data = chp;
    }


    chp->ring = us->peer.data;

	if(r->backupos_request ){
		chp->backupos_request = r->backupos_request;
	}

    if (chp->ring->r_number <= 8 * sizeof(uintptr_t)) {
        chp->tried = &chp->data;
        chp->data = 0;

    } else {
        n = (chp->ring->r_number + (8 * sizeof(uintptr_t) - 1))
                / (8 * sizeof(uintptr_t));

        chp->tried = ngx_pcalloc(r->pool, n * sizeof(uintptr_t));
        if (chp->tried == NULL) {
            return NGX_ERROR;
        }
    }

    if (ngx_http_script_run(r, &evaluated_key_to_hash, us->hash_uri_lengths->elts, 0,
                                us->hash_uri_values->elts)  == NULL)
    {
        return NGX_ERROR;
    }

    chp->point = ngx_crc32_long(evaluated_key_to_hash.data, evaluated_key_to_hash.len);

	///add by wumm
    static ngx_str_t slicerange = ngx_string("slice_range");
	key = ngx_hash_key_lc(slicerange.data, slicerange.len);
	v = ngx_http_get_variable(r, &slicerange, key);
	if(r != r->main && v != NULL) {
		if(v->len > 0) {
		  evaluated_key_slice_to_hash.len = evaluated_key_to_hash.len + v->len;
		  evaluated_key_slice_to_hash.data = ngx_pnalloc(r->pool, evaluated_key_slice_to_hash.len);
		  b = evaluated_key_slice_to_hash.data;
		  b = ngx_copy(b, evaluated_key_to_hash.data,evaluated_key_to_hash.len);
          b = ngx_copy(b, v->data,v->len);
		  chp->point = ngx_crc32_long(evaluated_key_slice_to_hash.data, evaluated_key_slice_to_hash.len);
		}
    }


    r->upstream->peer.free = ngx_http_upstream_free_consistent_hash_peer;
    r->upstream->peer.get = ngx_http_upstream_get_consistent_hash_peer;

	/*
    if (chp->ring->r_number < MAX_PEER_FAILED)
        r->upstream->peer.tries = chp->ring->r_number;
    else
        r->upstream->peer.tries = MAX_PEER_FAILED;
	*/
	/// add by zdw
	r->upstream->peer.tries = chp->ring->r_number;
	///

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_consistent_hash_compare_virtual_nodes(const void *one, const void *two)
{
    ngx_http_upstream_chash_virtual_node_t  *first, *second;

    first = (ngx_http_upstream_chash_virtual_node_t *) one;
    second = (ngx_http_upstream_chash_virtual_node_t *) two;

    return (first->point > second->point);
}


static ngx_http_upstream_chash_virtual_node_t *
ngx_http_upstream_consistent_hash_find(ngx_http_upstream_chash_ring_t *ring, ngx_uint_t point)
{
    ngx_uint_t mid = 0, low = 0, high = ring->v_number - 1;

    while (1) {
        if (point <= ring->v_nodes[low].point || point > ring->v_nodes[high].point) {
            return &ring->v_nodes[low];
        }

        /* test middle point */
        mid = low + (high - low) / 2;

        /* perfect match */
        if (point <= ring->v_nodes[mid].point && point > (mid ? ring->v_nodes[mid-1].point : 0)) {
            return &ring->v_nodes[mid];
        }

        /* too low, go up */
        if (ring->v_nodes[mid].point < point) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
}


ngx_int_t
ngx_http_upstream_init_consistent_hash(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us)
{
    u_char                          *hash_data;

    ngx_uint_t                       i, j, k, r, v;
    ngx_uint_t                       r_number, v_number;
    ngx_uint_t                       scale, total_weights;

    ngx_uint_t                       step = 0xFFFFFFFF / NR_BUCKETS;

    ngx_http_upstream_server_t      *server;
    ngx_http_upstream_chash_ring_t  *ring;

	//add by zdw
	ngx_uint_t							x;
	ngx_http_upstream_rr_peer_t		*rr_peer;
	ngx_http_upstream_rr_peers_t	*rr_peers;

	if(ngx_http_upstream_init_round_robin(cf, us) == NGX_ERROR){
		return NGX_ERROR;
	}

	rr_peers = (ngx_http_upstream_rr_peers_t*)us->peer.data;
	if(rr_peers == NULL) {
		return NGX_ERROR;
	}
	//

    us->peer.init = ngx_http_upstream_init_consistent_hash_peer;

    if (!us->servers) {
        return NGX_ERROR;
    }

    server = us->servers->elts;

    r_number = 0;
    v_number = 0;

    total_weights = 0;

    for (i=0; i<us->servers->nelts; i++) {
        if (server[i].backup) {
            continue;
        }

        r_number += server[i].naddrs;
        total_weights += server[i].weight * server[i].naddrs;
    }

    scale    =  NR_VIRTUAL_NODES / total_weights;
    v_number =  scale * total_weights;

    ring = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_chash_ring_t));
    if (ring == NULL) {
        return NGX_ERROR;
    }

    ring->r_number = r_number;
    ring->v_number = v_number;
    ring->name = &us->host;

    ring->r_nodes = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_chash_real_node_t) * r_number);
    if (ring->r_nodes == NULL) {
        return NGX_ERROR;
    }

    ring->v_nodes = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_chash_virtual_node_t) * v_number);
    if (ring->v_nodes == NULL) {
        return NGX_ERROR;
    }

    hash_data = ngx_pcalloc(cf->pool, sizeof("255.255.255.255:65535-65535"));
    if (hash_data == NULL) {
        return NGX_ERROR;
    }

    r = 0;
    v = 0;

	//add by zdw
	srandom(ngx_pid);
	rr_peer = rr_peers->peer;
	//
	
    for (i=0; i<us->servers->nelts; i++) {
        for (j=0; j<server[i].naddrs; j++) {
            if (server[i].backup) {
                continue;
            }

			///add by zdw
			for(x = 0; x < rr_peers->number; x++)
			{
				if(server[i].addrs[j].sockaddr == rr_peer[x].sockaddr) {
					ring->r_nodes[r].rr_peer = &rr_peer[x];
					break;
				}
			}
			////

            ring->r_nodes[r].sockaddr = server[i].addrs[j].sockaddr;
            ring->r_nodes[r].socklen = server[i].addrs[j].socklen;
            ring->r_nodes[r].name = server[i].addrs[j].name;

            ring->r_nodes[r].max_fails = server[i].max_fails;
            ring->r_nodes[r].fail_timeout = server[i].fail_timeout;
            ring->r_nodes[r].down = server[i].down;

            ring->r_nodes[r].index = r;

            for (k = 0; k < server[i].weight*scale; k++) {
				ngx_memzero(hash_data, sizeof("255.255.255.255:65535-65535"));
                ngx_snprintf(hash_data, sizeof("255.255.255.255:65535-65535"), "%V-%ui", &server[i].addrs[j].name, k);

                ring->v_nodes[v].index = v;
                ring->v_nodes[v].real  = &ring->r_nodes[r];
                ring->v_nodes[v].point = ngx_crc32_long(hash_data, strlen((char *)hash_data));

                v++;
            }

            r++;
        }
    }

    ngx_sort(ring->v_nodes, ring->v_number, sizeof(ngx_http_upstream_chash_virtual_node_t),
                            (const void *) ngx_http_upstream_consistent_hash_compare_virtual_nodes);

    for (i=0; i<ring->v_number; i++) {
        ring->v_nodes[i].next = &ring->v_nodes[(i+1)%ring->v_number];
    }

    for (i=0; i<NR_BUCKETS; i++)
        ring->buckets[i] = ngx_http_upstream_consistent_hash_find(ring, i*step);

    us->peer.data = ring;

    return NGX_OK;
}


static char *
ngx_http_upstream_consistent_hash(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                       *value;
    ngx_http_script_compile_t        sc;
    ngx_http_upstream_srv_conf_t    *uscf;

    ngx_memzero(&sc, sizeof(ngx_http_script_compile_t));
    
    value = cf->args->elts;
   
    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    uscf->hash_uri_lengths = NULL;
    uscf->hash_uri_values  = NULL;   

    sc.cf = cf;
    sc.source = &value[1];
    sc.lengths = &uscf->hash_uri_lengths;
    sc.values = &uscf->hash_uri_values;
    sc.complete_lengths = 1;
    sc.complete_values = 1;

    if (ngx_http_script_compile(&sc) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    uscf->peer.init_upstream = ngx_http_upstream_init_consistent_hash;

    uscf->flags = NGX_HTTP_UPSTREAM_CREATE
                  |NGX_HTTP_UPSTREAM_WEIGHT
                  |NGX_HTTP_UPSTREAM_MAX_FAILS
                  |NGX_HTTP_UPSTREAM_FAIL_TIMEOUT
                  |NGX_HTTP_UPSTREAM_DOWN;

    return NGX_CONF_OK;
}
