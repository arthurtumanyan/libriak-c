/* 
 File:   libriak-c.c
 Copyright (C) 2013  Arthur Tumanyan <arthurtumanyan@gmail.com; www.arthurtumanyan.tk>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "riak-c.h"

/*
 List keys without properties
 */
char ** RiakListKeys(RIAK_HNDL hnd, char *bucket, bool stream) {

    if (NULL == hnd) {
        return NULL;
    }

    if (NULL == bucket || (0 == strcmp(bucket, ""))) {
        return NULL;
    }
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (stream) {
        headers = curl_slist_append(headers, "Transfer-Encoding: chunked");
    }
    long http_code = 0;
    int sz = strlen(rserver_url) + strlen(bucket) + 38;
    char listkeys_url[sz];
    list_bucket_op_result.body = malloc(1);
    list_bucket_op_result.size = 0;

    snprintf(listkeys_url, sz, "%sbuckets/%s/keys?keys=%s&props=false", rserver_url, bucket, (stream) ? "stream" : "true");
    curl_easy_setopt(hnd, CURLOPT_URL, listkeys_url);
    curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, GetKListCallback);
    curl_easy_setopt(hnd, CURLOPT_WRITEDATA, (void *) &list_bucket_op_result);
    CURLcode ret = curl_easy_perform(hnd);

    curl_easy_getinfo(hnd, CURLINFO_RESPONSE_CODE, &http_code);
    if (ret == CURLE_OK && http_code == 200) {
        if (list_bucket_op_result.isOK) {
            return list_bucket_op_result.klist;
        }
    }
    if (headers) {
        curl_slist_free_all(headers);
        headers = NULL;
    }
    return NULL;
}

char ** RiakListBuckets(RIAK_HNDL hnd) {

    if (NULL == hnd) {
        return NULL;
    }

    long http_code = 0;
    int sz = strlen(rserver_url) + 21;
    char listbuckets_url[sz];
    list_bucket_op_result.body = malloc(1);
    list_bucket_op_result.size = 0;

    snprintf(listbuckets_url, sz, "%sbuckets?buckets=true", rserver_url);
    curl_easy_setopt(hnd, CURLOPT_URL, listbuckets_url);
    curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, GetBListCallback);
    curl_easy_setopt(hnd, CURLOPT_WRITEDATA, (void *) &list_bucket_op_result);
    CURLcode ret = curl_easy_perform(hnd);

    curl_easy_getinfo(hnd, CURLINFO_RESPONSE_CODE, &http_code);
    if (ret == CURLE_OK && http_code == 200) {
        if (list_bucket_op_result.isOK) {
            return list_bucket_op_result.blist;
        }
    }
    return NULL;
}

char * RiakGetKey(RIAK_HNDL hnd, char *bucket, char *key) {

    if (NULL == hnd) {
        return NULL;
    }
    if (NULL == bucket || (0 == strcmp(bucket, ""))) {
        return NULL;
    }
    if (NULL == key || (0 == strcmp(key, ""))) {
        return NULL;
    }

    long http_code = 0;
    get_key_op_result.body = malloc(1);
    get_key_op_result.size = 0;

    int sz = strlen(rserver_url) + strlen(bucket) + strlen(key) + 16;
    char getkey_url[sz];

    snprintf(getkey_url, sz, "%sbuckets/%s/keys/%s", rserver_url, bucket, key);
    curl_easy_setopt(hnd, CURLOPT_URL, getkey_url);
    curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, GetKeyCallback);
    curl_easy_setopt(hnd, CURLOPT_WRITEDATA, (void *) &get_key_op_result);
    CURLcode ret = curl_easy_perform(hnd);

    curl_easy_getinfo(hnd, CURLINFO_RESPONSE_CODE, &http_code);
    if (ret == CURLE_OK && http_code == 200) {
        if (get_key_op_result.isOK) {
            return get_key_op_result.body;
        }
    }
    return NULL;
}

int RiakSetKey(RIAK_HNDL hnd, char *bucket, char *key, char *value, _riak_optional_p *options) {

    if (NULL == hnd) {
        return false;
    }
    if (NULL == bucket || (0 == strcmp(bucket, ""))) {
        return false;
    }
    if (NULL == key) {
        return false;
    }
    if (!options->autokey && (0 == strcmp(key, ""))) {
        return false;
    }
    if (NULL == value || (0 == strcmp(value, ""))) {
        return false;
    }

    long http_code = 0;
    CURLcode ret;
    int sz;
    if (!options->autokey) {
        sz = strlen(rserver_url) + strlen(bucket) + strlen(key) + 32;
    } else {
        sz = strlen(rserver_url) + strlen(bucket) + 14;
    }
    char setkey_url[sz];

    if (!options->autokey) {
        snprintf(setkey_url, sz, "%sbuckets/%s/keys/%s%s%s", rserver_url, bucket, key, (!options->autokey) ? "?returnbody=" : "", (options->returnbody) ? "true" : "false");
    } else {
        snprintf(setkey_url, sz, "%sbuckets/%s/keys/", rserver_url, bucket);
    }

    struct curl_slist *headers = NULL;

    char * content_type = NULL;
    int content_t_size = strlen(options->content_type) + 15;
    if (options->content_type) {
        content_type = malloc(content_t_size);

        if (content_type) {
            strcpy(content_type, "Content-Type: ");
            strcat(content_type, options->content_type);
            content_type[content_t_size] = '\0';
            headers = curl_slist_append(headers, content_type);
        }
    }

    char * x_riak_clock = NULL;
    if (options->x_riak_vclock) {
        int x_clock_size = strlen(options->x_riak_vclock) + 16;
        x_riak_clock = malloc(x_clock_size);
        if (x_riak_clock) {
            strcpy(x_riak_clock, "X-Riak-Vclock: ");
            strcat(x_riak_clock, options->x_riak_vclock);
            x_riak_clock[x_clock_size] = '\0';
            headers = curl_slist_append(headers, x_riak_clock);
        }
    }

    char * x_riak_meta = NULL;
    if (options->x_riak_meta) {
        int x_meta_size = strlen(options->x_riak_meta) + strlen(options->metadata) + 15;
        x_riak_meta = malloc(x_meta_size);
        if (x_riak_meta) {
            strcpy(x_riak_meta, "X-Riak-Meta-");
            strcat(x_riak_meta, options->metadata);
            strcat(x_riak_meta, ": ");
            strcat(x_riak_meta, options->x_riak_meta);
            x_riak_meta[x_meta_size] = '\0';
            headers = curl_slist_append(headers, x_riak_meta);
        }
    }

    char * x_riak_index = NULL;
    if (options->x_riak_index) {
        int x_index_size = strlen(options->x_riak_index) + strlen(options->index_entry) + 16;
        x_riak_index = malloc(x_index_size);
        if (x_riak_index) {
            strcpy(x_riak_index, "X-Riak-Index-");
            strcat(x_riak_index, options->index_entry);
            strcat(x_riak_index, ": ");
            strcat(x_riak_index, options->x_riak_index);
            x_riak_index[x_index_size] = '\0';
            headers = curl_slist_append(headers, x_riak_index);
        }
    }

    char * x_riak_link = NULL;
    if (options->link) {
        int x_link_size = strlen(options->link) + 7;
        x_riak_link = malloc(x_link_size);
        if (x_riak_link) {
            strcpy(x_riak_link, "Link: ");
            strcat(x_riak_link, options->link);
            x_riak_link[x_link_size] = '\0';
            headers = curl_slist_append(headers, x_riak_link);
        } else {
            return false;
        }
    }

    curl_easy_setopt(hnd, CURLOPT_URL, setkey_url);
    curl_easy_setopt(hnd, CURLOPT_POSTFIELDS, value);
    curl_easy_setopt(hnd, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t) - 1);
    curl_easy_setopt(hnd, CURLOPT_HTTPHEADER, headers);
    if (!options->autokey) {
        curl_easy_setopt(hnd, CURLOPT_CUSTOMREQUEST, "PUT");
    }
    ret = curl_easy_perform(hnd);

    curl_easy_getinfo(hnd, CURLINFO_RESPONSE_CODE, &http_code);

    if (content_type) {
        free(content_type);
        content_type = NULL;
    }

    if (x_riak_clock) {
        free(x_riak_clock);
        x_riak_clock = NULL;
    }

    if (x_riak_meta) {
        free(x_riak_meta);
        x_riak_meta = NULL;
    }

    if (x_riak_index) {
        free(x_riak_index);
        x_riak_index = NULL;
    }

    if (x_riak_link) {
        free(x_riak_link);
        x_riak_link = NULL;
    }

    if (headers) {
        curl_slist_free_all(headers);
        headers = NULL;
    }

    if (ret == CURLE_OK) {
        if (options->autokey && http_code == 201) {
            return true;
        }
        if (!options->autokey && http_code == 200) {
            return true;
        }
    }
    return false;
}

int RiakDeleteKey(RIAK_HNDL hnd, char *bucket, char *key, _riak_optional_p *options) {

    if (NULL == hnd) {
        return false;
    }
    if (NULL == bucket || (0 == strcmp(bucket, ""))) {
        return false;
    }
    if (NULL == key || (0 == strcmp(key, ""))) {
        return false;
    }

    struct curl_slist *headers = NULL;
    long http_code = 0;
    CURLcode ret;

    int sz = strlen(rserver_url) + strlen(bucket) + strlen(key) + 16;
    char delkey_url[sz];

    char * x_riak_clid = NULL;
    if (options->x_riak_clid) {
        int x_clid_size = strlen(options->x_riak_clid) + 16;
        x_riak_clid = malloc(x_clid_size);
        if (x_riak_clid) {
            strcpy(x_riak_clid, "X-Riak-ClientId: ");
            strcat(x_riak_clid, options->x_riak_clid);
            x_riak_clid[x_clid_size] = '\0';
            headers = curl_slist_append(headers, x_riak_clid);
        }
    }

    snprintf(delkey_url, sz, "%sbuckets/%s/keys/%s", rserver_url, bucket, key);
    curl_easy_setopt(hnd, CURLOPT_URL, delkey_url);
    curl_easy_setopt(hnd, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(hnd, CURLOPT_HTTPHEADER, headers);
    ret = curl_easy_perform(hnd);
    curl_easy_getinfo(hnd, CURLINFO_RESPONSE_CODE, &http_code);

    if (x_riak_clid) {
        free(x_riak_clid);
        x_riak_clid = NULL;
    }

    if (headers) {
        curl_slist_free_all(headers);
        headers = NULL;
    }

    if (ret == CURLE_OK) {
        if (http_code == 204) {
            return true;
        }
    }
    return false;
}

RIAK_HNDL RiakInit(char *r_host, char *r_port, bool ssl) {

    CURL *hnd;
    char *protocol = NULL;
    char *host = NULL;
    char *port = NULL;

    if (NULL == r_host || (0 == strcasecmp(r_host, ""))) {
        host = RIAKHOST;
    } else {
        host = r_host;
    }
    if (NULL == r_port || (0 == strcasecmp(r_port, ""))) {
        port = "8098";
    } else {
        port = r_port;
    }

    if (ssl) {
        protocol = "https";
    } else {
        protocol = "http";
    }

    if (NULL != (hnd = curl_easy_init())) {
        curl_easy_setopt(hnd, CURLOPT_USERAGENT, "libriak/0.1");
        curl_easy_setopt(hnd, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(hnd, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(hnd, CURLOPT_FORBID_REUSE, 0L);
        curl_easy_setopt(hnd, CURLOPT_FRESH_CONNECT, 0L);
        curl_easy_setopt(hnd, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, NULL);
        curl_easy_setopt(hnd, CURLOPT_WRITEDATA, fopen("/dev/null", "w"));
        curl_easy_setopt(hnd, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(hnd, CURLOPT_VERBOSE, 0L);

        if (NULL == (rserver_url = malloc(30 * sizeof (char)))) {
            fprintf(stderr, "libriak: %s\n", strerror(errno));
            return NULL;
        }

        strcpy(rserver_url, protocol);
        strcat(rserver_url, "://");
        strcat(rserver_url, host);
        strcat(rserver_url, ":");
        strcat(rserver_url, port);
        strcat(rserver_url, "/");
    } else {
        return NULL;
    }
    return hnd;
}

bool RiakClose(RIAK_HNDL hnd) {
    if (NULL != hnd) {
        curl_easy_cleanup(hnd);
        hnd = NULL;
    } else {
        return false;
    }
    return true;
}

bool RiakPing(RIAK_HNDL hnd) {
    if (hnd != NULL) {
        int sz = strlen(rserver_url) + 6;
        char ping_url[sz];
        r_ping_res status;

        snprintf(ping_url, sz, "%sping", rserver_url);
        curl_easy_setopt(hnd, CURLOPT_URL, ping_url);
        curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, parse_ping_result);
        curl_easy_setopt(hnd, CURLOPT_WRITEDATA, &status);
        CURLcode ret = curl_easy_perform(hnd);
        if (ret == CURLE_OK && status.isOK) {
            return true;
        } else return false;

    } else return false;
}

char ** RiakGetBucketProps(char *bucket) {
    return NULL;
}

bool RiakResetBucketProps(RIAK_HNDL hnd, char *bucket) {

    if (NULL == hnd) {
        return false;
    }
    if (NULL == bucket || (0 == strcmp(bucket, ""))) {
        return false;
    }

    struct curl_slist *headers = NULL;
    long http_code = 0;
    CURLcode ret;

    int sz = strlen(rserver_url) + strlen(bucket) + 16;
    char resetprops_url[sz];

    snprintf(resetprops_url, sz, "%sbuckets/%s/props", rserver_url, bucket);
    curl_easy_setopt(hnd, CURLOPT_URL, resetprops_url);
    curl_easy_setopt(hnd, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(hnd, CURLOPT_HTTPHEADER, headers);
    ret = curl_easy_perform(hnd);
    curl_easy_getinfo(hnd, CURLINFO_RESPONSE_CODE, &http_code);

    if (headers) {
        curl_slist_free_all(headers);
        headers = NULL;
    }

    if (ret == CURLE_OK) {
        if (http_code == 204) {
            return true;
        }
    }
    return false;
}


