/**
 * @file operations.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Basic NETCONF operations implementation
 *
 * Copyright (c) 2016 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libyang/libyang.h>
#include <nc_server.h>
#include <sysrepo.h>

#include "config.h"

static char *
get_srval_name(sr_val_t *value, char **prefix)
{
    char *start, *stop, *aux;
    size_t len;

    if (!value || !value->xpath) {
        return NULL;
    }

    start = strrchr(value->xpath, '/');
    aux = strrchr(start, ':');
    if (aux) {
        *prefix = strndup(start + 1, aux - start - 1);
        start = aux;
    }
    start++;

    stop = strchr(start, '[');
    if (stop) {
        len = stop - start;
    } else {
        len = strlen(start);
    }

    aux = strndup(start, len);
    return aux;
}

static char *
get_srval_value(const struct lys_module *module, struct lyd_node *parent, const char *name, sr_val_t *value, char *buf)
{
    const struct lys_node *iter = NULL;

    if (!value) {
        return NULL;
    }

    switch (value->type) {
    case SR_STRING_T:
    case SR_BINARY_T:
    case SR_BITS_T:
    case SR_ENUM_T:
    case SR_IDENTITYREF_T:
    case SR_INSTANCEID_T:
    case SR_LEAFREF_T:
        return (value->data.string_val);
    case SR_LEAF_EMPTY_T:
        return NULL;
    case SR_BOOL_T:
        return value->data.bool_val ? "true" : "false";
    case SR_DECIMAL64_T:
        /* get fraction-digits */
        while ((iter = lys_getnext(iter, parent->schema, module, 0))) {
            if (!strcmp(iter->name, name)) {
                break;
            }
        }
        if (!iter) {
            return NULL;
        }
        sprintf(buf, "%.*f", ((struct lys_node_leaf *)iter)->type.info.dec64.dig, value->data.decimal64_val);
        return buf;
    case SR_UINT8_T:
        sprintf(buf, "%u", value->data.uint8_val);
        return buf;
    case SR_UINT16_T:
        sprintf(buf, "%u", value->data.uint16_val);
        return buf;
    case SR_UINT32_T:
        sprintf(buf, "%u", value->data.uint32_val);
        return buf;
    case SR_UINT64_T:
        sprintf(buf, "%lu", value->data.uint64_val);
        return buf;
    case SR_INT8_T:
        sprintf(buf, "%d", value->data.int8_val);
        return buf;
    case SR_INT16_T:
        sprintf(buf, "%d", value->data.int16_val);
        return buf;
    case SR_INT32_T:
        sprintf(buf, "%d", value->data.int32_val);
        return buf;
    case SR_INT64_T:
        sprintf(buf, "%ld", value->data.int64_val);
        return buf;
    default:
        return NULL;
    }

}

static struct lyd_node *
build_subtree(sr_session_ctx_t *ds, const struct lys_module *module, struct lyd_node *parent, sr_val_t *value)
{
    struct lyd_node *result;
    sr_val_t *child = NULL;
    sr_val_iter_t *iter = NULL;
    char *name, *strval = NULL;
    char buf[4096];
    int recursion, rc;

    switch (value->type) {
    case SR_CONTAINER_T:
    case SR_CONTAINER_PRESENCE_T:
    case SR_LIST_T:
        name = get_srval_name(value, &strval);
        if (strval) {
            module = ly_ctx_get_module(np2srv.ly_ctx, strval, NULL);
            free(strval);
        }
        result = lyd_new(parent, module, name);
        free(name);
        recursion = 1;
        break;
    default: /* all others (leafs and leaf-lists) */
        name = get_srval_name(value, &strval);
        if (strval) {
            module = ly_ctx_get_module(np2srv.ly_ctx, strval, NULL);
            free(strval);
        }
        strval = get_srval_value(module, parent, name, value, buf);
        result = lyd_new_leaf(parent, module, name, strval);
        free(name);
        recursion = 0;
        break;
    }

    if (!result) {
        ERR("Building data tree from sysrepo failed.");
        return NULL;
    }

    if (recursion) {
        rc = sr_get_items_iter(ds, value->xpath, false, &iter);
        if (rc != SR_ERR_OK) {
            ERR("Getting items (%s) from sysrepo failed (%s)", value->xpath, sr_strerror(rc));
            goto error;
        }

        while (sr_get_item_next(ds, iter, &child) == SR_ERR_OK){
            if (!build_subtree(ds, module, result, child)) {
                sr_free_val(child);
                sr_free_val_iter(iter);
                goto error;
            }
            sr_free_val(child);
        }
        sr_free_val_iter(iter);
    }

    return result;

error:
    lyd_free(result);

    return NULL;
}

struct nc_server_reply *
op_get(struct lyd_node *rpc, struct nc_session *ncs)
{
    sr_val_t *value = NULL;
    sr_val_iter_t *iter = NULL;
    struct lyd_node *root = NULL, *node;
    const char **list;
    char *xpath;
    int rc, i;
    struct np2sr_sessions *sessions;
    struct ly_set *nodeset;
    sr_session_ctx_t *ds;
    struct nc_server_error *e;

    /* TODO get and process filter, mandatory filter for get-config = only config true data */

    /* get sysrepo connections for this session */
    sessions = (struct np2sr_sessions *)nc_session_get_data(ncs);

    /* get know which datastore is being affected */
    if (!strcmp(rpc->schema->name, "get")) {
        ds = sessions->running;
    } else { /* get-config */
        nodeset = lyd_get_node(rpc, "/ietf-netconf:get-config/source/*");
        if (!strcmp(nodeset->dset[0]->schema->name, "running")) {
            ds = sessions->running;
        } else if (!strcmp(nodeset->dset[0]->schema->name, "startup")) {
            ds = sessions->startup;
        } else if (!strcmp(nodeset->dset[0]->schema->name, "candidate")) {
            ds = sessions->candidate;
        }
        /* TODO URL capability */
        ly_set_free(nodeset);
    }
    /* refresh sysrepo data */
    sr_session_refresh(ds);

    list = ly_ctx_get_module_names(np2srv.ly_ctx);
    if (!list) {
        return nc_server_reply_data(NULL, NC_PARAMTYPE_FREE);;
    }

    for (i = 0; list[i]; i++) {
        asprintf(&xpath, "/%s:", list[i]);

        /* get all list instances with their content (recursive) */
        rc = sr_get_items_iter(ds, xpath, false, &iter);
        free(xpath);
        if (rc == SR_ERR_UNKNOWN_MODEL) {
            /* skip internal modules not known to sysrepo */
            continue;
        } else if (rc != SR_ERR_OK) {
            ERR("Getting items (/%s) from sysrepo failed (%s)", list[i], sr_strerror(rc));
            e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
            nc_err_set_msg(e, "", "en"); /* TODO, get last error message */

            lyd_free_withsiblings(root);
            free(list);

            return nc_server_reply_err(e);
        }

        while (sr_get_item_next(ds, iter, &value) == SR_ERR_OK){
            node = build_subtree(ds, ly_ctx_get_module(np2srv.ly_ctx, list[i], NULL), NULL, value);
            sr_free_val(value);

            if (!root) {
                root = node;
            } else {
                lyd_insert_after(root->prev, node);
            }
        }
        sr_free_val_iter(iter);
    }
    free(list);

    /* debug
    lyd_print_file(stdout, root, LYD_XML_FORMAT, LYP_WITHSIBLINGS);
    debug */

    /* build RPC Reply */
    return nc_server_reply_data(root, NC_PARAMTYPE_FREE);
}

