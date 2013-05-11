/* 
 File:   libriak-c.h
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

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <curl/curl.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <json/json.h>


#ifndef LIBRIAK_C_H
#define	LIBRIAK_C_H

#define RIAKHOST "127.0.0.1"

#ifdef	__cplusplus
extern "C" {
#endif

    int msgsz;
    char *msg;

    typedef struct {
        bool isOK;
    } r_ping_res;

    typedef struct {
        bool isOK;
        char *msg;
        char *body;
        size_t size;
    } get_op_result;

    typedef struct {
        bool isOK;
        char *msg;
        char *body;
        char **blist;
        char **klist;
        size_t size;
    } list_op_result;

    typedef char * URL;
    typedef CURL * RIAK_HNDL;
    // rw - quorum for both operations (get and put) involved in deleting an object (default is set at the bucket level)
    // r  - (read quorum) how many replicas need to agree when retrieving the object
    // pr - (primary read quorum) works like r but requires that the nodes read from are not fallback nodes
    // w  - (write quorum) how many replicas must confirm receiving writes before returning a successful response
    // dw - (durable write quorum) how many replicas to commit to durable storage before returning a successful response
    // pw - (primary write quorum) how many replicas to commit to primary nodes before returning a successful response

    // Content-Type  - the media type/format
    // X-Riak-Vclock - the opaque vector clock for the object
    // X-Riak-Meta-* - any user-defined metadata defined when storing the object
    // ETag          - the entity tag for the object, useful for conditional GET operations and validation-based caching
    // Last-Modified - a timestamp for when the object was last written, in HTTP datetime format
    // Link          - user- and system-defined links to other resources. Read more about Links.

    URL rserver_url;

    typedef struct {
        int rw;
        int r;
        int pr;
        int w;
        int dw;
        int pw;
        bool returnbody;
        bool autokey;
        char *content_type;
        char *x_riak_vclock;
        char *x_riak_meta;
        char *metadata;
        char *x_riak_index;
        char *index_entry;
        char *etag;
        char *last_modified;
        char *link;
        char *x_riak_clid;

    } _riak_optional_p;


    get_op_result get_key_op_result;
    list_op_result list_bucket_op_result;


    extern RIAK_HNDL RiakInit(char *r_host, char *r_port, bool ssl);
    extern bool RiakClose(RIAK_HNDL hnd);
    extern char * RiakGetKey(RIAK_HNDL hnd, char *bucket, char *key);
    extern int RiakSetKey(RIAK_HNDL hnd, char *bucket, char *key, char *value, _riak_optional_p *);
    extern int RiakDeleteKey(RIAK_HNDL hnd, char *bucket, char *key, _riak_optional_p *options);
    extern bool RiakPing(RIAK_HNDL hnd);
    extern char ** RiakListBuckets(RIAK_HNDL hnd);
    extern char ** RiakGetBucketProps(char *bucket); /* Not implemented yet */
    extern char ** RiakListKeys(RIAK_HNDL hnd, char *bucket, bool stream);
    extern bool RiakResetBucketProps(RIAK_HNDL hnd, char *bucket);
    extern size_t parse_ping_result(void *buffer, size_t size, size_t nmemb, r_ping_res *); //+
    //
    extern void RiakFreeMem(void *ptr); //+
    extern size_t GetKeyCallback(void *contents, size_t size, size_t nmemb, void *userp);
    extern size_t GetBListCallback(void *contents, size_t size, size_t nmemb, void *userp);
    extern size_t GetKListCallback(void *contents, size_t size, size_t nmemb, void *userp);
#ifdef	__cplusplus
}
#endif

#endif	/* LIBRIAK_C_H */

