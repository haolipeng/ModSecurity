/*
 * ModSecurity for Apache 2.x, http://www.modsecurity.org/
 * Copyright (c) 2004-2007 Breach Security, Inc. (http://www.breach.com/)
 *
 * You should have received a copy of the licence along with this
 * program (stored in the file "LICENSE"). If the file is missing,
 * or if you have any other questions related to the licence, please
 * write to Breach Security, Inc. at support@breach.com.
 *
 */
#include "msc_parsers.h"
#include "iconv.h"
#include <ctype.h>
#include <errno.h>

/**
 *
 */
int parse_cookies_v0(modsec_rec *msr, char *_cookie_header, apr_table_t *cookies) {
    char *attr_name = NULL, *attr_value = NULL;
    char *cookie_header;
    char *saveptr = NULL;
    int cookie_count = 0;
    char *p = NULL;

    if (_cookie_header == NULL) {
        msr_log(msr, 1, "Cookie parser: Received null for argument.");
        return -1;
    }

    cookie_header = strdup(_cookie_header);
    if (cookie_header == NULL) return -1;

    p = apr_strtok(cookie_header, ";", &saveptr);
    while(p != NULL) {
        attr_name = NULL;
        attr_value = NULL;

        /* ignore whitespace at the beginning of cookie name */
        while(isspace(*p)) p++;
        attr_name = p;

        attr_value = strstr(p, "=");
        if (attr_value != NULL) {
            /* terminate cookie name */
            *attr_value = 0;
            /* move over to the beginning of the value */
            attr_value++;
        }

        /* we ignore cookies with empty names */
        if ((attr_name != NULL)&&(strlen(attr_name) != 0)) {
            if (attr_value != NULL) {
                msr_log(msr, 5, "Adding request cookie: name \"%s\", value \"%s\"", log_escape(msr->mp, attr_name), log_escape(msr->mp, attr_value));
                apr_table_add(cookies, attr_name, attr_value);
            } else {
                msr_log(msr, 5, "Adding request cookie: name \"%s\", value empty", log_escape(msr->mp, attr_name));
                apr_table_add(cookies, attr_name, "");
            }

            cookie_count++;
        }

        p = apr_strtok(NULL, ";", &saveptr);
    }

    free(cookie_header);
    return cookie_count;
}

/**
 *
 */
int parse_cookies_v1(modsec_rec *msr, char *_cookie_header, apr_table_t *cookies) {
    char *attr_name = NULL, *attr_value = NULL, *p = NULL;
    char *prev_attr_name = NULL;
    char *cookie_header = NULL;
    int cookie_count = 0;

    if (_cookie_header == NULL) return -1;
    cookie_header = strdup(_cookie_header);
    if (cookie_header == NULL) return -1;

    p = cookie_header;
    while(*p != 0) {
        attr_name = NULL;
        attr_value = NULL;

        /* attribute name */

        /* remove space from the beginning */
        while((isspace(*p))&&(*p != 0)) p++;
        attr_name = p;
        while((*p != 0)&&(*p != '=')&&(*p != ';')&&(*p != ',')) p++;

        /* if we've reached the end of string */
        if (*p == 0) goto add_cookie;

        /* if there is no cookie value supplied */
        if ((*p == ';')||(*p == ',')) {
            *p++ = 0; /* terminate the name */
            goto add_cookie;
        }

        /* terminate the attribute name,
         * writing over the = character
         */
        *p++ = 0;

        /* attribute value */

        /* skip over the whitespace at the beginning */
        while((isspace(*p))&&(*p != 0)) p++;

        /* no value supplied */
        if (*p == 0) goto add_cookie;

        if (*p == '"') {
            if (*++p == 0) goto add_cookie;
            attr_value = p;
            while((*p != 0)&&(*p != '"')) p++;
            if (*p != 0) *p++ = 0;
            else {
                /* Do nothing about this. */
            }
        } else {
            attr_value = p;
            while((*p != 0)&&(*p != ',')&&(*p != ';')) p++;
            if (*p != 0) *p++ = 0;

            /* remove the whitespace from the end of cookie value */
            if (attr_value != NULL) {
                char *t = attr_value;
                int i = 0;

                while(*t != 0) {
                    t++;
                    i++;
                }

                while((i-- > 0)&&(isspace(*(--t)))) *t = 0;
            }
        }

    add_cookie:

        /* remove the whitespace from the end of cookie name */
        if (attr_name != NULL) {
            char *t = attr_name;
            int i = 0;

            while(*t != 0) {
                t++;
                i++;
            }

            while((i-- > 0)&&(isspace(*(--t)))) *t = 0;
        }

        /* add the cookie to the list now */
        if ((attr_name != NULL)&&(strlen(attr_name) != 0)) {

            /* handle special attribute names */
            if (attr_name[0] == '$') {
                if (prev_attr_name != NULL) {
                    /* cookie keyword, we change the name we use
                     * so they can have a unique name in the cookie table
                     */
                    attr_name = apr_psprintf(msr->mp, "$%s_%s", prev_attr_name, attr_name + 1);
                }
            }

            if (attr_value != NULL) {
                msr_log(msr, 5, "Adding request cookie: name \"%s\", value \"%s\"",
                    log_escape(msr->mp, attr_name), log_escape(msr->mp, attr_value));
                apr_table_add(cookies, attr_name, attr_value);
            } else {
                msr_log(msr, 5, "Adding request cookie: name \"%s\", value empty",
                    log_escape(msr->mp, attr_name));
                apr_table_add(cookies, attr_name, "");
            }

            cookie_count++;

            /* only keep the cookie names for later */
            if (attr_name[0] != '$') prev_attr_name = attr_name;
        }

        /* at this point the *p is either 0 (in which case we exit), or
         * right after the current cookie ended - we need to look for
         * the next cookie
         */
        while( (*p != 0)&&( (*p == ',')||(*p == ';')||(isspace(*p)) ) ) p++;
    }

    return cookie_count;
}

/**
 *
 */
int parse_arguments(modsec_rec *msr, const char *s, apr_size_t inputlength,
    int argument_separator, const char *origin,
    apr_table_t *arguments, int *invalid_count)
{
    msc_arg *arg;
    apr_size_t i, j;
    char *value = NULL;
    char *buf;
    int status;

    if (s == NULL) return -1;
    if (inputlength == 0) return 1;

    /* Check that adding one will not overflow */
    if (inputlength + 1 <= 0) return -1;

    buf = (char *)malloc(inputlength + 1);
    if (buf == NULL) return -1;

    arg = (msc_arg *)apr_pcalloc(msr->mp, sizeof(msc_arg));
    arg->origin = origin;

    i = 0;
    j = 0;
    status = 0;
    *invalid_count = 0;
    while (i < inputlength) {
        if (status == 0) {
            /* parameter name */
            arg->name_origin_offset = i;
            while ((s[i] != '=') && (s[i] != argument_separator) && (i < inputlength)) {
                buf[j] = s[i];
                j++;
                i++;
            }
            buf[j++] = '\0';
            arg->name_origin_len = i - arg->name_origin_offset;
        } else {
            /* parameter value */
            arg->value_origin_offset = i;
            while ((s[i] != argument_separator) && (i < inputlength)) {
                buf[j] = s[i];
                j++;
                i++;
            }
            buf[j++] = '\0';
            arg->value_origin_len = i - arg->value_origin_offset;
        }

        if (status == 0) {
            arg->name_len = urldecode_nonstrict_inplace_ex((unsigned char *)buf, arg->name_origin_len, invalid_count);
            arg->name = apr_pstrmemdup(msr->mp, buf, arg->name_len);

            if (s[i] == argument_separator) {
                /* Empty parameter */
                arg->value_len = 0;
                arg->value = "";

                // apr_table_addn(arguments, arg->name, (void *)arg);
                add_argument(msr, arguments, arg);

                arg = (msc_arg *)apr_pcalloc(msr->mp, sizeof(msc_arg));
                arg->origin = origin;
                
                status = 0; /* unchanged */
                j = 0;
            } else {
                status = 1;
                value = &buf[j];
            }
        }
        else {
            arg->value_len = urldecode_nonstrict_inplace_ex((unsigned char *)value, arg->value_origin_len, invalid_count);
            arg->value = apr_pstrmemdup(msr->mp, value, arg->value_len);

            // apr_table_addn(arguments, arg->name, (void *)arg);
            add_argument(msr, arguments, arg);

            arg = (msc_arg *)apr_pcalloc(msr->mp, sizeof(msc_arg));
            arg->origin = origin;

            status = 0;
            j = 0;
        }

        i++; /* skip over the separator */
    }

    /* the last parameter was empty */
    if (status == 1) {
        arg->value_len = 0;
        arg->value = "";

        // apr_table_addn(arguments, arg->name, (void *)arg);
        add_argument(msr, arguments, arg);
    }

    free(buf);

    return 1;
}

/**
 *
 */
void add_argument(modsec_rec *msr, apr_table_t *arguments, msc_arg *arg) {
    msr_log(msr, 5, "Adding request argument (%s): name \"%s\", value \"%s\"",
        arg->origin, log_escape_ex(msr->mp, arg->name, arg->name_len),
        log_escape_ex(msr->mp, arg->value, arg->value_len));

    #ifdef WITH_ICONV
    if (msr->txcfg->request_encoding != NULL) {
        iconv_t convset;

        // TODO Convert parameter names too.

        /* Initialise iconv. */
        convset = iconv_open("ISO-8859-1", msr->txcfg->request_encoding);
        if (convset == (iconv_t)(-1)) {
            msr_log(msr, 1, "Iconv init to %s failed: %s",
                msr->txcfg->request_encoding, strerror(errno));
        } else {
            int ctlparam = 1;
            size_t input_bytes = arg->value_len;
            size_t output_bytes = arg->value_len;
            char *o, *outbuf;

            // TODO Can output be longer than input?
            o = outbuf = apr_palloc(msr->mp, output_bytes);

            /* Tell iconv to reject invalid character sequences. */
            iconvctl(convset, ICONV_SET_DISCARD_ILSEQ, &ctlparam);

            /* Convert input character sequence. */
            if (iconv(convset, (const char **)&arg->value,
                (size_t *)&input_bytes,
                (char **)&outbuf,
                (size_t *)&output_bytes) == (size_t)(-1))
            {
                msr_log(msr, 1, "Error converting to %s: %s",
                    msr->txcfg->request_encoding, strerror(errno));
            } else {
                arg->value = o;
                arg->value_len = arg->value_len - output_bytes;

                msr_log(msr, 5, "Parameter value after conversion from %s: %s",
                    msr->txcfg->request_encoding,
                    log_escape_nq_ex(msr->mp, arg->value, arg->value_len));
            }

            iconv_close(convset);
        }
    }
    #endif

    apr_table_addn(arguments, arg->name, (void *)arg);
}
