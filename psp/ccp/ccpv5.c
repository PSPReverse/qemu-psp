/*
 * AMD PSP emulation
 *
 * Copyright (C) 2020 Robert Buhren <robert@robertbuhren.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"

#include <byteswap.h>
#include <zlib.h>

#include "psp/ccp/ccpv5.h"

#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "system/address-spaces.h"
#include "trace.h"

#include "psp/ccp/ccpv5-linux.h"
#include "psp/ccp/ccpv5-nettle.h"
#include "psp/psp-todo.h"
/* TODO: Restrict access to specific MemoryRegions only.
 *       Verify correct use of ccp_map/cpu_physical_memory_unmap */

/*
 * Attributes of the CCP's own memory accesses. They are specified
 * (unspecified == 0), so a device can tell CCP DMA from a write injected from
 * outside the guest: the mailbox doorbell fires only for the latter. The
 * requester id has no hardware meaning; it marks CCP DMA in traces.
 */
#define CCP_REQUESTER_ID 0xcc
static const MemTxAttrs ccp_attrs = {.requester_id = CCP_REQUESTER_ID};

/*
 * cpu_physical_memory_map with ccp_attrs. A bounce buffer (an MMIO target)
 * writes back with the same attributes on cpu_physical_memory_unmap.
 */
static void* ccp_map(hwaddr addr, hwaddr* plen, bool is_write)
{
    return address_space_map(&address_space_memory, addr, plen, is_write,
                             ccp_attrs);
}

/* Upper bound on descriptors processed per queue run (guest controls head) */
#define CCP_Q_MAX_DESC_PER_RUN 4096

/* Upper bound on one AES request: the host staging buffers are sized by it */
#define CCP_AES_MAX_LEN (16 * 1024 * 1024)

/* TODO Document*/
static void ccp_process_q(CcpV5State* s, uint32_t id);

// static void ccp_timer_cb(void *opaque)
//{
//     CcpV5State *s = CCP_V5(opaque);
//
//     if (s->dma_timer_qid != -1) {
//         ccp_process_q(s, s->dma_timer_qid);
//         s->dma_timer_qid = -1;
//     } else {
//         qemu_log_mask(LOG_GUEST_ERROR, "CCP Error: Timer fired, but no queue
//         ID set\n");
//
//     }
// }

static void ccp_reverse_buf(uint8_t* buf, size_t len)
{
    uint8_t tmp;
    uint8_t* buf_top = buf + len - 1;

    while (buf < buf_top) {
        tmp = *buf;
        *buf = *buf_top;
        *buf_top = tmp;
        buf++;
        buf_top--;
    }
}

static uint32_t ccp_queue_read(CcpV5State* s, hwaddr offset, uint32_t id)
{
    uint32_t ret;
    CcpV5QState* qs;

    qs = &s->q_states[id];
    switch (offset) {
    case CCP_Q_CTRL_OFFSET:
        if (qs->ccp_q_control & CCP_Q_RUN) {
            ccp_process_q(s, id);
        }
        ret = qs->ccp_q_control;
        trace_ccp_queue_ctrl_read(id, ret);
        break;
    case CCP_Q_TAIL_LO_OFFSET:
        ret = qs->ccp_q_tail;
        trace_ccp_queue_tail_lo_read(id, ret);
        break;
    case CCP_Q_HEAD_LO_OFFSET:
        ret = qs->ccp_q_head;
        trace_ccp_queue_head_lo_read(id, ret);
        break;
    case CCP_Q_STATUS_OFFSET:
        ret = qs->ccp_q_status;
        trace_ccp_queue_status_read(id, ret);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "CCP Error: CCP queue read at unknown "
                      "offset: 0x%" HWADDR_PRIx "\n",
                      offset);
        ret = 0;
    }
    return ret;
}

/** memcpy with ccp bitwise and byteswap ops */
static void ccp_memcpy(void* dst, void* src, size_t len, ccp_pt_bitwise bwise,
                       ccp_pt_byteswap bswap)
{

    if (bwise != CCP_PASSTHRU_BITWISE_NOOP) {
        PSP_TODO("ccp.passthru.bitwise", "copy skipped, reported as successful",
                 "CCP Error: unimplemented bitwise op: %d", bwise);
        return;
    }

    if (bwise == CCP_PASSTHRU_BITWISE_NOOP &&
        bswap == CCP_PASSTHRU_BYTESWAP_NOOP) {
        /* Normal memcpy */
        memcpy(dst, src, len);
        return;
    }

    if (bswap == CCP_PASSTHRU_BYTESWAP_256BIT) {
        /* For now, we only support swap-size aligned ops */
        if (len % 0x20) {
            PSP_TODO("ccp.passthru.unaligned_bswap",
                     "copy skipped, reported as successful",
                     "CCP Error: unaligned bswap op of %zu bytes", len);
            return;
        }
        /* TODO clean up */
        memcpy(dst, src, len);
        ccp_reverse_buf(dst, len);
    }
}

/*
 * True if [addr, addr + len) lies within the LSB. The guest controls both
 * values, so the check is written to not wrap around for a large addr.
 */
static bool ccp_lsb_range_ok(CcpV5State *s, hwaddr addr, hwaddr len)
{
    return addr <= sizeof(s->lsb.u.lsb) && len <= sizeof(s->lsb.u.lsb) - addr;
}

static void* get_raw_ptr(CcpV5State* s, hwaddr addr, hwaddr* len,
                         ccp_memtype src_type, bool write)
{
    void* ptr = NULL;

    if (src_type == CCP_MEMTYPE_LOCAL) {
        ptr = ccp_map(addr, len, write);
        if (ptr == NULL) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "CCP Error: Couldn't map guest memory during"
                          " passthrough operation\n");
            return ptr;
        }
    } else if (src_type == CCP_MEMTYPE_SB) {
        if (!ccp_lsb_range_ok(s, addr, *len)) {
            /* The access exceeds the LSB size: Cap the len at LSB max. TODO */
            PSP_TODO("ccp.passthru.lsb_range",
                     "copy stopped, reported as successful",
                     "CCP Error: access exceeds LSB size");
            return NULL;
        }
        ptr = s->lsb.u.lsb + addr;
    } else {
        PSP_TODO("ccp.memtype.invalid", "copy stopped, reported as successful",
                 "CCP Error: invalid memtype %d", src_type);
    }
    return ptr;
}
static void perform_ccp_passthrough(CcpV5State* s, uint32_t id, hwaddr dst,
                                    hwaddr src, uint32_t len,
                                    ccp_memtype dst_type, ccp_memtype src_type,
                                    ccp_pt_bitwise bwise, ccp_pt_byteswap bswap)
{
    /* TODO: Ensure that we don't access un-accessible memory regions *
     *       Test whether dst - dst + len is within SRAM.
     *       Verify that we don't overflow any host memory.
     *       Cleanup mappings on early return.
     */

    if (bwise != CCP_PASSTHRU_BITWISE_NOOP ||
        (bswap != CCP_PASSTHRU_BYTESWAP_NOOP &&
         bswap != CCP_PASSTHRU_BYTESWAP_256BIT)) {

        PSP_TODO("ccp.passthru.bitops",
                 "descriptor skipped, reported as successful",
                 "CCP Error: unimplemented passthrough bit ops: bitwise 0x%x "
                 "byteswap 0x%x",
                 bwise, bswap);
        return;
    }

    if (dst_type == CCP_MEMTYPE_SYSTEM || src_type == CCP_MEMTYPE_SYSTEM) {
        PSP_TODO("ccp.passthru.system_memtype",
                 "descriptor skipped, reported as successful",
                 "CCP Error: unimplemented memtype CCP_MEMTYPE_SYSTEM");
        return;
    }
    do {
        hwaddr dest_len = len;
        void* hdst = get_raw_ptr(s, dst, &dest_len, dst_type, true);
        if (hdst == NULL) {
            return;
        }
        hwaddr src_len = len;
        void* hsrc = get_raw_ptr(s, src, &src_len, src_type, false);
        if (hsrc == NULL) {
            if (dst_type == CCP_MEMTYPE_LOCAL)
                cpu_physical_memory_unmap(hdst, dest_len, true, 0);
            return;
        }
        hwaddr min_len = MIN(MIN(dest_len, src_len), len);
        ccp_memcpy(hdst, hsrc, min_len, bwise, bswap);

        if (dst_type == CCP_MEMTYPE_LOCAL)
            cpu_physical_memory_unmap(hdst, dest_len, true, min_len);
        if (src_type == CCP_MEMTYPE_LOCAL)
            cpu_physical_memory_unmap(hsrc, src_len, false, min_len);
        dst += min_len;
        src += min_len;
        len -= min_len;
    } while (len > 0);
}
char const* ccp_memtype_str(ccp_memtype type);
char const* ccp_memtype_str(ccp_memtype type)
{
    switch (type) {
    case CCP_MEMTYPE_SYSTEM:
        return "CCP_MEMTYPE_SYSTEM";
    case CCP_MEMTYPE_SB:
        return "CCP_MEMTYPE_SB";
    case CCP_MEMTYPE_LOCAL:
        return "CCP_MEMTYPE_LOCAL";
    default:
        return "CCP_MEMTYPE_UNKNOWN";
    }
}
static void decode_ccp_passthrough(CcpV5State* s, uint32_t id, ccp5_desc* desc)
{
    ccp_function func;
    ccp_memtype src_type;
    ccp_memtype dst_type;
    hwaddr src;
    hwaddr dst;
    uint32_t cbytes;
    ccp_pt_bitwise bwise;
    ccp_pt_byteswap bswap;

    func.raw = CCP5_CMD_FUNCTION(desc);
    src_type = CCP5_CMD_SRC_MEM(desc);
    dst_type = CCP5_CMD_DST_MEM(desc);
    cbytes = CCP5_CMD_LEN(desc);
    /* TODO: Does the shift maybe cause issues? */
    src = CCP5_CMD_SRC_LO(desc) | ((hwaddr)(CCP5_CMD_SRC_HI(desc)) << 32);
    dst = CCP5_CMD_DST_LO(desc) | ((hwaddr)(CCP5_CMD_DST_HI(desc)) << 32);
    bwise = func.pt.bitwise;
    bswap = func.pt.byteswap;
    char const* const src_type_name = ccp_memtype_str(src_type);
    char const* const dst_type_name = ccp_memtype_str(dst_type);
    trace_ccp_passthrough(src_type_name, src, dst_type_name, dst, cbytes);
    perform_ccp_passthrough(s, id, dst, src, cbytes, dst_type, src_type, bwise,
                            bswap);
}

static void ccp_perform_sha_256(CcpV5State* s, hwaddr src, uint32_t len,
                                bool eom, bool init, uint8_t* lsb_ctx)
{
    /* TODO: use nettle. See:
     * https://www.gnutls.org/manual/html_node/Using-GnuTLS-as-a-cryptographic-library.html#Using-GnuTLS-as-a-cryptographic-library
     */
    /* Use "init" to determine whether we need to initialize a new sha context
     */
    void* hsrc;
    hwaddr plen;
    uint32_t bytes_read = 0;

    /*
     * The context is shared by both SHA types: restart it on init, or when
     * the live context belongs to the other type.
     */
    if (init || s->sha_ctx.raw == NULL || s->sha_ctx_type != CCP_SHA_TYPE_256) {
        ccp_clear_sha_ctx(&s->sha_ctx);
        ccp_init_sha256_ctx(&s->sha_ctx);
        s->sha_ctx_type = CCP_SHA_TYPE_256;
    }
    if (s->sha_ctx.raw != NULL) {
        while (bytes_read < len) {
            plen = len - bytes_read;
            hsrc = ccp_map(src + bytes_read, &plen, false);
            // qemu_log_mask(LOG_GUEST_ERROR, "CCP SHA256: Mapped %#lx bytes\n",
            // plen); qemu_log_mask(LOG_GUEST_ERROR, "CCP SHA256: Mapped from
            // %#lx to %#lx\n", src + bytes_read, src + bytes_read + plen);
            ccp_update_sha256(&s->sha_ctx, plen, hsrc);
            cpu_physical_memory_unmap(hsrc, plen, false, plen);
            bytes_read += plen;
        }

        // qemu_log_mask(LOG_GUEST_ERROR, "CCP SHA256: Digested %#x bytes\n",
        // bytes_read);

        if (eom) {
            ccp_digest_sha256(&s->sha_ctx, lsb_ctx);
            /* The CCP seems to store the digest in reversed order -> Do as the
             * CCP would do, even if it means we reverse it again when the
             * digest is copied from the lsb to the PSP.
             */
            ccp_reverse_buf(lsb_ctx, 32); // 32 -> SHA256 digest size
        }
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "CCP Error SHA256: SHA context not initialized\n");
        return;
    }
}

static void ccp_perform_sha_384(CcpV5State* s, hwaddr src, uint32_t len,
                                bool eom, bool init, uint8_t* lsb_ctx)
{
    /* TODO: use nettle. See:
     * https://www.gnutls.org/manual/html_node/Using-GnuTLS-as-a-cryptographic-library.html#Using-GnuTLS-as-a-cryptographic-library
     */
    /* Use "init" to determine whether we need to initialize a new sha context
     */
    void* hsrc;
    hwaddr plen;
    uint32_t bytes_read = 0;

    /*
     * The context is shared by both SHA types: restart it on init, or when
     * the live context belongs to the other type.
     */
    if (init || s->sha_ctx.raw == NULL || s->sha_ctx_type != CCP_SHA_TYPE_384) {
        ccp_clear_sha_ctx(&s->sha_ctx);
        ccp_init_sha384_ctx(&s->sha_ctx);
        s->sha_ctx_type = CCP_SHA_TYPE_384;
    }
    if (s->sha_ctx.raw != NULL) {
        while (bytes_read < len) {
            plen = len - bytes_read;
            hsrc = ccp_map(src + bytes_read, &plen, false);
            // qemu_log_mask(LOG_GUEST_ERROR, "CCP SHA384: Mapped %#lx bytes\n",
            // plen); qemu_log_mask(LOG_GUEST_ERROR, "CCP SHA384: Mapped from
            // %#lx to %#lx\n", src + bytes_read, src + bytes_read + plen);
            ccp_update_sha384(&s->sha_ctx, plen, hsrc);
            cpu_physical_memory_unmap(hsrc, plen, false, plen);
            bytes_read += plen;
        }

        // qemu_log_mask(LOG_GUEST_ERROR, "CCP SHA384: Digested %#x bytes\n",
        // bytes_read);

        if (eom) {
            ccp_digest_sha384(&s->sha_ctx, lsb_ctx);
            /* The CCP seems to store the digest in reversed order -> Do as the
             * CCP would do, even if it means we reverse it again when the
             * digest is copied from the lsb to the PSP.
             */
            ccp_reverse_buf(lsb_ctx, 48); // 48 -> SHA384 digest size
        }

    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "CCP Error SHA384: SHA context not initialized\n");
        return;
    }
}

static void ccp_zlib(CcpV5State* s, uint32_t id, ccp5_desc* desc)
{
    bool init;
    bool eom;
    ccp_memtype src_type;
    ccp_memtype dst_type;
    hwaddr src;
    hwaddr dst;
    uint32_t len;
    void* hsrc;
    void* hdst;
    hwaddr plen_in;
    hwaddr plen_out;

    src_type = CCP5_CMD_SRC_MEM(desc);
    dst_type = CCP5_CMD_DST_MEM(desc);
    len = CCP5_CMD_LEN(desc);
    plen_in = len;
    /* TODO: Does the shift maybe cause issues? */
    src = CCP5_CMD_SRC_LO(desc) | ((hwaddr)(CCP5_CMD_SRC_HI(desc)) << 32);
    dst = CCP5_CMD_DST_LO(desc) | ((hwaddr)(CCP5_CMD_DST_HI(desc)) << 32);
    init = CCP5_CMD_INIT(desc);
    eom = CCP5_CMD_EOM(desc);

    trace_ccp_zlib(src, dst, len, src_type, dst_type, init, eom);

    if (!init || !eom) {
        PSP_TODO("ccp.zlib.flags",
                 "descriptor skipped, reported as successful",
                 "CCP Error ZLIB: only INIT=1 and EOM=1 are supported (init "
                 "%d, eom %d)",
                 init, eom);
        return;
    }

    if (init) {
        s->zlib_bytes_written = 0;
        if (ccp_zlib_init(&s->zlib_state)) {
            PSP_TODO("ccp.zlib.init",
                     "descriptor skipped, reported as successful",
                     "CCP ZLIB Error: couldn't initialize zlib state");
            return;
        }
    }
    // TODO: Iteravily map the input buffer and decompress page wise
    /* Mapping input buffer */
    hsrc = ccp_map(src, &plen_in, false);
    if (plen_in != len) {
        PSP_TODO("ccp.zlib.input_map",
                 "descriptor skipped, reported as successful",
                 "CCP Error ZLIB: couldn't map guest input memory");
        if (hsrc) {
            cpu_physical_memory_unmap(hsrc, plen_in, false, 0);
        }
        /* The stream was initialised above; end it or its state leaks */
        ccp_zlib_end(&s->zlib_state);
        return;
    }
    /* Mapping output buffer */
    /* TODO: Try to decompress page wise until we can't map the hosts memory
     * anymore */
    plen_out = CCP_ZLIB_CHUNK_SIZE;
    hdst = ccp_map(dst, &plen_out, true);

    // qemu_log_mask(LOG_GUEST_ERROR, "CCP ZLIB: mapped %lx bytes\n", plen_out);
    // qemu_log_mask(LOG_GUEST_ERROR, "CCP ZLIB: mapped from %p to %p\n",
    // (void*)dst, (void*)dst+plen_out);

    ccp_zlib_status status =
        ccp_zlib_inflate(&s->zlib_state, plen_in, plen_out, hdst, hsrc);
    if (plen_out)
        cpu_physical_memory_unmap(hdst, plen_out, true, plen_out);

    while (status != CCP_ZLIB_SUCCESS) {
        if (status == CCP_ZLIB_ERROR) {
            PSP_TODO("ccp.zlib.inflate",
                     "output partly written, reported as successful",
                     "CCP ERROR ZLIB: inflation failed after %#lx bytes",
                     s->zlib_state.total_out);
            break;
        }
        if (status == CCP_ZLIB_MORE_DATA_PENDING) {
            plen_out = CCP_ZLIB_CHUNK_SIZE;
            hdst = ccp_map(dst + s->zlib_state.total_out, &plen_out, true);
            // qemu_log_mask(LOG_GUEST_ERROR, "CCP ZLIB: mapped %lx bytes\n",
            // plen_out); qemu_log_mask(LOG_GUEST_ERROR, "CCP ZLIB: mapped from
            // %p to %p\n", (void*)dst+s->zlib_state.total_out,
            // (void*)dst+s->zlib_state.total_out+plen_out);
            status = ccp_zlib_continue_inflate(&s->zlib_state, plen_out, hdst);
            if (plen_out)
                cpu_physical_memory_unmap(hdst, plen_out, true, plen_out);
            continue;
        }
    }

    if (hsrc) {
        cpu_physical_memory_unmap(hsrc, plen_in, false, plen_in);
    }

    s->zlib_bytes_written += s->zlib_state.total_out;

    // qemu_log_mask(LOG_GUEST_ERROR, "CCP ZLIB: Inflation ended after %#lx
    // bytes. (%#x total)\n", s->zlib_state.total_out, s->zlib_bytes_written);

    if (eom) {
        ccp_zlib_end(&s->zlib_state);
    }
}

/*
 * @return false if the access could not be completed (the guest range is
 * unmapped or only partly mapped, or it exceeds the LSB); the caller fails the
 * descriptor.
 */
static bool ccp_copy_to_host(CcpV5State *s, void *hdst, hwaddr src,
                             ccp_memtype stype, size_t len)
{
    void *hsrc = NULL;
    hwaddr plen = len;

    if (stype == CCP_MEMTYPE_LOCAL) {
        hsrc = ccp_map(src, &plen, false);
        if (!hsrc) {
            return false;
        }
        if (plen != len) {
            cpu_physical_memory_unmap(hsrc, plen, false, 0);
            return false;
        }
        memcpy(hdst, hsrc, len);
        cpu_physical_memory_unmap(hsrc, len, false, plen);
    } else if (stype == CCP_MEMTYPE_SB) {
        if (!ccp_lsb_range_ok(s, src, len)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "CCP Error: Access exceeds LSB size\n");
            return false;
        }
        hsrc = s->lsb.u.lsb + src;
        memcpy(hdst, hsrc, len);
    } else {
        PSP_TODO("ccp.copy.src_memtype",
                 "bytes not copied, reported as successful",
                 "CCP Error: unimplemented source memtype %d", stype);
    }
    return true;
}

/* @return false if the access could not be completed, see ccp_copy_to_host */
static bool ccp_copy_to_guest(CcpV5State *s, hwaddr dst, void *hsrc,
                              ccp_memtype dtype, size_t len)
{
    void *hdst = NULL;
    hwaddr plen = len;

    if (dtype == CCP_MEMTYPE_LOCAL) {
        hdst = ccp_map(dst, &plen, true);
        if (!hdst) {
            return false;
        }
        if (plen != len) {
            cpu_physical_memory_unmap(hdst, plen, true, 0);
            return false;
        }
        memcpy(hdst, hsrc, len);
        cpu_physical_memory_unmap(hdst, len, true, len);
    } else if (dtype == CCP_MEMTYPE_SB) {
        if (!ccp_lsb_range_ok(s, dst, len)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "CCP Error: Access exceeds LSB size\n");
            return false;
        }
        hdst = s->lsb.u.lsb + dst;
        memcpy(hdst, hsrc, len);
    } else {
        PSP_TODO("ccp.copy.dst_memtype",
                 "bytes not copied, reported as successful",
                 "CCP Error: unimplemented destination memtype %d", dtype);
    }
    return true;
}

static void ccp_aes_log_func(ccp_function func)
{
    const char* mode;
    switch (func.aes.mode) {
    case CCP_AES_MODE_ECB:
        mode = "ecb";
        break;
    case CCP_AES_MODE_CBC:
        mode = "cbc";
        break;
    case CCP_AES_MODE_OFB:
        mode = "ofb";
        break;
    case CCP_AES_MODE_CFB:
        mode = "cfb";
        break;
    case CCP_AES_MODE_CTR:
        mode = "ctr";
        break;
    case CCP_AES_MODE_CMAC:
        mode = "cmac";
        break;
    case CCP_AES_MODE_GHASH:
        mode = "ghash";
        break;
    case CCP_AES_MODE_GCTR:
        mode = "gctr";
        break;
    case CCP_AES_MODE_GCM:
        mode = "gcm";
        break;
    case CCP_AES_MODE_GMAC:
        mode = "gmac";
        break;
    default:
        mode = "unknown";
    }
    const char* type;
    switch (func.aes.type) {
    case CCP_AES_TYPE_128:
        type = "aes128";
        break;
    case CCP_AES_TYPE_192:
        type = "aes193";
        break;
    case CCP_AES_TYPE_256:
        type = "aes256";
        break;
    default:
        type = "unknown";
    }
    qemu_log_mask(LOG_TRACE,
                  "CCP AES Request: engine=AES mode=%s type=%s encrypt=%s\n",
                  mode, type, func.aes.encrypt ? "true" : "false");
}

/* @return true if the descriptor failed and the queue must report an error */
static bool ccp_aes(CcpV5State *s, uint32_t id, ccp5_desc *desc)
{
    ccp_function func;
    // bool init;
    // bool eom;
    uint8_t key_local[32] = {0};
    ccp_memtype key_mtype, src_mtype, dst_mtype;
    hwaddr key_addr, src_addr, dst_addr;
    uint32_t len;

    func.raw = CCP5_CMD_FUNCTION(desc);
    // init = CCP5_CMD_INIT(desc);
    // eom = CCP5_CMD_EOM(desc);
    len = CCP5_CMD_LEN(desc);

    if (len == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "CCP Error AES: zero-length request\n");
        return true;
    }
    if (len > CCP_AES_MAX_LEN) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "CCP Error AES: request length %u exceeds %u\n", len,
                      CCP_AES_MAX_LEN);
        return true;
    }
    /* nettle asserts on partial blocks in ECB mode */
    if (func.aes.mode == CCP_AES_MODE_ECB && len % 16) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "CCP Error AES: ECB length %u is not a multiple of 16\n",
                      len);
        return true;
    }

    /* A SYSTEM source is not copied, so keep the staged input deterministic */
    uint8_t *src_buf = g_try_malloc0(len);
    uint8_t *dst_buf = g_try_malloc(len);
    bool failed = false;

    if (!src_buf || !dst_buf) {
        qemu_log_mask(LOG_GUEST_ERROR, "CCP Error AES: out of host memory\n");
        g_free(src_buf);
        g_free(dst_buf);
        return true;
    }

    ccp_aes_log_func(func);

    key_mtype = CCP5_CMD_KEY_MEM(desc);
    src_mtype = CCP5_CMD_SRC_MEM(desc);
    dst_mtype = CCP5_CMD_DST_MEM(desc);
    key_addr = CCP5_CMD_KEY_LO(desc) | ((hwaddr)(CCP5_CMD_KEY_HI(desc)) << 32);
    src_addr = CCP5_CMD_SRC_LO(desc) | ((hwaddr)(CCP5_CMD_SRC_HI(desc)) << 32);
    dst_addr = CCP5_CMD_DST_LO(desc) | ((hwaddr)(CCP5_CMD_DST_HI(desc)) << 32);
    trace_ccp_aes(func.aes.mode, func.aes.type, func.aes.encrypt, src_addr,
                  src_mtype, dst_addr, dst_mtype, len);

    switch (func.aes.mode) {

    case CCP_AES_MODE_ECB:
        switch (func.aes.type) {
        case CCP_AES_TYPE_128:
            if (!ccp_copy_to_host(s, key_local, key_addr, key_mtype, 16) ||
                !ccp_copy_to_host(s, src_buf, src_addr, src_mtype, len)) {
                failed = true;
                break;
            }

            ccp_aes128ecb(dst_buf, src_buf, len, key_local, func.aes.encrypt);

            failed = !ccp_copy_to_guest(s, dst_addr, dst_buf, dst_mtype, len);
            break;
        case CCP_AES_TYPE_256:
            if (!ccp_copy_to_host(s, key_local, key_addr, key_mtype, 32) ||
                !ccp_copy_to_host(s, src_buf, src_addr, src_mtype, len)) {
                failed = true;
                break;
            }

            ccp_aes256ecb(dst_buf, src_buf, len, key_local, func.aes.encrypt);

            failed = !ccp_copy_to_guest(s, dst_addr, dst_buf, dst_mtype, len);
            break;
        default:
            PSP_TODO("ccp.aes.type", "dst not written, reported as successful",
                     "CCP Error: unimplemented AES TYPE %d", func.aes.type);
        }
        break;
    default:
        PSP_TODO("ccp.aes.mode", "dst not written, reported as successful",
                 "CCP Error: unimplemented AES MODE %d", func.aes.mode);
    }

    g_free(src_buf);
    g_free(dst_buf);
    return failed;
}

static void ccp_sha(CcpV5State* s, uint32_t id, ccp5_desc* desc)
{
    ccp_function func;
    bool init;
    bool eom;
    hwaddr src;
    uint32_t len;
    uint64_t sha_len;
    uint32_t ctx_id;
    uint8_t* ctx;

    func.raw = CCP5_CMD_FUNCTION(desc);
    init = CCP5_CMD_INIT(desc);
    eom = CCP5_CMD_EOM(desc);
    len = CCP5_CMD_LEN(desc);
    src = CCP5_CMD_SRC_LO(desc) | ((hwaddr)(CCP5_CMD_SRC_HI(desc)) << 32);
    sha_len = CCP5_CMD_SHA_LO(desc) | ((uint64_t)(CCP5_CMD_SHA_HI(desc)) << 32);
    ctx_id = CCP5_CMD_LSB_ID(desc);
    if (ctx_id >= CCP_LSB_COUNT) {
        PSP_TODO("ccp.sha.lsb_id", "descriptor skipped, reported as successful",
                 "CCP Error SHA: LSB context id %u out of range", ctx_id);
        return;
    }

    ctx = s->lsb.u.slots[ctx_id].data;

    switch (func.sha.type) {
    case CCP_SHA_TYPE_256:
        trace_ccp_sha(src, len, sha_len, init, eom, ctx_id);
        ccp_perform_sha_256(s, src, len, eom, init, ctx);

        break;
    case CCP_SHA_TYPE_384:
        /* The 48-byte digest is written from the context slot onwards. */
        if (ctx_id * CCP_LSB_SIZE + 48 > sizeof(s->lsb.u.lsb)) {
            PSP_TODO("ccp.sha384.lsb_range",
                     "descriptor skipped, reported as successful",
                     "CCP Error SHA384: LSB context id %u too close to the end "
                     "of the LSB",
                     ctx_id);
            break;
        }
        trace_ccp_sha(src, len, sha_len, init, eom, ctx_id);
        ccp_perform_sha_384(s, src, len, eom, init, ctx);

        break;
    default:
        PSP_TODO("ccp.sha.type", "descriptor skipped, reported as successful",
                 "CCP Error: unimplemented SHA type %d", func.sha.type);
        break;
    }
}

/* @return true if the descriptor failed, see ccp_aes */
static bool ccp_rsa(CcpV5State *s, uint32_t id, ccp5_desc *desc)
{
    uint16_t rsa_size;
    uint16_t rsa_mode;
    ccp_function func;
    bool init;
    bool eom;
    uint32_t len;
    ccp_memtype rsa_key_mtype;
    ccp_memtype rsa_src_mtype;
    ccp_memtype rsa_dst_mtype;
    hwaddr rsa_key_addr;
    hwaddr rsa_src;
    hwaddr rsa_dst;
    CcpV5RsaPubKey rsa_pubkey;

    /*
     * Only 2048 & 4096 for now. The inputs are zeroed because a SYSTEM source
     * is not copied, and the result because mpz_export() writes nothing when
     * the result is 0.
     */
    uint8_t rsa_exp[512] = {0};
    uint8_t rsa_mod[512] = {0};
    /* Assuming it is a signature with length equal to the modulus */
    uint8_t rsa_msg[512] = {0};
    uint8_t rsa_result[512] = {0};

    func.raw = CCP5_CMD_FUNCTION(desc);
    init = CCP5_CMD_INIT(desc);
    eom = CCP5_CMD_EOM(desc);
    len = CCP5_CMD_LEN(desc);
    rsa_size = func.rsa.size;
    rsa_mode = func.rsa.mode;
    rsa_key_mtype = CCP5_CMD_KEY_MEM(desc);
    rsa_src_mtype = CCP5_CMD_SRC_MEM(desc);
    rsa_dst_mtype = CCP5_CMD_DST_MEM(desc);

    rsa_key_addr =
        CCP5_CMD_KEY_LO(desc) | ((hwaddr)(CCP5_CMD_KEY_HI(desc)) << 32);
    rsa_src = CCP5_CMD_SRC_LO(desc) | ((hwaddr)(CCP5_CMD_SRC_HI(desc)) << 32);
    rsa_dst = CCP5_CMD_DST_LO(desc) | ((hwaddr)(CCP5_CMD_DST_HI(desc)) << 32);

    trace_ccp_rsa(desc->src_lo, len, rsa_dst, rsa_dst_mtype, rsa_mode, rsa_size,
                  eom, init);

    if (rsa_mode == 0 &&
        ((rsa_size == 256 && len == 512) || (rsa_size == 512 && len == 1024))) {

        /* TODO consistent naming */
        rsa_pubkey.rsa_len = rsa_size;

        /* TODO: Use guest memory ?*/

        /* Copy RSA exponent */
        if (!ccp_copy_to_host(s, rsa_exp, rsa_key_addr, rsa_key_mtype,
                              rsa_size)) {
            return true;
        }

        /* Copy RSA modulus
         * "rsa_src" contains the modulus, follwed by the message
         */
        if (!ccp_copy_to_host(s, rsa_mod, rsa_src, rsa_src_mtype, rsa_size)) {
            return true;
        }

        /* Copy RSA message (signature) */
        if (!ccp_copy_to_host(s, rsa_msg, rsa_src + rsa_size, rsa_src_mtype,
                              rsa_size)) {
            return true;
        }

        ccp_rsa_init_key(&rsa_pubkey, rsa_mod, rsa_exp);

        ccp_rsa_encrypt(&rsa_pubkey, rsa_msg, rsa_result);

        ccp_rsa_clear_key(&rsa_pubkey);

        return !ccp_copy_to_guest(s, rsa_dst, rsa_result, rsa_dst_mtype,
                                  rsa_size);
    }
    PSP_TODO("ccp.rsa.mode_size", "not executed, reported as successful",
             "CCP Error: unimplemented RSA mode %u size %u len %u", rsa_mode,
             rsa_size, len);
    return false;
}

/*
 * @return true if the request used an unimplemented engine (XTS-AES, DES3 or
 * ECC) and was not executed. Unknown engines are reported with PSP_TODO and
 * return false, so the queue reports them as successful.
 */
static bool ccp_execute(CcpV5State* s, uint32_t id, ccp5_desc* desc)
{

    ccp_engine engine = CCP5_CMD_ENGINE(desc);

    switch (engine) {
    case CCP_ENGINE_AES:
        return ccp_aes(s, id, desc);
    case CCP_ENGINE_XTS_AES_128:
        PSP_TODO("ccp.engine.xts", "descriptor fails, status ERROR",
                 "CCP Error: unimplemented engine (AES-XTS-128)");
        return true;
    case CCP_ENGINE_DES3:
        PSP_TODO("ccp.engine.des3", "descriptor fails, status ERROR",
                 "CCP Error: unimplemented engine (DES3)");
        return true;
    case CCP_ENGINE_SHA:
        ccp_sha(s, id, desc);
        break;
    case CCP_ENGINE_RSA:
        return ccp_rsa(s, id, desc);
    case CCP_ENGINE_PASSTHRU:
        decode_ccp_passthrough(s, id, desc);
        break;
    case CCP_ENGINE_ZLIB_DECOMPRESS:
        ccp_zlib(s, id, desc);
        break;
    case CCP_ENGINE_ECC:
        PSP_TODO("ccp.engine.ecc", "descriptor fails, status ERROR",
                 "CCP Error: unimplemented engine (ECC)");
        return true;
    default:
        PSP_TODO("ccp.engine.unknown",
                 "descriptor skipped, reported as successful",
                 "CCP Error: Unknown engine! val 0x%x", CCP5_CMD_ENGINE(desc));
        break;
    }
    return false;
}

static void ccp_process_q(CcpV5State* s, uint32_t id)
{

    CcpV5QState* qs;
    ccp5_desc* desc;
    hwaddr req_len;
    uint32_t tail;
    uint32_t head;
    uint32_t desc_count = 0;
    bool failed = false;
    /* TODO: This operation needs to be delayed! */

    qs = &s->q_states[id];
    trace_ccp_process_queue(qs->ccp_q_id, qs->ccp_q_tail, qs->ccp_q_head);

    /* Clear HALT bit */
    /* Clear RUN bit. TODO: The secure OS never re-sets the RUN bit. We need
     * to figure out how/when we really clear this bit */
    qs->ccp_q_control &= ~((CCP_Q_CTRL_HALT | CCP_Q_RUN));

    /* TODO fix tail/head data types? */
    tail = qs->ccp_q_tail;
    head = qs->ccp_q_head;

    while (tail < head) {
        /* Head and tail are guest-controlled, bound the work per call */
        if (desc_count++ >= CCP_Q_MAX_DESC_PER_RUN) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "CCP Error: queue %u exceeds %d descriptors\n", id,
                          CCP_Q_MAX_DESC_PER_RUN);
            failed = true;
            break;
        }

        /* The mapping may be shortened by the memory layer, so reset it */
        req_len = sizeof(ccp5_desc);
        desc = ccp_map(tail, &req_len, false);
        if (!desc || req_len < sizeof(ccp5_desc)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "CCP Error: cannot map descriptor at 0x%x\n", tail);
            if (desc) {
                cpu_physical_memory_unmap(desc, req_len, false, 0);
            }
            failed = true;
            break;
        }
        failed |= ccp_execute(s, id, desc);
        /* TODO: What is "access_len" ? */
        cpu_physical_memory_unmap(desc, req_len, false, sizeof(ccp5_desc));
        tail += sizeof(ccp5_desc);
    }

    /* TODO: Return proper CCP error codes */
    qs->ccp_q_tail = qs->ccp_q_head;
    qs->ccp_q_status = failed ? CCP_Q_STATUS_ERROR : CCP_Q_STATUS_SUCCESS;
    qs->ccp_q_control |= CCP_Q_CTRL_HALT;

    s->int_status |= CCP_CFG_INT_STATUS_DONE;
    qemu_set_irq(s->irq, 1);
}

static void ccp_queue_write(CcpV5State* s, hwaddr offset, uint32_t val,
                            uint32_t id)
{
    CcpV5QState* qs = &s->q_states[id];
    switch (offset) {
    case CCP_Q_CTRL_OFFSET:
        trace_ccp_queue_ctrl_write(id, val);
        qs->ccp_q_control = val;
        /*TODO: Is this the correct place to start processing the queue? Or
         * should we only start the queue on head/tail writes? */
        // if (qs->ccp_q_control & CCP_Q_RUN) {
        //     /* Process the request in 100ns from now */
        //     if (s->dma_timer_qid == -1) {
        //         s->dma_timer_qid = id;
        //         timer_mod(&s->dma_timer,
        //         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100);
        //     } else {
        //         qemu_log_mask(LOG_GUEST_ERROR, "CCP Error: Can't program
        //         timer. A CCP request is already pending.\n");
        //     }
        // }
        break;
    case CCP_Q_TAIL_LO_OFFSET:
        qs->ccp_q_tail = val;
        trace_ccp_queue_tail_lo_write(id, val);
        break;
    case CCP_Q_HEAD_LO_OFFSET:
        qs->ccp_q_head = val;
        trace_ccp_queue_head_lo_write(id, val);
        break;
    case CCP_Q_STATUS_OFFSET:
        qs->ccp_q_status = val;
        trace_ccp_queue_status_write(id, val);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "CCP Error: CCP queue write at unknown "
                      "offset: 0x%" HWADDR_PRIx " val 0x%x\n",
                      offset, val);
    }
    return;
}

/*
 * Random number output at control offset 0x0c. Inferred from firmware
 * behaviour, not hardware-verified: the firmware reads it as a random word
 * and fails when it reads 0 twice in a row. Modelled as a deterministic
 * splitmix64 stream seeded from trng_seed, so runs are reproducible. Each
 * draw returns the high half of the next output, skipping words that are 0
 * or equal to the previous one.
 */
static uint32_t ccp_trng_next(CcpV5State* s)
{
    uint32_t word;

    do {
        uint64_t z = (s->trng_state += 0x9e3779b97f4a7c15ULL);

        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        word = (z ^ (z >> 31)) >> 32;
    } while (word == 0 || word == s->trng_last);
    s->trng_last = word;
    trace_ccp_trng_draw(word);
    return word;
}

static uint32_t ccp_ctrl_read(CcpV5State* s, hwaddr offset)
{
    (void)s;

    /* TODO: handle or log global register access */
    switch (offset) {
    case CCP_G_Q_MASK_OFFSET:
        /* Ignored for now */
        break;
    case CCP_G_Q_PRIO_OFFSET:
        /* Ignored for now */
        break;
    default:
        break;
    }

    trace_ccp_queue_ctrl_read(offset, 0);
    return 0;
}

static void ccp_ctrl_write(CcpV5State* s, hwaddr offset, uint32_t value)
{
    (void)s;
    /* TODO: handle or log global register access */
    switch (offset) {
    case CCP_G_Q_MASK_OFFSET:
        /* Ignored for now */
        break;
    case CCP_G_Q_PRIO_OFFSET:
        /* Ignored for now */
        break;
    default:
        break;
    }

    trace_ccp_ctrl_write(offset, value);
    return;
}

static uint32_t ccp_config_read(CcpV5State* s, hwaddr offset)
{
    (void)s;

    uint32_t val = 0;
    switch (offset) {
    case 0x28:
        /* size of zlib decompressed data? */
        val = s->zlib_bytes_written;
        break;
    case CCP_CFG_INT_STATUS_OFFSET:
        val = s->int_status;
        break;
    case 0x38:
        /* The on chip bootloader waits for bit 0 to go 1. */
        val = 1;
        break;
    }

    trace_ccp_config_read(offset, val);
    return val;
}

static void ccp_config_write(CcpV5State* s, hwaddr offset, uint32_t value)
{
    trace_ccp_config_write(offset, value);
    if (offset == CCP_CFG_INT_STATUS_OFFSET) {
        s->int_status &= ~value;
    }
}

static uint64_t ccp_read(void* opaque, hwaddr offset, unsigned int size)
{
    CcpV5State* s = CCP_V5(opaque);
    uint32_t ret = 0;
    uint32_t id = 0;

    /*
     * A 32-bit read draws the next random word; a narrow read returns bytes
     * of the last word drawn (0 before the first draw) without drawing.
     */
    if (offset >= CCP_G_TRNG_OFFSET && offset < CCP_G_TRNG_OFFSET + 4) {
        if (size == sizeof(uint32_t)) {
            return ccp_trng_next(s);
        }
        return extract32(s->trng_last, (offset & 3) * 8, size * 8);
    }
    if (size != sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "CCP Error: Unsupported read size: "
                      "0x%x\n",
                      size);
        return 0;
    }
    if (offset < CCP_Q_OFFSET) {
        ret = ccp_ctrl_read(s, offset);
    } else if (offset < CCP_CONFIG_OFFSET) {
        /* CCP queue access */
        id = (offset >> 12) - 1; // Make queue index start with 0
        if (id >= CCP_Q_COUNT) {
            qemu_log_mask(LOG_GUEST_ERROR, "CCP Error: Invalid queue id 0x%d\n",
                          id);
            return ret;
        }
        ret = ccp_queue_read(s, offset & 0xfff, id);
    } else {
        ret = ccp_config_read(s, offset & 0xfff);
    }

    return ret;
}

static void ccp_write(void* opaque, hwaddr offset, uint64_t value,
                      unsigned int size)
{
    CcpV5State* s = CCP_V5(opaque);
    uint32_t id = 0;

    if (size != sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "CCP Error: Unsupported write size:"
                      "0x%x\n",
                      size);
        return;
    }

    if (offset < CCP_Q_OFFSET) {
        /* TODO: Explicit cast? */
        ccp_ctrl_write(s, offset, value);
    } else if (offset < CCP_CONFIG_OFFSET) {
        /* CCP queue access */
        id = (offset >> 12) - 1; // Make queue index start with 0
        if (id >= CCP_Q_COUNT) {
            qemu_log_mask(LOG_GUEST_ERROR, "CCP Error: Invalid queue id 0x%d\n",
                          id);
            return;
        }
        ccp_queue_write(s, offset & 0xfff, value, id);
    } else {
        ccp_config_write(s, offset & 0xfff, value);
    }
}

static void ccp_init_q(CcpV5State* s)
{
    int i;

    for (i = 0; i < CCP_Q_COUNT; i++) {
        memset(&s->q_states[i], 0, sizeof(CcpV5QState));
        s->q_states[i].ccp_q_control = CCP_Q_CTRL_HALT;
        s->q_states[i].ccp_q_status = CCP_Q_STATUS_SUCCESS;
        s->q_states[i].ccp_q_id = i;
    }
}

static const MemoryRegionOps ccp_mem_ops = {
    .read = ccp_read,
    .write = ccp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* TODO: Does the CCP really allow non-word aligned accesses ? */
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void ccp_init(Object* obj)
{
    CcpV5State* s = CCP_V5(obj);
    SysBusDevice* sbd = SYS_BUS_DEVICE(obj);

    // timer_init_ns(&s->dma_timer, QEMU_CLOCK_VIRTUAL, ccp_timer_cb, s);
    // s->dma_timer_qid = -1;

    memory_region_init_io(&s->iomem, obj, &ccp_mem_ops, s, TYPE_CCP_V5,
                          CCP_MMIO_SIZE);

    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    ccp_init_q(s);
    s->sha_ctx.raw = NULL;
    s->zlib_bytes_written = 0;
}

static const VMStateDescription ccp_q_vmstate = {
    .name = "amd.ccpV5/q_state",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ccp_q_control, CcpV5QState),
        VMSTATE_UINT32(ccp_q_tail, CcpV5QState),
        VMSTATE_UINT32(ccp_q_head, CcpV5QState),
        VMSTATE_UINT32(ccp_q_status, CcpV5QState),
        VMSTATE_UINT32(ccp_q_id, CcpV5QState),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Drop any in-flight hash or inflate stream. Neither is migrated, so the
 * stream state is only meaningful within the instance that started it.
 */
static void ccp_drop_stream_state(CcpV5State *s)
{
    ccp_clear_sha_ctx(&s->sha_ctx);
    /* Ends the inflate state if one is live; a no-op on a zeroed stream. */
    ccp_zlib_end(&s->zlib_state);
    memset(&s->zlib_state, 0, sizeof(s->zlib_state));
}

static int ccp_post_load(void *opaque, int version_id)
{
    /*
     * Snapshots taken mid-hash or mid-stream are not supported: the restored
     * device starts with no in-flight SHA or zlib context.
     */
    ccp_drop_stream_state(opaque);
    return 0;
}

/*
 * Not migrated: sha_ctx (raw pointer to a lazily allocated nettle context),
 * zlib_state (z_stream with internal pointers) and dma_timer/dma_timer_qid
 * (QEMUTimer, currently unused). They are dropped in ccp_post_load instead.
 * Version 2 adds the random number stream; a version 1 stream loads with the
 * stream as reset left it, and a version 2 stream is refused by older builds.
 */
static const VMStateDescription ccp_vmstate = {
    .name = TYPE_CCP_V5,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = ccp_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(int_status, CcpV5State),
        VMSTATE_STRUCT_ARRAY(q_states, CcpV5State, CCP_Q_COUNT, 1,
                             ccp_q_vmstate, CcpV5QState),
        VMSTATE_UINT8_ARRAY(lsb.u.lsb, CcpV5State,
                            CCP_LSB_SIZE * CCP_LSB_COUNT),
        VMSTATE_UINT32(zlib_bytes_written, CcpV5State),
        VMSTATE_UINT64_V(trng_state, CcpV5State, 2),
        VMSTATE_UINT32_V(trng_last, CcpV5State, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void ccp_reset_hold(Object *obj, ResetType type)
{
    CcpV5State *s = CCP_V5(obj);

    ccp_drop_stream_state(s);
    s->zlib_bytes_written = 0;
    s->trng_state = s->trng_seed;
    s->trng_last = 0;
}

static const Property ccp_properties[] = {
    /* Arbitrary fixed default, so the stream is the same on every run */
    DEFINE_PROP_UINT64("trng_seed", CcpV5State, trng_seed,
                       0x5053505f54524e47ULL),
};

static void ccp_class_init(ObjectClass* oc, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->vmsd = &ccp_vmstate;
    rc->phases.hold = ccp_reset_hold;
    device_class_set_props(dc, ccp_properties);
}

static const TypeInfo ccp_info = {
    .name = TYPE_CCP_V5,
    .parent = TYPE_SYS_BUS_DEVICE,
    .class_init = ccp_class_init,
    .instance_init = ccp_init,
    .instance_size = sizeof(CcpV5State),

};

static void ccp_register_types(void) { type_register_static(&ccp_info); }

type_init(ccp_register_types);
