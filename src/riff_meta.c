#include "riff_meta.h"
#include "DANAUtility.h"
#include "DANAByteArray.h"
#include <stdlib.h>
#include <string.h>

static inline uint32_t read_syncsafe32(const uint8_t* p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) |
           ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7)  |
           ((uint32_t)(p[3] & 0x7F));
}

static inline void write_syncsafe32(uint8_t* p, uint32_t val) {
    p[0] = (uint8_t)((val >> 21) & 0x7F);
    p[1] = (uint8_t)((val >> 14) & 0x7F);
    p[2] = (uint8_t)((val >> 7) & 0x7F);
    p[3] = (uint8_t)(val & 0x7F);
}

static char* extract_id3_text(const uint8_t* data, uint32_t len) {
    if (len == 0) return NULL;
    uint8_t enc = data[0];
    const uint8_t* text = data + 1;
    uint32_t tlen = len - 1;

    if (enc == 0 || enc == 3) {
        // ISO-8859-1 or UTF-8
        while (tlen > 0 && text[tlen - 1] == 0) tlen--;
        return DANAUtility_StrNDup((const char*)text, tlen);
    } else if (enc == 1 || enc == 2) {
        // UTF-16 with or without BOM, convert ASCII subset, skip non-ASCII
        if (tlen < 2) return NULL;
        int be = (enc == 2) ? 1 : ((text[0] == 0xFE && text[1] == 0xFF) ? 1 : 0);
        uint32_t offset = (enc == 1) ? 2 : 0;
        uint32_t max_chars = (tlen - offset) / 2;
        char* out = malloc(max_chars + 1);
        if (!out) return NULL;
        uint32_t out_idx = 0;
        for (uint32_t i = offset; i + 1 < tlen; i += 2) {
            uint16_t ch = be ? (((uint16_t)text[i] << 8) | text[i + 1])
                             : (((uint16_t)text[i + 1] << 8) | text[i]);
            if (ch == 0) break;
            if (ch < 0x80) out[out_idx++] = (char)ch;
        }
        out[out_idx] = '\0';
        return out;
    }
    return NULL;
}

void RIFFMeta_ParseInfoChunk(const uint8_t* data, uint32_t size, struct DANAMetadata* meta) {
    if (!data || size < 4 || !meta) return;
    if (memcmp(data, "INFO", 4) != 0) return;

    uint32_t pos = 4;
    while (pos + 8 <= size) {
        char tag[5] = {0};
        memcpy(tag, data + pos, 4);
        uint32_t sub_size = (uint32_t)data[pos + 4] |
                            ((uint32_t)data[pos + 5] << 8) |
                            ((uint32_t)data[pos + 6] << 16) |
                            ((uint32_t)data[pos + 7] << 24);
        pos += 8;

        if (pos + sub_size > size) break;

        const char* val = (const char*)(data + pos);
        uint32_t str_len = strnlen(val, sub_size);

        if (strcmp(tag, "INAM") == 0 && !meta->title)  meta->title  = DANAUtility_StrNDup(val, str_len);
        if (strcmp(tag, "IART") == 0 && !meta->artist) meta->artist = DANAUtility_StrNDup(val, str_len);
        if (strcmp(tag, "IPRD") == 0 && !meta->album)  meta->album  = DANAUtility_StrNDup(val, str_len);
        if (strcmp(tag, "ICRD") == 0 && !meta->year)   meta->year   = DANAUtility_StrNDup(val, str_len);
        if (strcmp(tag, "IGNR") == 0 && !meta->genre)  meta->genre  = DANAUtility_StrNDup(val, str_len);
        if (strcmp(tag, "ITRK") == 0 && !meta->track)  meta->track  = DANAUtility_StrNDup(val, str_len);

        pos += (sub_size + 1) & ~1U;
    }
}

void RIFFMeta_ParseId3Chunk(const uint8_t* data, uint32_t size, struct DANAMetadata* meta) {
    if (!data || size < 10 || !meta) return;
    if (memcmp(data, "ID3", 3) != 0) return;

    uint8_t version = data[3];
    if (version != 3 && version != 4) return;

    uint32_t tag_size = read_syncsafe32(data + 6);
    if (tag_size + 10 > size) tag_size = size - 10;

    uint32_t pos = 10;
    uint32_t end = 10 + tag_size;

    while (pos + 10 <= end) {
        if (data[pos] == 0) break;

        char frame_id[5] = {0};
        memcpy(frame_id, data + pos, 4);

        uint32_t frame_size;
        if (version == 4) {
            frame_size = read_syncsafe32(data + pos + 4);
        } else {
            frame_size = ((uint32_t)data[pos + 4] << 24) |
                         ((uint32_t)data[pos + 5] << 16) |
                         ((uint32_t)data[pos + 6] << 8)  |
                         ((uint32_t)data[pos + 7]);
        }
        pos += 10;

        if (pos + frame_size > end) break;
        const uint8_t* fpayload = data + pos;

        if (strcmp(frame_id, "TIT2") == 0 && !meta->title) {
            meta->title = extract_id3_text(fpayload, frame_size);
        } else if (strcmp(frame_id, "TPE1") == 0 && !meta->artist) {
            meta->artist = extract_id3_text(fpayload, frame_size);
        } else if (strcmp(frame_id, "TALB") == 0 && !meta->album) {
            meta->album = extract_id3_text(fpayload, frame_size);
        } else if ((strcmp(frame_id, "TYER") == 0 || strcmp(frame_id, "TDRC") == 0) && !meta->year) {
            meta->year = extract_id3_text(fpayload, frame_size);
        } else if (strcmp(frame_id, "TCON") == 0 && !meta->genre) {
            meta->genre = extract_id3_text(fpayload, frame_size);
        } else if (strcmp(frame_id, "TRCK") == 0 && !meta->track) {
            meta->track = extract_id3_text(fpayload, frame_size);
        } else if (strcmp(frame_id, "TBPM") == 0 && !meta->bpm) {
            meta->bpm = extract_id3_text(fpayload, frame_size);
        } else if (strcmp(frame_id, "TKEY") == 0 && !meta->key) {
            meta->key = extract_id3_text(fpayload, frame_size);
        } else if (strcmp(frame_id, "USLT") == 0 && !meta->lyrics) {
            if (frame_size > 4) {
                uint32_t lpos = 4;
                while (lpos < frame_size && fpayload[lpos] != 0) lpos++;
                if (lpos < frame_size) lpos++;
                if (lpos < frame_size) {
                    meta->lyrics = DANAUtility_StrNDup((const char*)(fpayload + lpos), frame_size - lpos);
                }
            }
        } else if (strcmp(frame_id, "APIC") == 0 && !meta->cover_data) {
            if (frame_size > 4) {
                uint32_t cpos = 1;
                while (cpos < frame_size && fpayload[cpos] != 0) cpos++;
                if (cpos < frame_size) cpos++;
                if (cpos < frame_size) cpos++; // picture type
                while (cpos < frame_size && fpayload[cpos] != 0) cpos++;
                if (cpos < frame_size) cpos++;
                if (cpos < frame_size) {
                    meta->cover_size = frame_size - cpos;
                    meta->cover_data = malloc(meta->cover_size);
                    if (meta->cover_data) {
                        memcpy(meta->cover_data, fpayload + cpos, meta->cover_size);
                    }
                }
            }
        }

        pos += frame_size;
    }
}

uint8_t* RIFFMeta_BuildInfoChunk(const struct DANAMetadata* meta, uint32_t* out_size) {
    if (!meta || !out_size) return NULL;

    uint32_t alloc_sz = 1024;
    uint8_t* buf = malloc(alloc_sz);
    if (!buf) return NULL;

    memcpy(buf, "INFO", 4);
    uint32_t pos = 4;

#define APPEND_INFO_SUBCHUNK(tag, str) \
    if (str) { \
        uint32_t slen = (uint32_t)strlen(str) + 1; \
        uint32_t padded = (slen + 1) & ~1U; \
        if (pos + 8 + padded > alloc_sz) { \
            alloc_sz = (pos + 8 + padded) * 2; \
            buf = realloc(buf, alloc_sz); \
        } \
        memcpy(buf + pos, tag, 4); \
        buf[pos + 4] = (uint8_t)(slen & 0xFF); \
        buf[pos + 5] = (uint8_t)((slen >> 8) & 0xFF); \
        buf[pos + 6] = (uint8_t)((slen >> 16) & 0xFF); \
        buf[pos + 7] = (uint8_t)((slen >> 24) & 0xFF); \
        memcpy(buf + pos + 8, str, slen); \
        if (padded > slen) buf[pos + 8 + slen] = 0; \
        pos += 8 + padded; \
    }

    APPEND_INFO_SUBCHUNK("INAM", meta->title);
    APPEND_INFO_SUBCHUNK("IART", meta->artist);
    APPEND_INFO_SUBCHUNK("IPRD", meta->album);
    APPEND_INFO_SUBCHUNK("ICRD", meta->year);
    APPEND_INFO_SUBCHUNK("IGNR", meta->genre);
    APPEND_INFO_SUBCHUNK("ITRK", meta->track);
#undef APPEND_INFO_SUBCHUNK

    if (pos == 4) {
        free(buf);
        *out_size = 0;
        return NULL;
    }

    *out_size = pos;
    return buf;
}

uint8_t* RIFFMeta_BuildId3Chunk(const struct DANAMetadata* meta, uint32_t* out_size) {
    if (!meta || !out_size) return NULL;

    uint32_t alloc_sz = 4096 + (meta->cover_size > 0 ? meta->cover_size : 0);
    uint8_t* buf = malloc(alloc_sz);
    if (!buf) return NULL;

    memcpy(buf, "ID3\x03\x00\x00\x00\x00\x00\x00", 10);
    uint32_t pos = 10;

#define APPEND_TEXT_FRAME(fid, str) \
    if (str) { \
        uint32_t slen = (uint32_t)strlen(str); \
        uint32_t fsize = 1 + slen; \
        if (pos + 10 + fsize > alloc_sz) { \
            alloc_sz = (pos + 10 + fsize) * 2; \
            buf = realloc(buf, alloc_sz); \
        } \
        memcpy(buf + pos, fid, 4); \
        buf[pos + 4] = (uint8_t)((fsize >> 24) & 0xFF); \
        buf[pos + 5] = (uint8_t)((fsize >> 16) & 0xFF); \
        buf[pos + 6] = (uint8_t)((fsize >> 8)  & 0xFF); \
        buf[pos + 7] = (uint8_t)(fsize & 0xFF); \
        buf[pos + 8] = 0; buf[pos + 9] = 0; \
        buf[pos + 10] = 3; /* UTF-8 encoding */ \
        memcpy(buf + pos + 11, str, slen); \
        pos += 10 + fsize; \
    }

    APPEND_TEXT_FRAME("TIT2", meta->title);
    APPEND_TEXT_FRAME("TPE1", meta->artist);
    APPEND_TEXT_FRAME("TALB", meta->album);
    APPEND_TEXT_FRAME("TYER", meta->year);
    APPEND_TEXT_FRAME("TCON", meta->genre);
    APPEND_TEXT_FRAME("TRCK", meta->track);
    APPEND_TEXT_FRAME("TBPM", meta->bpm);
    APPEND_TEXT_FRAME("TKEY", meta->key);
#undef APPEND_TEXT_FRAME

    if (meta->lyrics) {
        uint32_t llen = (uint32_t)strlen(meta->lyrics);
        // enc(1) + lang(3) + desc(1) + text
        uint32_t fsize = 1 + 3 + 1 + llen;
        if (pos + 10 + fsize > alloc_sz) {
            alloc_sz = (pos + 10 + fsize) * 2;
            buf = realloc(buf, alloc_sz);
        }
        memcpy(buf + pos, "USLT", 4);
        buf[pos + 4] = (uint8_t)((fsize >> 24) & 0xFF);
        buf[pos + 5] = (uint8_t)((fsize >> 16) & 0xFF);
        buf[pos + 6] = (uint8_t)((fsize >> 8)  & 0xFF);
        buf[pos + 7] = (uint8_t)(fsize & 0xFF);
        buf[pos + 8] = 0; buf[pos + 9] = 0;
        buf[pos + 10] = 3;
        memcpy(buf + pos + 11, "eng", 3);
        buf[pos + 14] = 0;
        memcpy(buf + pos + 15, meta->lyrics, llen);
        pos += 10 + fsize;
    }

    if (meta->cover_data && meta->cover_size > 0) {
        const char* mime = "image/jpeg";
        if (meta->cover_size >= 8 && memcmp(meta->cover_data, "\x89PNG\r\n\x1a\n", 8) == 0) {
            mime = "image/png";
        }
        uint32_t mlen = (uint32_t)strlen(mime) + 1;
        // enc(1) + mime + pic_type(1) + desc(1) + img
        uint32_t fsize = 1 + mlen + 1 + 1 + meta->cover_size;
        if (pos + 10 + fsize > alloc_sz) {
            alloc_sz = pos + 10 + fsize + 1024;
            buf = realloc(buf, alloc_sz);
        }
        memcpy(buf + pos, "APIC", 4);
        buf[pos + 4] = (uint8_t)((fsize >> 24) & 0xFF);
        buf[pos + 5] = (uint8_t)((fsize >> 16) & 0xFF);
        buf[pos + 6] = (uint8_t)((fsize >> 8)  & 0xFF);
        buf[pos + 7] = (uint8_t)(fsize & 0xFF);
        buf[pos + 8] = 0; buf[pos + 9] = 0;
        buf[pos + 10] = 0; // ISO-8859-1
        memcpy(buf + pos + 11, mime, mlen);
        buf[pos + 11 + mlen] = 3; // Front cover
        buf[pos + 12 + mlen] = 0; // Description terminator
        memcpy(buf + pos + 13 + mlen, meta->cover_data, meta->cover_size);
        pos += 10 + fsize;
    }

    if (pos == 10) {
        free(buf);
        *out_size = 0;
        return NULL;
    }

    write_syncsafe32(buf + 6, pos - 10);
    *out_size = pos;
    return buf;
}