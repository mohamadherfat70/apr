/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 */

#include "apr_private.h"

#include "apr_lib.h"
#include "apr_xlate.h"

/* If no implementation is available, don't generate code here since
 * apr_xlate.h emitted macros which return APR_ENOTIMPL.
 */

#if APR_HAS_XLATE

#ifdef HAVE_ICONV_H
#include <iconv.h>
#endif

#ifndef min
#define min(x,y) ((x) <= (y) ? (x) : (y))
#endif

struct ap_xlate_t {
    ap_pool_t *pool;
    char *frompage;
    char *topage;
    char *sbcs_table;
#ifdef HAVE_ICONV
    iconv_t ich;
#endif
};

/* get_default_codepage()
 *
 * simple heuristic to determine codepage of source code so that
 * literal strings (e.g., "GET /\r\n") in source code can be translated
 * properly
 *
 * If appropriate, a symbol can be set at configure time to determine
 * this.  On EBCDIC platforms, it will be important how the code was
 * unpacked.
 */

static const char *get_default_codepage(void)
{
#ifdef __MVS__
    #ifdef __CODESET__
        return __CODESET__;
    #else
        return "IBM-1047";
    #endif
#endif

    if ('}' == 0xD0) {
        return "IBM-1047";
    }

    if ('{' == 0xFB) {
        return "EDF04";
    }

    if ('A' == 0xC1) {
        return "EBCDIC"; /* not useful */
    }

    if ('A' == 0x41) {
        return "ISO8859-1"; /* not necessarily true */
    }

    return "unknown";
}

static ap_status_t ap_xlate_cleanup(void *convset)
{
#ifdef HAVE_ICONV
    ap_xlate_t *old = convset;

    if (old->ich != (iconv_t)-1) {
        if (iconv_close(old->ich)) {
            return errno;
        }
    }
#endif
    return APR_SUCCESS;
}

#ifdef HAVE_ICONV
static void check_sbcs(ap_xlate_t *convset)
{
    char inbuf[256], outbuf[256];
    char *inbufptr = inbuf, *outbufptr = outbuf;
    size_t inbytes_left, outbytes_left;
    int i;
    size_t translated;

    for (i = 0; i < sizeof(inbuf); i++) {
        inbuf[i] = i;
    }

    inbytes_left = outbytes_left = sizeof(inbuf);
    translated = iconv(convset->ich, (const char **)&inbufptr, 
                       &inbytes_left, &outbufptr, &outbytes_left);
    if (translated != (size_t) -1 &&
        inbytes_left == 0 &&
        outbytes_left == 0) {
        /* hurray... this is simple translation; save the table,
         * close the iconv descriptor
         */
        
        convset->sbcs_table = ap_palloc(convset->pool, sizeof(outbuf));
        memcpy(convset->sbcs_table, outbuf, sizeof(outbuf));
        iconv_close(convset->ich);
        convset->ich = (iconv_t)-1;

        /* TODO: add the table to the cache */
    }
}
#endif /* HAVE_ICONV */

ap_status_t ap_xlate_open(ap_xlate_t **convset, const char *topage,
                          const char *frompage, ap_pool_t *pool)
{
    ap_status_t status;
    ap_xlate_t *new;
    int found = 0;

    *convset = NULL;
    
    if (!topage) {
        topage = get_default_codepage();
    }

    if (!frompage) {
        frompage = get_default_codepage();
    }
    
    new = (ap_xlate_t *)ap_pcalloc(pool, sizeof(ap_xlate_t));
    if (!new) {
        return APR_ENOMEM;
    }

    new->pool = pool;
    new->topage = ap_pstrdup(pool, topage);
    new->frompage = ap_pstrdup(pool, frompage);
    if (!new->topage || !new->frompage) {
        return APR_ENOMEM;
    }

#ifdef TODO
    /* search cache of codepage pairs; we may be able to avoid the
     * expensive iconv_open()
     */

    set found to non-zero if found in the cache
#endif

#ifdef HAVE_ICONV
    if (!found) {
        new->ich = iconv_open(topage, frompage);
        if (new->ich == (iconv_t)-1) {
            return errno;
        }
        found = 1;
        check_sbcs(new);
    }
#endif /* HAVE_ICONV */

    if (found) {
        *convset = new;
        ap_register_cleanup(pool, (void *)new, ap_xlate_cleanup,
                            ap_null_cleanup);
        status = APR_SUCCESS;
    }
    else {
        status = EINVAL; /* same as what iconv() would return if it
                            couldn't handle the pair */
    }
    
    return status;
}

ap_status_t ap_xlate_conv_buffer(ap_xlate_t *convset, const char *inbuf,
                                 ap_size_t *inbytes_left, char *outbuf,
                                 ap_size_t *outbytes_left)
{
    ap_status_t status = APR_SUCCESS;
#ifdef HAVE_ICONV
    size_t translated;

    if (convset->ich != (iconv_t)-1) {
        char *inbufptr = (char *)inbuf;
        char *outbufptr = outbuf;
        
        translated = iconv(convset->ich, (const char **)&inbufptr, 
                           inbytes_left, &outbufptr, outbytes_left);
        if (translated == (size_t)-1) {
            return errno;
        }
    }
    else
#endif
    {
        int to_convert = min(*inbytes_left, *outbytes_left);
        int converted = to_convert;
        char *table = convset->sbcs_table;
        
        while (to_convert) {
            *outbuf = table[(unsigned char)*inbuf];
            ++outbuf;
            ++inbuf;
            --to_convert;
        }
        *inbytes_left -= converted;
        *outbytes_left -= converted;
    }

    return status;
}

ap_int32_t ap_xlate_conv_byte(ap_xlate_t *convset, unsigned char inchar)
{
    if (convset->sbcs_table) {
        return convset->sbcs_table[inchar];
    }
    else {
        return -1;
    }
}

ap_status_t ap_xlate_close(ap_xlate_t *convset)
{
    ap_status_t status;

    if ((status = ap_xlate_cleanup(convset)) == APR_SUCCESS) {
        ap_kill_cleanup(convset->pool, convset, ap_xlate_cleanup);
    }

    return status;
}

#endif /* APR_HAS_XLATE */
