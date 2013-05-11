/* 
 File:   callbacks.c
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

void RiakFreeMem(void *ptr) {
    if (ptr) {
        free(ptr);
    }
}

size_t GetBListCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    list_op_result *mem = (list_op_result *) userp;
    const char *bucket = NULL;

    mem->body = realloc(mem->body, mem->size + realsize + 1);
    if (mem->body == NULL) {
        /* out of memory! */
        mem->isOK = false;
        mem->msg = "Not enough memory";
        return 0;
    }

    memcpy(&(mem->body[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->body[mem->size] = 0;
    //
    json_object * jobj = json_tokener_parse(mem->body);
    enum json_type type;

    json_object_object_foreach(jobj, key, val) {
        type = json_object_get_type(val);

        if (type == json_type_array) {
            jobj = json_object_object_get(jobj, key);
            int arraylen = json_object_array_length(jobj);
            mem->blist = (char**) malloc(arraylen * sizeof (char*));
            int i;
            json_object * jvalue;
            for (i = 0; i < arraylen; i++) {
                jvalue = json_object_array_get_idx(jobj, i);
                bucket = json_object_get_string(jvalue);
                mem->blist[i] = (char*) malloc(strlen(bucket) * sizeof (char));
                strcpy(mem->blist[i], bucket);
            }
        } else {
            mem->isOK = false;
            mem->msg = "Not an array";
            return 0;
        }
    }
    mem->isOK = true;
    mem->msg = "Success";

    return realsize;
}

size_t GetKListCallback(void *contents, size_t size, size_t nmemb, void *userp) {
   
    size_t realsize = size * nmemb;
    list_op_result *mem = (list_op_result *) userp;
    const char *keys = NULL;

    mem->body = realloc(mem->body, mem->size + realsize + 1);
    if (mem->body == NULL) {
        /* out of memory! */
        mem->isOK = false;
        mem->msg = "Not enough memory";
        return 0;
    }

    memcpy(&(mem->body[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->body[mem->size] = 0;
    //
    json_object * jobj = json_tokener_parse(mem->body);
    enum json_type type;

    json_object_object_foreach(jobj, key, val) {
        type = json_object_get_type(val);

        if (type == json_type_array) {
            jobj = json_object_object_get(jobj, key);
            int arraylen = json_object_array_length(jobj);
            mem->klist = (char**) malloc(arraylen * sizeof (char*));
            int i;
            json_object * jvalue;
            for (i = 0; i < arraylen; i++) {
                jvalue = json_object_array_get_idx(jobj, i);
                keys = json_object_get_string(jvalue);
                mem->klist[i] = (char*) malloc(strlen(keys) * sizeof (char));
                strcpy(mem->klist[i], keys);
            }
        } else {
            mem->isOK = false;
            mem->msg = "Not an array";
            return 0;
        }
    }
    mem->isOK = true;
    mem->msg = "Success";

    return realsize;
}

size_t GetKeyCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    get_op_result *mem = (get_op_result *) userp;

    mem->body = realloc(mem->body, mem->size + realsize + 1);
    if (mem->body == NULL) {
        /* out of memory! */
        mem->isOK = false;
        mem->msg = "Not enough memory";
        return 0;
    }

    memcpy(&(mem->body[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->body[mem->size] = 0;
    mem->isOK = true;
    mem->msg = "Success";

    return realsize;
}


size_t parse_ping_result(void *buffer, size_t size, size_t nmemb, r_ping_res *st) {

    if (0 == strcasecmp((char *) buffer, "OK")) {
        st->isOK = true;
    } else {
        st->isOK = false;
    }
    return size * nmemb;
}
