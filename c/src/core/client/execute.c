/*******************************************************************************
 * This file is part of the Incubed project.
 * Sources: https://github.com/slockit/in3-c
 * 
 * Copyright (C) 2018-2020 slock.it GmbH, Blockchains LLC
 * 
 * 
 * COMMERCIAL LICENSE USAGE
 * 
 * Licensees holding a valid commercial license may use this file in accordance 
 * with the commercial license agreement provided with the Software or, alternatively, 
 * in accordance with the terms contained in a written agreement between you and 
 * slock.it GmbH/Blockchains LLC. For licensing terms and conditions or further 
 * information please contact slock.it at in3@slock.it.
 * 	
 * Alternatively, this file may be used under the AGPL license as follows:
 *    
 * AGPL LICENSE USAGE
 * 
 * This program is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free Software 
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *  
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY 
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A 
 * PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.
 * [Permissions of this strong copyleft license are conditioned on making available 
 * complete source code of licensed works and modifications, which include larger 
 * works using a licensed work, under the same license. Copyright and license notices 
 * must be preserved. Contributors provide an express grant of patent rights.]
 * You should have received a copy of the GNU Affero General Public License along 
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *******************************************************************************/

#include "../../third-party/crypto/ecdsa.h"
#include "../../third-party/crypto/secp256k1.h"
#include "../../third-party/crypto/sha3.h"
#include "../util/data.h"
#include "../util/log.h"
#include "../util/mem.h"
#include "../util/stringbuilder.h"
#include "../util/utils.h"
#include "cache.h"
#include "client.h"
#include "context_internal.h"
#include "keys.h"
#include "nodelist.h"
#include "verifier.h"
#include <stdint.h>
#include <string.h>
#include <time.h>

#define WAIT_TIME_CAP 3600
#define BLACKLISTTIME 24 * 3600

NONULL static void response_free(in3_ctx_t* ctx) {
  int nodes_count = 1;
  if (ctx->nodes) {
    nodes_count = ctx_nodes_len(ctx->nodes);
    in3_ctx_free_nodes(ctx->nodes);
  }
  if (ctx->raw_response) {
    for (int i = 0; i < nodes_count; i++) {
      if (ctx->raw_response[i].data.data) _free(ctx->raw_response[i].data.data);
    }
    _free(ctx->raw_response);
  }

  if (ctx->responses) _free(ctx->responses);
  if (ctx->response_context) json_free(ctx->response_context);
  if (ctx->signers) _free(ctx->signers);
  ctx->response_context = NULL;
  ctx->responses        = NULL;
  ctx->raw_response     = NULL;
  ctx->nodes            = NULL;
  ctx->signers          = NULL;
}

NONULL static void free_ctx_intern(in3_ctx_t* ctx, bool is_sub) {
  // only for intern requests, we actually free the original request-string
  if (is_sub)
    _free(ctx->request_context->c);
  else
    ctx->client->pending--;
  if (ctx->error) _free(ctx->error);
  response_free(ctx);
  if (ctx->request_context)
    json_free(ctx->request_context);

  if (ctx->requests) _free(ctx->requests);
  if (ctx->times)
    _free(ctx->times);
  if (ctx->cache) in3_cache_free(ctx->cache);
  if (ctx->required) free_ctx_intern(ctx->required, true);

  _free(ctx);
}

NONULL static bool auto_ask_sig(const in3_ctx_t* ctx) {
  return (ctx_is_method(ctx, "in3_nodeList") && !(ctx->client->flags & FLAGS_NODE_LIST_NO_SIG) && ctx->client->chain_id != ETH_CHAIN_ID_BTC);
}

NONULL static in3_ret_t pick_signers(in3_ctx_t* ctx, d_token_t* request) {

  const in3_t* c = ctx->client;

  if (in3_ctx_get_proof(ctx) == PROOF_NONE && !auto_ask_sig(ctx))
    return IN3_OK;

  // For nodeList request, we always ask for proof & atleast one signature
  uint8_t total_sig_cnt = c->signature_count ? c->signature_count : auto_ask_sig(ctx) ? 1 : 0;

  if (total_sig_cnt) {
    node_match_t*     signer_nodes = NULL;
    in3_node_filter_t filter       = NODE_FILTER_INIT;
    filter.nodes                   = d_get(d_get(request, K_IN3), K_SIGNER_NODES);
    filter.props                   = c->node_props | NODE_PROP_SIGNER;
    const in3_ret_t res            = in3_node_list_pick_nodes(ctx, &signer_nodes, total_sig_cnt, filter);
    if (res < 0)
      return ctx_set_error(ctx, "Could not find any nodes for requesting signatures", res);
    if (ctx->signers) _free(ctx->signers);
    const int node_count  = ctx_nodes_len(signer_nodes);
    ctx->signers_length   = node_count;
    ctx->signers          = _malloc(sizeof(bytes_t) * node_count);
    const node_match_t* w = signer_nodes;
    for (int i = 0; i < node_count; i++) {
      ctx->signers[i].len  = w->node->address->len;
      ctx->signers[i].data = w->node->address->data;
      w                    = w->next;
    }
    if (signer_nodes) in3_ctx_free_nodes(signer_nodes);
  }

  return IN3_OK;
}

static void free_urls(char** urls, int len, bool free_items) {
  if (!urls) return;
  if (free_items) {
    for (int i = 0; i < len; i++) _free(urls[i]);
  }
  _free(urls);
}

static int add_bytes_to_hash(struct SHA3_CTX* msg_hash, void* data, int len) {
  if (msg_hash) sha3_Update(msg_hash, data, len);
  return len;
}

NONULL static void add_token_to_hash(struct SHA3_CTX* msg_hash, d_token_t* t) {
  switch (d_type(t)) {
    case T_ARRAY:
    case T_OBJECT:
      for (d_iterator_t iter = d_iter(t); iter.left; d_iter_next(&iter))
        add_token_to_hash(msg_hash, iter.token);
      return;
    case T_NULL:
      return;

    default: {
      bytes_t b = d_to_bytes(t);
      sha3_Update(msg_hash, b.data, b.len);
    }
  }
}

NONULL static in3_ret_t ctx_create_payload(in3_ctx_t* c, sb_t* sb, bool multichain) {
  static unsigned long rpc_id_counter = 1;
  char                 temp[100];
  in3_t*               rc       = c->client;
  struct SHA3_CTX*     msg_hash = rc->key ? alloca(sizeof(struct SHA3_CTX)) : NULL;
  in3_proof_t          proof    = in3_ctx_get_proof(c);

  sb_add_char(sb, '[');

  for (uint_fast16_t i = 0; i < c->len; i++) {
    d_token_t *request_token = c->requests[i], *t;
    if (msg_hash) sha3_256_Init(msg_hash);

    if (i > 0) sb_add_char(sb, ',');
    sb_add_char(sb, '{');
    if ((t = d_get(request_token, K_ID)) == NULL)
      sb_add_key_value(sb, "id", temp, add_bytes_to_hash(msg_hash, temp, sprintf(temp, "%lu", rpc_id_counter++)), false);
    else if (d_type(t) == T_INTEGER)
      sb_add_key_value(sb, "id", temp, add_bytes_to_hash(msg_hash, temp, sprintf(temp, "%i", d_int(t))), false);
    else
      sb_add_key_value(sb, "id", d_string(t), add_bytes_to_hash(msg_hash, d_string(t), d_len(t)), true);
    sb_add_char(sb, ',');
    sb_add_key_value(sb, "jsonrpc", "2.0", 3, true);
    sb_add_char(sb, ',');
    if ((t = d_get(request_token, K_METHOD)) == NULL)
      return ctx_set_error(c, "missing method-property in request", IN3_EINVAL);
    else
      sb_add_key_value(sb, "method", d_string(t), add_bytes_to_hash(msg_hash, d_string(t), d_len(t)), true);
    sb_add_char(sb, ',');
    if ((t = d_get(request_token, K_PARAMS)) == NULL)
      sb_add_key_value(sb, "params", "[]", 2, false);
    else {
      if (d_is_binary_ctx(c->request_context)) return ctx_set_error(c, "only text json input is allowed", IN3_EINVAL);
      const str_range_t ps = d_to_json(t);
      if (msg_hash) add_token_to_hash(msg_hash, t);
      sb_add_key_value(sb, "params", ps.data, ps.len, false);
    }

    if (proof || msg_hash) {
      // add in3
      sb_add_range(sb, temp, 0, sprintf(temp, ",\"in3\":{\"verification\":\"%s\",\"version\": \"%s\"", proof == PROOF_NONE ? "never" : "proof", IN3_PROTO_VER));
      if (multichain)
        sb_add_range(sb, temp, 0, sprintf(temp, ",\"chainId\":\"0x%x\"", (unsigned int) rc->chain_id));
      const in3_chain_t* chain = in3_find_chain(rc, c->client->chain_id);
      if (chain->whitelist) {
        const bytes_t adr = bytes(chain->whitelist->contract, 20);
        sb_add_bytes(sb, ",\"whiteListContract\":", &adr, 1, false);
      }
      if (msg_hash) {
        uint8_t sig[65], hash[32];
        bytes_t sig_bytes = bytes(sig, 65);
        keccak_Final(msg_hash, hash);
        if (ecdsa_sign_digest(&secp256k1, c->client->key, hash, sig, sig + 64, NULL) < 0)
          return ctx_set_error(c, "could not sign the request", IN3_EINVAL);
        sb_add_bytes(sb, ",\"sig\":", &sig_bytes, 1, false);
      }
      if (rc->finality)
        sb_add_range(sb, temp, 0, sprintf(temp, ",\"finality\":%i", rc->finality));
      if (rc->replace_latest_block)
        sb_add_range(sb, temp, 0, sprintf(temp, ",\"latestBlock\":%i", rc->replace_latest_block));
      if (c->signers_length)
        sb_add_bytes(sb, ",\"signers\":", c->signers, c->signers_length, true);
      if ((rc->flags & FLAGS_INCLUDE_CODE) && strcmp(d_get_stringk(request_token, K_METHOD), "eth_call") == 0)
        sb_add_chars(sb, ",\"includeCode\":true");
      if (proof == PROOF_FULL)
        sb_add_chars(sb, ",\"useFullProof\":true");
      if ((rc->flags & FLAGS_STATS) == 0)
        sb_add_chars(sb, ",\"noStats\":true");
      if ((rc->flags & FLAGS_BINARY))
        sb_add_chars(sb, ",\"useBinary\":true");

      // do we have verified hashes?
      if (chain->verified_hashes) {
        uint_fast16_t l = rc->max_verified_hashes;
        for (uint_fast16_t i = 0; i < l; i++) {
          if (!chain->verified_hashes[i].block_number) {
            l = i;
            break;
          }
        }
        if (l) {
          bytes_t* hashes = alloca(sizeof(bytes_t) * l);
          for (uint_fast16_t i = 0; i < l; i++) hashes[i] = bytes(chain->verified_hashes[i].hash, 32);
          sb_add_bytes(sb, ",\"verifiedHashes\":", hashes, l, true);
        }
      }

#ifdef PAY
      if (c->client->pay && c->client->pay->handle_request) {
        in3_ret_t ret = c->client->pay->handle_request(c, sb, rc, c->client->pay->cptr);
        if (ret != IN3_OK) return ret;
      }
#endif
      sb_add_range(sb, "}}", 0, 2);
    } else
      sb_add_char(sb, '}');
  }
  sb_add_char(sb, ']');
  return IN3_OK;
}
NONULL static void update_nodelist_cache(in3_ctx_t* ctx) {
  // we don't update weights for local chains.
  if (!ctx->client->cache || ctx->client->chain_id == ETH_CHAIN_ID_LOCAL) return;
  chain_id_t chain_id = ctx->client->chain_id;
  in3_cache_store_nodelist(ctx->client, in3_find_chain(ctx->client, chain_id));
}

NONULL static in3_ret_t ctx_parse_response(in3_ctx_t* ctx, char* response_data, int len) {

  d_track_keynames(1);
  ctx->response_context = (response_data[0] == '{' || response_data[0] == '[') ? parse_json(response_data) : parse_binary_str(response_data, len);
  d_track_keynames(0);
  if (!ctx->response_context)
    return ctx_set_error(ctx, "Error in JSON-response : ", ctx_set_error(ctx, str_remove_html(response_data), IN3_EINVALDT));

  if (d_type(ctx->response_context->result) == T_OBJECT) {
    // it is a single result
    ctx->responses    = _malloc(sizeof(d_token_t*));
    ctx->responses[0] = ctx->response_context->result;
    if (ctx->len != 1) return ctx_set_error(ctx, "The response must be a single object!", IN3_EINVALDT);
  } else if (d_type(ctx->response_context->result) == T_ARRAY) {
    int        i;
    d_token_t* t = NULL;
    if (d_len(ctx->response_context->result) != (int) ctx->len)
      return ctx_set_error(ctx, "The responses must be a array with the same number as the requests!", IN3_EINVALDT);
    ctx->responses = _malloc(sizeof(d_token_t*) * ctx->len);
    for (i = 0, t = ctx->response_context->result + 1; i < (int) ctx->len; i++, t = d_next(t))
      ctx->responses[i] = t;
  } else
    return ctx_set_error(ctx, "The response must be a Object or Array", IN3_EINVALDT);

  return IN3_OK;
}

NONULL static void blacklist_node(node_match_t* node_weight) {
  if (node_weight && node_weight->weight) {
    // blacklist the node
    node_weight->weight->blacklisted_until = in3_time(NULL) + BLACKLISTTIME;
    node_weight->weight                    = NULL; // setting the weight to NULL means we reject the response.
    in3_log_debug("Blacklisting node for empty response: %s\n", node_weight->node->url);
  }
}

static uint16_t update_waittime(uint64_t nodelist_block, uint64_t current_blk, uint8_t repl_latest, uint16_t avg_blktime) {
  if (nodelist_block > current_blk)
    // misbehaving node, so allow to update right away and it'll get blacklisted due to the exp_last_block mechanism
    return 0;

  uint64_t diff = current_blk - nodelist_block;
  if (diff >= repl_latest)
    return 0;
  // we need to cap wait time as we might end up waiting for too long for chains with higher block time
  return min((repl_latest - diff) * avg_blktime, WAIT_TIME_CAP);
}

static void check_autoupdate(const in3_ctx_t* ctx, in3_chain_t* chain, d_token_t* response_in3, node_match_t* node) {
  if ((ctx->client->flags & FLAGS_AUTO_UPDATE_LIST) == 0) return;

  if (d_get_longk(response_in3, K_LAST_NODE_LIST) > d_get_longk(response_in3, K_CURRENT_BLOCK)) {
    // this shouldn't be possible, so we ignore this lastNodeList and do NOT try to update the nodeList
    return;
  }

  if (d_get_longk(response_in3, K_LAST_NODE_LIST) > chain->last_block) {
    if (chain->nodelist_upd8_params == NULL)
      chain->nodelist_upd8_params = _malloc(sizeof(*(chain->nodelist_upd8_params)));

    // overwrite old params since we have a newer nodelist update now
    memcpy(chain->nodelist_upd8_params->node, node->node->address->data, node->node->address->len);
    chain->nodelist_upd8_params->exp_last_block = d_get_longk(response_in3, K_LAST_NODE_LIST);
    chain->nodelist_upd8_params->timestamp      = in3_time(NULL) + update_waittime(d_get_longk(response_in3, K_LAST_NODE_LIST),
                                                                              d_get_longk(response_in3, K_CURRENT_BLOCK),
                                                                              ctx->client->replace_latest_block,
                                                                              chain->avg_block_time);
  }

  if (chain->whitelist && d_get_longk(response_in3, K_LAST_WHITE_LIST) > chain->whitelist->last_block)
    chain->whitelist->needs_update = true;
}

static inline bool is_blacklisted(const node_match_t* node_weight) { return node_weight && node_weight->weight == NULL; }

static bool is_user_error(d_token_t* error) {
  char* err_msg = d_type(error) == T_STRING ? d_string(error) : d_get_stringk(error, K_MESSAGE);
  // here we need to find a better way to detect user errors
  // currently we assume a error-message starting with 'Error:' is a server error and not a user error.
  return err_msg && strncmp(err_msg, "Error:", 6) != 0;
}

static in3_ret_t find_valid_result(in3_ctx_t* ctx, int nodes_count, in3_response_t* response, in3_chain_t* chain, in3_verifier_t* verifier) {
  node_match_t* node = ctx->nodes;

  // find the verifier
  in3_vctx_t vc;
  vc.ctx             = ctx;
  vc.chain           = chain;
  bool still_pending = false;

  // blacklist nodes for missing response
  for (int n = 0; n < nodes_count; n++, node = node ? node->next : NULL) {

    // if the response is still pending, we skip...
    if (response[n].state == IN3_WAITING) {
      still_pending = true;
      continue;
    }

    // handle times
    if (ctx->times && node && node->weight && ctx->times[n]) {
      node->weight->response_count++;
      node->weight->total_response_time += ctx->times[n];
      ctx->times[n] = 0; // make sure we count the time only once
    }

    // since nodes_count was detected before, this should not happen!
    if (response[n].state) {
      if (is_blacklisted(node))
        continue;
      else if (node)
        blacklist_node(node);
      ctx_set_error(ctx, response[n].data.len ? response[n].data.data : "no response from node", IN3_ERPC);
      if (response[n].data.data) {
        // clean up invalid data
        _free(response[n].data.data);
        response[n].data.data     = NULL;
        response[n].data.allocted = 0;
        response[n].data.len      = 0;
      }
    } else {
      // we need to clean up the previos responses if set
      if (ctx->error) _free(ctx->error);
      if (ctx->responses) _free(ctx->responses);
      if (ctx->response_context) json_free(ctx->response_context);
      ctx->error = NULL;

      if (node && node->weight) node->weight->blacklisted_until = 0;                        // we reset the blacklisted, because if the response was correct, no need to blacklist, otherwise we will set the blacklisted_until anyway
      in3_ret_t res = ctx_parse_response(ctx, response[n].data.data, response[n].data.len); // parse the result
      if (res < 0)
        blacklist_node(node);
      else {
        // check each request
        for (uint_fast16_t i = 0; i < ctx->len; i++) {
          vc.request = ctx->requests[i];
          vc.result  = d_get(ctx->responses[i], K_RESULT);
          vc.client  = ctx->client;

          if ((vc.proof = d_get(ctx->responses[i], K_IN3))) {
            // vc.proof is temporary set to the in3-section. It will be updated to real proof in the next lines.
#ifdef PAY
            // we update the payment info from the in3-section
            if (ctx->client->pay && ctx->client->pay->follow_up) {
              res = ctx->client->pay->follow_up(ctx, node, vc.proof, d_get(ctx->responses[i], K_ERROR), ctx->client->pay->cptr);
              if (res == IN3_WAITING && ctx->attempt < ctx->client->max_attempts - 1) {
                // this means we need to retry with the same node
                ctx->attempt++;
                for (int i = 0; i < nodes_count; i++) {
                  _free(ctx->raw_response[i].error.data);
                  _free(ctx->raw_response[i].result.data);
                }
                _free(ctx->raw_response);
                _free(ctx->responses);
                json_free(ctx->response_context);

                ctx->raw_response     = NULL;
                ctx->response_context = NULL;
                ctx->responses        = NULL;
                return res;

              } else if (res)
                return ctx_set_error(ctx, "Error following up the payment data", (ctx->verification_state = res));
            }
#endif
            vc.last_validator_change = d_get_longk(vc.proof, K_LAST_VALIDATOR_CHANGE);
            vc.currentBlock          = d_get_longk(vc.proof, K_CURRENT_BLOCK);
            vc.proof                 = d_get(vc.proof, K_PROOF);
          }

          if (!vc.result && ctx->attempt < ctx->client->max_attempts - 1) {
            // if we don't have a result, the node reported an error
            // since we don't know if this error is our fault or the server fault,we don't blacklist the node, but retry
            ctx->verification_state = IN3_ERPC;
            if (is_user_error(d_get(ctx->responses[i], K_ERROR)))
              node->weight = NULL; // we mark it as blacklisted, but not blacklist it in the nodelist, since it was not the nodes fault.
            else
              blacklist_node(node);
            break;
          } else if (verifier) {
            res = ctx->verification_state = verifier->verify(&vc);
            if (res == IN3_WAITING)
              return res;
            else if (res < 0) {
              blacklist_node(node);
              break;
            }
          } else
            // no verifier - nothing to verify
            ctx->verification_state = IN3_OK;
        }
      }
    }

    // check auto update opts only if this node wasn't blacklisted (due to wrong result/proof)
    if (!is_blacklisted(node) && ctx->responses && d_get(ctx->responses[0], K_IN3) && !d_get(ctx->responses[0], K_ERROR))
      check_autoupdate(ctx, chain, d_get(ctx->responses[0], K_IN3), node);

    // !node_weight is valid, because it means this is a internaly handled response
    if (!node || !is_blacklisted(node))
      return IN3_OK; // this reponse was successfully verified, so let us keep it.
  }
  // no valid response found,
  // if pending, we remove the error and wait
  if (still_pending) {
    if (ctx->error) _free(ctx->error);
    ctx->error              = NULL;
    ctx->verification_state = IN3_WAITING;
    return IN3_WAITING;
  }

  return IN3_EINVAL;
}

NONULL static char* convert_to_http_url(char* src_url) {
  const int l = strlen(src_url);
  if (strncmp(src_url, "https://", 8) == 0) {
    char* url = _malloc(l);
    strcpy(url, src_url + 1);
    url[0] = 'h';
    url[2] = 't';
    url[3] = 'p';
    return url;
  } else
    return _strdupn(src_url, l);
}

NONULL in3_request_t* in3_create_request(in3_ctx_t* ctx) {

  int       nodes_count = ctx_nodes_len(ctx->nodes);
  in3_ret_t res;

  // create url-array
  char**        urls       = nodes_count ? _malloc(sizeof(char*) * nodes_count) : NULL;
  node_match_t* node       = ctx->nodes;
  bool          multichain = false;

  for (int n = 0; n < nodes_count; n++) {
    urls[n] = node->node->url;

    // if the multichain-prop is set we need to specify the chain_id in the request
    if (in3_node_props_get(node->node->props, NODE_PROP_MULTICHAIN)) multichain = true;

    // cif we use_http, we need to malloc a new string, so we also need to free it later!
    if (ctx->client->flags & FLAGS_HTTP) urls[n] = convert_to_http_url(urls[n]);

    node = node->next;
  }

  // prepare the payload
  sb_t* payload = sb_new(NULL);
  res           = ctx_create_payload(ctx, payload, multichain);
  if (res < 0) {
    // we clean up
    sb_free(payload);
    free_urls(urls, nodes_count, ctx->client->flags & FLAGS_HTTP);
    // since we cannot return an error, we set the error in the context and return NULL, indicating the error.
    ctx_set_error(ctx, "could not generate the payload", res);
    return NULL;
  }

  // prepare response-object
  if (ctx->times) _free(ctx->times);
  in3_request_t* request = _calloc(sizeof(in3_request_t), 1);
  request->in3           = ctx->client;
  request->payload       = payload->data;
  request->urls_len      = nodes_count;
  request->urls          = urls;
  request->times         = nodes_count ? _calloc(nodes_count, sizeof(uint32_t)) : NULL;
  request->timeout       = ctx->client->timeout;
  ctx->times             = request->times;

  if (!nodes_count) nodes_count = 1; // at least one result, because for internal response we don't need nodes, but a result big enough.
  request->results = _calloc(sizeof(in3_response_t), nodes_count);
  for (int n = 0; n < nodes_count; n++) request->results[n].state = IN3_WAITING;

  // we set the raw_response
  ctx->raw_response = request->results;

  // we only clean up the the stringbuffer, but keep the content (payload->data)
  _free(payload);
  return request;
}

NONULL void request_free(in3_request_t* req, const in3_t* c, bool free_response) {
  // free resources
  free_urls(req->urls, req->urls_len, c->flags & FLAGS_HTTP);

  if (free_response) {
    for (int n = 0; n < req->urls_len; n++) {
      if (req->results[n].data.data)
        _free(req->results[n].data.data);
    }
    _free(req->results);
  }

  _free(req->payload);
  _free(req);
}

NONULL static bool ctx_is_allowed_to_fail(in3_ctx_t* ctx) {
  return ctx_is_method(ctx, "in3_nodeList");
}

NONULL in3_ret_t ctx_handle_failable(in3_ctx_t* ctx) {
  in3_ret_t res = IN3_OK;

  // blacklist node that gave us an error response for nodelist (if not first update)
  // and clear nodelist params
  in3_chain_t* chain = in3_find_chain(ctx->client, ctx->client->chain_id);

  if (nodelist_not_first_upd8(chain))
    blacklist_node_addr(chain, chain->nodelist_upd8_params->node, BLACKLISTTIME);
  _free(chain->nodelist_upd8_params);
  chain->nodelist_upd8_params = NULL;

  if (ctx->required) {
    // if first update return error otherwise return IN3_OK, this is because first update is
    // always from a boot node which is presumed to be trusted
    if (nodelist_first_upd8(chain))
      res = ctx_set_error(ctx, ctx->required->error ? ctx->required->error : "error handling subrequest", IN3_ERPC);

    if (res == IN3_OK) res = ctx_remove_required(ctx, ctx->required);
  }

  return res;
}

in3_ret_t in3_send_ctx(in3_ctx_t* ctx) {
  int       retry_count = 0;
  in3_ret_t res;

  while ((res = in3_ctx_execute(ctx)) != IN3_OK) {

    // error we stop here
    if (res != IN3_WAITING) return res;

    // we are waiting for an response.
    retry_count++;
    if (retry_count > 10) return ctx_set_error(ctx, "Looks like the response is not valid or not set, since we are calling the execute over and over", IN3_ERPC);

    // handle subcontexts first
    while (ctx->required && in3_ctx_state(ctx->required) != CTX_SUCCESS) {
      res = in3_send_ctx(ctx->required);
      if (res == IN3_EIGNORE)
        ctx_handle_failable(ctx);
      else if (res != IN3_OK)
        return ctx_set_error(ctx, ctx->required->error ? ctx->required->error : "error handling subrequest", res);

      // recheck in order to prepare the request.
      if ((res = in3_ctx_execute(ctx)) != IN3_WAITING) return res;
    }

    if (!ctx->raw_response) {
      switch (ctx->type) {
        case CT_RPC: {
          if (ctx->client->transport) {
            // handle transports
            in3_request_t* request = in3_create_request(ctx);
            if (request == NULL)
              return IN3_ENOMEM;
            in3_log_trace("... request to " COLOR_YELLOW_STR "\n... " COLOR_MAGENTA_STR "\n", request->urls[0], request->payload);
            ctx->client->transport(request);
            in3_log_trace(request->results->state
                              ? "... response: \n... " COLOR_RED_STR "\n"
                              : "... response: \n... " COLOR_GREEN_STR "\n",
                          request->results->data.data);
            request_free(request, ctx->client, false);
            break;
          } else
            return ctx_set_error(ctx, "no transport set", IN3_ECONFIG);
        }
        case CT_SIGN: {
          if (ctx->client->signer) {
            d_token_t*     params = d_get(ctx->requests[0], K_PARAMS);
            in3_sign_ctx_t sign_ctx;
            sign_ctx.message = d_to_bytes(d_get_at(params, 0));
            sign_ctx.account = d_to_bytes(d_get_at(params, 1));
            sign_ctx.type    = SIGN_EC_HASH;
            sign_ctx.ctx     = ctx;
            sign_ctx.wallet  = ctx->client->signer->wallet;
            if (!sign_ctx.message.data) return ctx_set_error(ctx, "missing data to sign", IN3_ECONFIG);
            if (!sign_ctx.account.data) return ctx_set_error(ctx, "missing account to sign", IN3_ECONFIG);

            ctx->raw_response = _calloc(sizeof(in3_response_t), 1);
            sb_init(&ctx->raw_response[0].data);
            in3_log_trace("... request to sign ");
            res = ctx->client->signer->sign(&sign_ctx);
            if (res < 0) return ctx_set_error(ctx, ctx->raw_response->data.data, res);
            sb_add_range(&ctx->raw_response->data, (char*) sign_ctx.signature, 0, 65);
            break;
          } else
            return ctx_set_error(ctx, "no signer set", IN3_ECONFIG);
        }
      }
    }
  }
  return res;
}

in3_ctx_t* ctx_find_required(const in3_ctx_t* parent, const char* search_method) {
  in3_ctx_t* sub_ctx = parent->required;
  while (sub_ctx) {
    if (!sub_ctx->requests) continue;
    if (ctx_is_method(sub_ctx, search_method)) return sub_ctx;
    sub_ctx = sub_ctx->required;
  }
  return NULL;
}

in3_ret_t ctx_add_required(in3_ctx_t* parent, in3_ctx_t* ctx) {
  //  printf(" ++ add required %s > %s\n", ctx_name(parent), ctx_name(ctx));
  ctx->required    = parent->required;
  parent->required = ctx;
  return in3_ctx_execute(ctx);
}

in3_ret_t ctx_remove_required(in3_ctx_t* parent, in3_ctx_t* ctx) {
  if (!ctx) return IN3_OK;
  in3_ctx_t* p = parent;
  while (p) {
    if (p->required == ctx) {
      //      printf(" -- remove required %s > %s\n", ctx_name(parent), ctx_name(ctx));
      p->required = NULL; //ctx->required;
      free_ctx_intern(ctx, true);
      return IN3_OK;
    }
    p = p->required;
  }
  return IN3_EFIND;
}

in3_ctx_state_t in3_ctx_state(in3_ctx_t* ctx) {
  if (ctx == NULL) return CTX_SUCCESS;
  in3_ctx_state_t required_state = in3_ctx_state(ctx->required);
  if (required_state == CTX_ERROR) return CTX_ERROR;
  if (ctx->error) return CTX_ERROR;
  if (ctx->required && required_state != CTX_SUCCESS) return CTX_WAITING_FOR_REQUIRED_CTX;
  if (!ctx->raw_response) return CTX_WAITING_FOR_RESPONSE;
  if (ctx->type == CT_RPC && !ctx->response_context) return CTX_WAITING_FOR_RESPONSE;
  return CTX_SUCCESS;
}

void ctx_free(in3_ctx_t* ctx) {
  if (ctx) free_ctx_intern(ctx, false);
}

static inline in3_ret_t pre_handle(in3_verifier_t* verifier, in3_ctx_t* ctx) {
  return verifier->pre_handle ? verifier->pre_handle(ctx, &ctx->raw_response) : IN3_OK;
}

in3_ret_t in3_ctx_execute(in3_ctx_t* ctx) {
  in3_ret_t ret;
  // if there is an error it does not make sense to execute.
  if (ctx->error) return (ctx->verification_state && ctx->verification_state != IN3_WAITING) ? ctx->verification_state : IN3_EUNKNOWN;

  // is it a valid request?
  if (!ctx->request_context || !d_get(ctx->requests[0], K_METHOD)) return ctx_set_error(ctx, "No Method defined", IN3_ECONFIG);

  // if there is response we are done.
  if (ctx->response_context && ctx->verification_state == IN3_OK) return IN3_OK;

  // if we have required-contextes, we need to check them first
  if (ctx->required && (ret = in3_ctx_execute(ctx->required)))
    return ret;

  switch (ctx->type) {
    case CT_RPC: {

      // check chain_id
      in3_chain_t* chain = in3_find_chain(ctx->client, ctx->client->chain_id);
      if (!chain) return ctx_set_error(ctx, "chain not found", IN3_EFIND);

      // find the verifier
      in3_verifier_t* verifier = in3_get_verifier(chain->type);
      if (verifier == NULL) return ctx_set_error(ctx, "No Verifier found", IN3_EFIND);

      // do we need to handle it internaly?
      if (!ctx->raw_response && !ctx->response_context && (ret = pre_handle(verifier, ctx)) < 0)
        return ctx_set_error(ctx, "The request could not be handled", ret);

      // if we don't have a nodelist, we try to get it.
      if (!ctx->raw_response && !ctx->nodes) {
        in3_node_filter_t filter = NODE_FILTER_INIT;
        filter.nodes             = d_get(d_get(ctx->requests[0], K_IN3), K_DATA_NODES);
        filter.props             = (ctx->client->node_props & 0xFFFFFFFF) | NODE_PROP_DATA | ((ctx->client->flags & FLAGS_HTTP) ? NODE_PROP_HTTP : 0) | (in3_ctx_get_proof(ctx) != PROOF_NONE ? NODE_PROP_PROOF : 0);
        if ((ret = in3_node_list_pick_nodes(ctx, &ctx->nodes, ctx->client->request_count, filter)) == IN3_OK) {
          if ((ret = pick_signers(ctx, ctx->requests[0])) < 0)
            return ctx_set_error(ctx, "error configuring the config for request", ret);

#ifdef PAY

          // now we have the nodes, we can prepare the payment
          if (ctx->client->pay && ctx->client->pay->prepare && (ret = ctx->client->pay->prepare(ctx, ctx->client->pay->cptr)) != IN3_OK) return ret;
#endif

        } else
          // since we could not get the nodes, we either report it as error or wait.
          return ctx_set_error(ctx, "could not find any node", ret);
      }

      // if we still don't have an response, we keep on waiting
      if (!ctx->raw_response) return IN3_WAITING;

      // ok, we have a response, then we try to evaluate the responses
      // verify responses and return the node with the correct result.
      ret = find_valid_result(ctx, ctx->nodes == NULL ? 1 : ctx_nodes_len(ctx->nodes), ctx->raw_response, chain, verifier);

      // update weights in the cache
      update_nodelist_cache(ctx);

      // we wait or are have successfully verified the response
      if (ret == IN3_WAITING || ret == IN3_OK) return ret;

      // if not, then we clean up
      response_free(ctx);

      // we count this is an attempt
      ctx->attempt++;

      // should we retry?
      if (ctx->attempt < ctx->client->max_attempts) {
        in3_log_debug("Retrying send request...\n");
        // reset the error and try again
        if (ctx->error) _free(ctx->error);
        ctx->error              = NULL;
        ctx->verification_state = IN3_WAITING;
        // now try again, which should end in waiting for the next request.
        return in3_ctx_execute(ctx);
      } else {
        if (ctx_is_allowed_to_fail(ctx)) {
          ret                     = IN3_EIGNORE;
          ctx->verification_state = IN3_EIGNORE;
        }
        // we give up
        return ctx->error ? (ret ? ret : IN3_ERPC) : ctx_set_error(ctx, "reaching max_attempts and giving up", IN3_ELIMIT);
      }
    }

    case CT_SIGN: {
      if (!ctx->raw_response || ctx->raw_response->state == IN3_WAITING)
        return IN3_WAITING;
      else if (ctx->raw_response->state)
        return IN3_ERPC;
      return IN3_OK;
    }
    default:
      return IN3_EINVAL;
  }
}

void in3_req_add_response(
    in3_request_t* req,      /**< [in] the request-pointer passed to the transport-function containing the payload and url */
    int            index,    /**< [in] the index of the url, since this request could go out to many urls */
    bool           is_error, /**< [in] if true this will be reported as error. the message should then be the error-message */
    const char*    data,     /**<  the data or the the string*/
    int            data_len  /**<  the length of the data or the the string (use -1 if data is a null terminated string)*/
) {
  if (req->results[index].state == IN3_OK && is_error) req->results[index].data.len = 0;
  req->results[index].state = is_error ? IN3_ERPC : IN3_OK;
  if (data_len == -1)
    sb_add_chars(&req->results[index].data, data);
  else
    sb_add_range(&req->results[index].data, data, 0, data_len);
}
