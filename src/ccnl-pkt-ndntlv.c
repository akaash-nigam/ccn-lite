/*
 * @f pkt-ndntlv.c
 * @b CCN lite - NDN pkt decoding and composition (TLV pkt format March 2014)
 *
 * Copyright (C) 2014, Christian Tschudin, University of Basel
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * File history:
 * 2014-03-05 created
 * 2014-11-05 merged from pkt-ndntlv-enc.c pkt-ndntlv-dec.c
 */

#ifndef PKT_NDNTLV_C
#define PKT_NDNTLV_C

#include "ccnl-pkt-ndntlv.h"

// ----------------------------------------------------------------------
// packet parsing

static int
ccnl_ndntlv_varlenint(unsigned char **buf, int *len, int *val)
{

    if (**buf < 253 && *len >= 1) {
        *val = **buf;
        *buf += 1;
        *len -= 1;
    } else if (**buf == 253 && *len >= 3) { // 2 bytes
        *val = ntohs(*(uint16_t*)(*buf + 1));
        *buf += 3;
        *len -= 3;
    } else if (**buf == 254 && *len >= 5) { // 4 bytes
        *val = ntohl(*(uint32_t*)(*buf + 1));
        *buf += 5;
        *len -= 5;
    } else {
        // not implemented yet (or too short)
        return -1;
    }
    return 0;
}


unsigned long int
ccnl_ndntlv_nonNegInt(unsigned char *cp, int len)
{
    unsigned long int val = 0;

    while (len-- > 0) {
        val = (val << 8) | *cp;
        cp++;
    }
    return val;
}


int
ccnl_ndntlv_dehead(unsigned char **buf, int *len,
                   int *typ, int *vallen)
{
    if (ccnl_ndntlv_varlenint(buf, len, typ))
        return -1;
    if (ccnl_ndntlv_varlenint(buf, len, vallen))
        return -1;
    return 0;
}

// we use one extraction routine for both interest and data pkts
struct ccnl_buf_s*
ccnl_ndntlv_extract(int hdrlen,
                    unsigned char **data, int *datalen,
                    int *scope, int *mbf, int *min, int *max,
                    unsigned int *final_block_id,
                    struct ccnl_prefix_s **prefix,
                    struct ccnl_prefix_s **tracing,
                    struct ccnl_buf_s **nonce,
                    struct ccnl_buf_s **ppkl,
                    unsigned char **content, int *contlen)
{
    unsigned char *start = *data - hdrlen;
    int i, len, typ, oldpos;
    struct ccnl_prefix_s *p;
    struct ccnl_buf_s *buf, *n = 0, *pub = 0;

    DEBUGMSG(DEBUG, "extracting NDNTLV packet\n");

    if (content)
        *content = NULL;

    p = ccnl_prefix_new(CCNL_SUITE_NDNTLV, CCNL_MAX_NAME_COMP);
    p->compcnt = 0;
    if (!p)
        return NULL;

    oldpos = *data - start;
    while (ccnl_ndntlv_dehead(data, datalen, &typ, &len) == 0) {
        unsigned char *cp = *data;
        int len2 = len;

        switch (typ) {
        case NDN_TLV_Name:
            p->nameptr = start + oldpos;
            while (len2 > 0) {
                if (ccnl_ndntlv_dehead(&cp, &len2, &typ, &i))
                    goto Bail;
                if (typ == NDN_TLV_NameComponent &&
                            p->compcnt < CCNL_MAX_NAME_COMP) {
                    if(cp[0] == NDN_Marker_SegmentNumber) {
                        p->chunknum = ccnl_malloc(sizeof(int));
                        // TODO: requires ccnl_ndntlv_includedNonNegInt which includes the length of the marker
                        // it is implemented for encode, the decode is not yet implemented
                        *p->chunknum = ccnl_ndntlv_nonNegInt(cp + 1, i - 1);
                    }
                    p->comp[p->compcnt] = cp;
                    p->complen[p->compcnt] = i;
                    p->compcnt++;
                }  // else unknown type: skip
                cp += i;
                len2 -= i;
            }
            p->namelen = *data - p->nameptr;
    #ifdef USE_NFN
            if (p->compcnt > 0 && p->complen[p->compcnt-1] == 3 &&
                    !memcmp(p->comp[p->compcnt-1], "NFN", 3)) {
                p->nfnflags |= CCNL_PREFIX_NFN;
                p->compcnt--;
                if (p->compcnt > 0 && p->complen[p->compcnt-1] == 5 &&
                        !memcmp(p->comp[p->compcnt-1], "THUNK", 5)) {
                    p->nfnflags |= CCNL_PREFIX_THUNK;
                    p->compcnt--;
                }
            }
    #endif
            break;
        case NDN_TLV_Selectors:
            while (len2 > 0) {
                if (ccnl_ndntlv_dehead(&cp, &len2, &typ, &i))
                    goto Bail;

                if (typ == NDN_TLV_MinSuffixComponents && min)
                    *min = ccnl_ndntlv_nonNegInt(cp, i);
                if (typ == NDN_TLV_MinSuffixComponents && max)
                    *max = ccnl_ndntlv_nonNegInt(cp, i);
                if (typ == NDN_TLV_MustBeFresh && mbf)
                    *mbf = 1;
                if (typ == NDN_TLV_Exclude) {
                    DEBUGMSG(WARNING, "'Exclude' field ignored\n");
                }
                cp += i;
                len2 -= i;
            }
            break;
        case NDN_TLV_Nonce:
            if (!n)
                n = ccnl_buf_new(*data, len);
            break;
        case NDN_TLV_Scope:
            if (scope)
                *scope = ccnl_ndntlv_nonNegInt(*data, len);
            break;
        case NDN_TLV_Content:
            if (content) {
                *content = *data;
                *contlen = len;
            }
            break;
        case NDN_TLV_MetaInfo:
            while (len2 > 0) {
                if (ccnl_ndntlv_dehead(&cp, &len2, &typ, &i))
                    goto Bail;
                if (typ == NDN_TLV_ContentType) {
                    // Not used
                    // = ccnl_ndntlv_nonNegInt(cp, i);
                    DEBUGMSG(WARNING, "'ContentType' field ignored\n");
                }
                if (typ == NDN_TLV_FreshnessPeriod)
                    // Not used
                    // = ccnl_ndntlv_nonNegInt(cp, i);
                    DEBUGMSG(WARNING, "'FreshnessPeriod' field ignored\n");
                if (typ == NDN_TLV_FinalBlockId) {
                    if (ccnl_ndntlv_dehead(&cp, &len2, &typ, &i))
                        goto Bail;
                    if (typ == NDN_TLV_NameComponent && final_block_id) {
                        // TODO: again, includedNonNeg not yet implemented
                        *final_block_id = ccnl_ndntlv_nonNegInt(cp + 1, i - 1);
                    } 
                }
                cp += i;
                len2 -= i;
            }
            break;
        default:
            break;
        }
        *data += len;
        *datalen -= len;
        oldpos = *data - start;
    }
    if (*datalen > 0)
        goto Bail;

    if (prefix)    *prefix = p;    else free_prefix(p);
    if (nonce)     *nonce = n;     else ccnl_free(n);
    if (ppkl)      *ppkl = pub;    else ccnl_free(pub);

    buf = ccnl_buf_new(start, *data - start);
    // carefully rebase ptrs to new buf because of 64bit pointers:
    if (content && *content)
        *content = buf->data + (*content - start);
    for (i = 0; i < p->compcnt; i++)
        p->comp[i] = buf->data + (p->comp[i] - start);
    if (p->nameptr)
        p->nameptr = buf->data + (p->nameptr - start);

    return buf;
Bail:
    free_prefix(p);
    free_2ptr_list(n, pub);
    return NULL;
}

// ----------------------------------------------------------------------
// packet composition

#ifdef NEEDS_PACKET_CRAFTING

int
ccnl_ndntlv_prependTLval(unsigned long val, int *offset, unsigned char *buf)
{
    int len, i, t;

    if (val < 253)
        len = 0, t = val;
    else if (val <= 0xffff)
        len = 2, t = 253;
    else if (val <= 0xffffffffL)
        len = 4, t = 254;
    else
        len = 8, t = 255;
    if (*offset < (len+1))
        return -1;

    for (i = 0; i < len; i++) {
        buf[--(*offset)] = val & 0xff;
        val = val >> 8;
    }
    buf[--(*offset)] = t;
    return len + 1;
}

int
ccnl_ndntlv_prependTL(int type, unsigned int len,
                      int *offset, unsigned char *buf)
{
    int oldoffset = *offset;
    if (ccnl_ndntlv_prependTLval(len, offset, buf) < 0)
        return -1;
    if (ccnl_ndntlv_prependTLval(type, offset, buf) < 0)
        return -1;
    return oldoffset - *offset;
}

int
ccnl_ndntlv_prependNonNegIntVal(unsigned int val,
                                int *offset, unsigned char *buf) {
    int len = 0, i;
    static char fill[] = {1, 0, 0, 1, 0, 3, 2, 1, 0};

    while (val) {
        if ((*offset)-- < 1)
            return -1;
        buf[*offset] = (unsigned char) (val & 0xff);
        len++;
        val = val >> 8;
    }
    for (i = fill[len]; i > 0; i--) {
        if ((*offset)-- < 1)
            return -1;
        buf[*offset] = 0;
        len++;
    }
    return len;
}

int
ccnl_ndntlv_prependNonNegInt(int type, 
                             unsigned int val,
                             int *offset, unsigned char *buf)
{
    int oldoffset = *offset;
    if (ccnl_ndntlv_prependNonNegIntVal(val, offset, buf) < 0)
        return -1;
    if (ccnl_ndntlv_prependTL(type, oldoffset - *offset, offset, buf) < 0)
        return -1;
    return oldoffset - *offset;
}


int
ccnl_ndntlv_prependIncludedNonNegInt(int type, unsigned int val,
                                     char marker,
                                     int *offset, unsigned char *buf)
{
    int oldoffset = *offset;
    if (ccnl_ndntlv_prependNonNegIntVal(val, offset, buf) < 0)
        return -1;

    if((*offset)-- < 1)
        return -1;
    buf[*offset] = marker;

    if (ccnl_ndntlv_prependTL(type, oldoffset - *offset, offset, buf) < 0)
        return -1;
    return oldoffset - *offset;

}


int
ccnl_ndntlv_prependBlob(int type, unsigned char *blob, int len,
                        int *offset, unsigned char *buf)
{
    int oldoffset = *offset;

    if (*offset < len)
        return -1;
    memcpy(buf + *offset - len, blob, len);
    *offset -= len;
    if (ccnl_ndntlv_prependTL(type, len, offset, buf) < 0)
        return -1;
    return oldoffset - *offset;
}

int
ccnl_ndntlv_prependName(struct ccnl_prefix_s *name,
                        int *offset, unsigned char *buf)
{
    int oldoffset = *offset, cnt;

    if(name->chunknum) {
        if (ccnl_ndntlv_prependIncludedNonNegInt(NDN_TLV_NameComponent,
                                                 *name->chunknum, 
                                                 NDN_Marker_SegmentNumber,
                                                 offset, buf) < 0)
            return -1;
    }

#ifdef USE_NFN
    if (name->nfnflags & CCNL_PREFIX_NFN) {
        if (ccnl_ndntlv_prependBlob(NDN_TLV_NameComponent,
                                (unsigned char*) "NFN", 3, offset, buf) < 0)
            return -1;
        if (name->nfnflags & CCNL_PREFIX_THUNK)
            if (ccnl_ndntlv_prependBlob(NDN_TLV_NameComponent,
                                (unsigned char*) "THUNK", 5, offset, buf) < 0)
                return -1;
    }
#endif
    for (cnt = name->compcnt - 1; cnt >= 0; cnt--) {
        if (ccnl_ndntlv_prependBlob(NDN_TLV_NameComponent, name->comp[cnt],
                                    name->complen[cnt], offset, buf) < 0)
            return -1;
    }
    if (ccnl_ndntlv_prependTL(NDN_TLV_Name, oldoffset - *offset,
                              offset, buf) < 0)
        return -1;

    return 0;
}

// ----------------------------------------------------------------------

int
ccnl_ndntlv_prependInterest(struct ccnl_prefix_s *name, int scope, int *nonce,
                            int *offset, unsigned char *buf)
{
    int oldoffset = *offset;
    unsigned char lifetime[2] = { 0x0f, 0xa0 };
    //unsigned char mustbefresh[2] = { NDN_TLV_MustBeFresh, 0x00 };

    if (scope >= 0) {
        if (scope > 2)
            return -1;
        if (ccnl_ndntlv_prependNonNegInt(NDN_TLV_Scope, scope, offset, buf) < 0)
            return -1;
    }

    if (ccnl_ndntlv_prependBlob(NDN_TLV_InterestLifetime, lifetime, 2,
                                offset, buf) < 0)
        return -1;

    if (nonce && ccnl_ndntlv_prependBlob(NDN_TLV_Nonce, (unsigned char*) nonce, 4,
                                offset, buf) < 0)
        return -1;

    /*if (ccnl_ndntlv_prependBlob(NDN_TLV_Selectors, mustbefresh, 2,
                                offset, buf) < 0)
        return -1;*/

    if (ccnl_ndntlv_prependName(name, offset, buf))
        return -1;
    if (ccnl_ndntlv_prependTL(NDN_TLV_Interest, oldoffset - *offset,
                              offset, buf) < 0)
        return -1;

    return oldoffset - *offset;
}

int
ccnl_ndntlv_prependContent(struct ccnl_prefix_s *name, 
                           unsigned char *payload, int paylen,  
                           int *offset, int *contentpos,
                           unsigned int *final_block_id,
                           unsigned char *buf)
{
    int oldoffset = *offset, oldoffset2;
    unsigned char signatureType[1] = { NDN_SigTypeVal_SignatureSha256WithRsa };

    if (contentpos)
        *contentpos = *offset - paylen;

    // fill in backwards

    // mandatory (empty for now)
    if (ccnl_ndntlv_prependTL(NDN_TLV_SignatureValue, 0, offset, buf) < 0)
        return -1;

    // to find length of SignatureInfo
    oldoffset2 = *offset;

    // optional (empty for now) because ndn client lib also puts it in by default
    if (ccnl_ndntlv_prependTL(NDN_TLV_KeyLocator, 0, offset, buf) < 0)
        return -1;

    // use NDN_SigTypeVal_SignatureSha256WithRsa because this is default in ndn client libs
    if (ccnl_ndntlv_prependBlob(NDN_TLV_SignatureType, signatureType, 1,
                offset, buf)< 0)
        return 1;

    // Groups KeyLocator and Signature Type with stored len
    if (ccnl_ndntlv_prependTL(NDN_TLV_SignatureInfo, oldoffset2 - *offset, offset, buf) < 0)
        return -1;

    // mandatory
    if (ccnl_ndntlv_prependBlob(NDN_TLV_Content, payload, paylen,
                                offset, buf) < 0)
        return -1;

    // to find length of optional MetaInfo fields
    oldoffset2 = *offset;
    if(final_block_id) {
        if (ccnl_ndntlv_prependIncludedNonNegInt(NDN_TLV_NameComponent,
                                                 *final_block_id, 
                                                 NDN_Marker_SegmentNumber,
                                                 offset, buf) < 0)
            return -1;

        // optional
        if (ccnl_ndntlv_prependTL(NDN_TLV_FinalBlockId, oldoffset2 - *offset, offset, buf) < 0)
            return -1;
    }

    // mandatory (empty for now)
    if (ccnl_ndntlv_prependTL(NDN_TLV_MetaInfo, oldoffset2 - *offset, offset, buf) < 0)
        return -1;

    // mandatory
    if (ccnl_ndntlv_prependName(name, offset, buf))
        return -1;

    // mandatory
    if (ccnl_ndntlv_prependTL(NDN_TLV_Data, oldoffset - *offset,
                              offset, buf) < 0)
           return -1;

    if (contentpos)
        *contentpos -= *offset;

    return oldoffset - *offset;
}

#endif // NEEDS_PACKET_CRAFTING

#endif // PKT_NDNTLV_C

// eof
