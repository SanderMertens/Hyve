/* Copyright (c) 2010-2018 the corto developers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <corto>
#include "object.h"
#include "memory_ser.h"
#include "init_ser.h"

struct corto_fmt_s {
    corto_string name;
    bool isBinary;

    /* Translate values to and from a format value */
    void* ___ (*fromValue)(
        corto_fmt_opt *data,
        corto_value *v);

    int16_t ___ (*toValue)(
        corto_fmt_opt *data,
        corto_value *v,
        const void* content);

    /* Translate results to and from self-contained format values */
    void* ___ (*fromResult)(
        corto_fmt_opt *data,
        corto_record *o);

    int16_t ___ (*toResult)(
        corto_fmt_opt *data,
        corto_record* o,
        const void* content);

    /* Translate objects to and from self-contained format values */
    void* ___ (*fromObject)(
        corto_fmt_opt *data,
        corto_object o);

    int16_t ___ (*toObject)(
        corto_fmt_opt *data,
        corto_object* o,
        const void* content);

    /* Duplicate a format value */
    void* ___ (*copy)(
        const void* content);

    /* Free a format value */
    void (*release)(
        void* content);
};

extern ut_mutex_s corto_adminLock;

static ut_ll formats = NULL;

static
void* corto_fmt_ptr_fromValue(
    corto_fmt_opt *opt,
    corto_value *v)
{
    corto_type t = corto_value_typeof(v);
    void *ptr = corto_mem_new(t);

    if (t->flags & CORTO_TYPE_HAS_RESOURCES) {
        corto_value dst = corto_value_mem(ptr, t);
        corto_value_copy(&dst, v);
    } else {
        memcpy(ptr, corto_value_ptrof(v), t->size);
    }

    return ptr;
}

static
int16_t corto_fmt_ptr_toValue(
    corto_fmt_opt *opt,
    corto_value *v,
    const void* ptr)
{
    corto_type t = corto_type(*(corto_type*)CORTO_OFFSET(ptr, -sizeof(corto_type)));
    *v = corto_value_mem((void*)ptr, t);
    return 0;
}

static
void corto_fmt_ptr_release(
    void* ptr)
{
    if (ptr) {
        corto_mem_free((void*)ptr);
    }
}

static
void* corto_fmt_ptr_copy(
    const void* src)
{
    corto_type t = corto_mem_typeof((void*)src);
    corto_value srcValue = corto_value_mem((void*)src, t);
    return corto_fmt_ptr_fromValue(NULL, &srcValue);
}

static
void* corto_fmt_str_fromValue(
    corto_fmt_opt *opt,
    corto_value *v)
{
    corto_string_ser_t serData;
    corto_walk_opt s = corto_string_ser(
        CORTO_LOCAL|CORTO_READONLY|CORTO_PRIVATE, CORTO_NOT, CORTO_WALK_TRACE_NEVER);

    serData.buffer = UT_STRBUF_INIT;
    serData.buffer.max = 0;
    serData.compactNotation = TRUE;
    serData.prefixType = FALSE;
    serData.enableColors = FALSE;

    corto_walk_value(&s, v, &serData);
    corto_string result = ut_strbuf_get(&serData.buffer);
    corto_walk_deinit(&s, &serData);

    return result;
}

static
int16_t corto_fmt_str_toValue(
    corto_fmt_opt *opt,
    corto_value *v,
    const void* str)
{
    corto_string_deser_t serData = {
        .out = corto_value_ptrof(v),
        .type = corto_value_typeof(v),
        .isObject = v->kind == CORTO_OBJECT
    };

    if (!corto_string_deser((char*)str, &serData)) {
        ut_assert(!serData.out, "deserializer failed but out is set");
    }

    if (serData.out) {
        corto_value_ptrset(v, serData.out);
    } else {
        goto error;
    }

    return 0;
error:
    return -1;
}

static
int16_t corto_fmt_str_toObject(
    corto_fmt_opt *opt,
    corto_object *o,
    const void* data)
{
    corto_object obj = *(void**)o;
    corto_string_deser_t serData = {
        .out = obj,
        .type = obj ? corto_typeof(obj) : NULL,
        .isObject = TRUE
    };

    if (!corto_string_deser((char*)data, &serData)) {
        ut_assert(!serData.out, "deserializer failed but out is set");
    }

    if (serData.out) {
        *(void**)o = serData.out;
    } else {
        return -1;
    }
    return 0;
}

static
corto_fmt corto_findContentType(
    bool isBinary,
    corto_string format)
{
    corto_fmt result = NULL;
    if (formats) {
        ut_iter it = ut_ll_iter(formats);
        while (ut_iter_hasNext(&it)) {
            corto_fmt ct = ut_iter_next(&it);
            if (!strcmp(ct->name, format) && (ct->isBinary == isBinary)) {
                result = ct;
                break;
            }
        }
    } else {
        formats = ut_ll_new();
    }

    if (!result && !strcmp(format, "corto") && !isBinary) {
        result = corto_calloc(sizeof(struct corto_fmt_s));
        result->name = ut_strdup("corto");
        result->isBinary = isBinary;
        result->toValue = corto_fmt_str_toValue;
        result->fromValue = corto_fmt_str_fromValue;
        result->release = (void ___ (*)(void*))corto_dealloc;
        result->copy = (void* ___ (*)(const void*)) ut_strdup;
        result->toObject = corto_fmt_str_toObject;
        ut_ll_append(formats, result);

    } else if (!result && !strcmp(format, "corto") && isBinary) {
        result = corto_calloc(sizeof(struct corto_fmt_s));
        result->name = ut_strdup("corto");
        result->isBinary = isBinary;
        result->toValue = corto_fmt_ptr_toValue;
        result->fromValue = corto_fmt_ptr_fromValue;
        result->release = corto_fmt_ptr_release;
        result->copy = corto_fmt_ptr_copy;
        ut_ll_append(formats, result);
    }

    return result;
}

corto_fmt
corto_fmt_lookup(
    const char *format)
{
    corto_fmt result = NULL;
    bool isBinary = true;

    /* Built-in Corto string serializer */
    char *packagePtr = strchr(format, '/');
    if (!packagePtr) {
        ut_throw("invalid content type %s (expected '/')", format);
        goto error;
    }

    if (!memcmp(format, "text", packagePtr - format)) {
        isBinary = false;
    }

    packagePtr ++;

    /* Find content type in admin */
    ut_mutex_lock(&corto_adminLock);
    result = corto_findContentType(isBinary, packagePtr);
    ut_mutex_unlock(&corto_adminLock);

    /* Load format outside of lock */
    if (!result) {
        ut_dl dl = NULL;
        corto_id packageId;
        sprintf(packageId, "driver.fmt.%s", packagePtr);

        result = corto_alloc(sizeof(struct corto_fmt_s));
        result->name = ut_strdup(packagePtr);
        result->isBinary = isBinary;

        /* Load package associated with content type */
        if (ut_use(packageId, 0, NULL)) {
            ut_throw("unresolved package '%s' for format '%s'",
                packageId, format);
            goto error;
        }

        /* Load serialization routines */
        corto_id id;
        sprintf(id, "%s_fromValue", packagePtr);
        result->fromValue =
            (void* ___ (*)(corto_fmt_opt*, corto_value*))ut_load_proc(packageId, &dl, id);
        if (!result->fromValue) {
            ut_throw("symbol '%s' missing for format '%s'", id, format);
            goto error;
        }

        sprintf(id, "%s_toValue", packagePtr);
        result->toValue =
            (int16_t ___ (*)(corto_fmt_opt*, corto_value*, const void*))
                ut_load_proc(packageId, &dl, id);
        if (!result->toValue) {
            ut_throw("symbol '%s' missing for format '%s'", id, format);
            goto error;
        }


        sprintf(id, "%s_fromResult", packagePtr);
        result->fromResult =
          (void* ___ (*)(corto_fmt_opt*, corto_record*))
            ut_load_proc(packageId, &dl, id);
        if (!result->fromResult) {
            ut_throw(
                "symbol '%s' missing for format '%s'", id, format);
            goto error;
        }

        sprintf(id, "%s_toResult", packagePtr);
        result->toResult =
          (int16_t ___ (*)(corto_fmt_opt*, corto_record*, const void*))
            ut_load_proc(packageId, &dl, id);
        if (!result->toResult) {
            ut_throw(
                "symbol '%s' missing for format '%s'", id, format);
            goto error;
        }


        sprintf(id, "%s_fromObject", packagePtr);
        result->fromObject =
          (void* ___ (*)(corto_fmt_opt*, corto_object))
            ut_load_proc(packageId, &dl, id);
        if (!result->fromObject) {
            ut_throw(
                "symbol '%s' missing for format '%s'", id, format);
            goto error;
        }

        sprintf(id, "%s_toObject", packagePtr);
        result->toObject =
          (int16_t ___ (*)(corto_fmt_opt*, corto_object*, const void*))
            ut_load_proc(packageId, &dl, id);
        if (!result->toObject) {
            ut_throw(
                "symbol '%s' missing for format '%s'", id, format);
            goto error;
        }

        sprintf(id, "%s_copy", packagePtr);
        result->copy =
            (void* ___ (*)(const void*))ut_load_proc(packageId, &dl, id);
        if (!result->copy) {
            ut_throw(
                "symbol '%s' missing for format '%s'", id, format);
            goto error;
        }

        sprintf(id, "%s_release", packagePtr);
        result->release =
            (void ___ (*)(void*))ut_load_proc(packageId, &dl, id);
        if (!result->release) {
            ut_throw(
                "symbol '%s' missing for format '%s'", id, format);
            goto error;
        }

        /* Add to admin, verify that it hasn't been already added by another
         * thread */
         ut_mutex_lock(&corto_adminLock);
         corto_fmt alreadyAdded = corto_findContentType(isBinary, packagePtr);
         if (!alreadyAdded) {
            ut_ll_append(formats, result);
         } else {
            corto_dealloc(result->name);
            corto_dealloc(result);
            result = alreadyAdded;
         }
         ut_mutex_unlock(&corto_adminLock);
    }

    return result;
error:
    return NULL;
}

void corto_fmt_deinit(void)
{
    if (formats) {
        ut_iter it = ut_ll_iter(formats);
        while (ut_iter_hasNext(&it)) {
            corto_fmt fmt = ut_iter_next(&it);
            if (fmt->name) free(fmt->name);
            free(fmt);
        }
        ut_ll_free(formats);
    }
}

void* corto_fmt_from_value(
    corto_fmt fmt,
    corto_fmt_opt *opt,
    corto_value *v)
{
    return fmt->fromValue(opt, v);
}

int16_t corto_fmt_to_value(
    corto_fmt fmt,
    corto_fmt_opt *opt,
    corto_value *v,
    const void *data)
{
    return fmt->toValue(opt, v, data);
}

void* corto_fmt_from_result(
    corto_fmt fmt,
    corto_fmt_opt *opt,
    corto_record *result)
{
    return fmt->fromResult(opt, result);
}

int16_t corto_fmt_to_result(
    corto_fmt fmt,
    corto_fmt_opt *opt,
    corto_record *result,
    const void *data)
{
    return fmt->toResult(opt, result, data);
}

void* corto_fmt_from_object(
    corto_fmt fmt,
    corto_fmt_opt *opt,
    corto_object object)
{
    return fmt->fromObject(opt, object);
}

int16_t corto_fmt_to_object(
    corto_fmt fmt,
    corto_fmt_opt *opt,
    corto_object *object_out,
    const void *data)
{
    return fmt->toObject(opt, object_out, data);
}

void* corto_fmt_copy(
    corto_fmt fmt,
    const void *data)
{
    return fmt->copy(data);
}

void corto_fmt_release(
    corto_fmt fmt,
    void *data)
{
    if (fmt->release) {
        fmt->release(data);
    }
}
