
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "LZMA_decoder.h"

#define CONFIG_MAX_SIZE         (1 << 20)
#define CONFIG_IN_BUF_SIZE      (1 << 12)
#define CONFIG_OUT_BUF_SIZE     (1 << 12)

#define LZMA_OK                     0
#define LZMA_ERROR_DATA             1
#define LZMA_ERROR_MEM              2
#define LZMA_ERROR_CRC              3
#define LZMA_ERROR_UNSUPPORTED      4
#define LZMA_ERROR_PARAM            5
#define LZMA_ERROR_INPUT_EOF        6
#define LZMA_ERROR_OUTPUT_EOF       7
#define LZMA_ERROR_READ             8
#define LZMA_ERROR_WRITE            9
#define LZMA_ERROR_PROGRESS         10
#define LZMA_ERROR_FAIL             11
#define LZMA_ERROR_THREAD           12
#define LZMA_ERROR_NO_OPERATOR      13
#define LZMA_ERROR_ARCHIVE          16
#define LZMA_ERROR_NO_ARCHIVE       17

typedef int LZMA_ret;

/**
 *  lzma resource operator
 */
typedef struct lzma_opr
{
    ISzAlloc    mem_opr;

    int     (*Read)(void *dest, void *src, int length, struct lzma_opr *pLzma_opr);
    int     (*Write)(void *dest, void *src, int length, struct lzma_opr *pLzma_opr);

    int     (*Report_data_size)(uint32_t unpack_size, struct lzma_opr *pLzma_opr);

    void    *pExtra;

} lzma_opr_t;


#define MIN(a,b)                ((a)<(b)?(a):(b))
#define log_msg(str, ...)       printf("[%s:%d] " str, __func__, __LINE__, ## __VA_ARGS__)

static SRes
_Decode2Ram(CLzmaDec *pHLzma_dec, uint8_t *des, uint8_t *src, uint32_t unpack_size, lzma_opr_t *pLzma_opr)
{
    SRes    res = 0;

    do {
        int         has_remained = (unpack_size != (uint32_t)-1);
        ISzAlloc    *pHAlloc = 0;
        uint8_t     *inBuf = 0;
        uint8_t     *outBuf = 0;
        size_t      inPos = 0, inSize = 0, outPos = 0;

        uint8_t     *pCur_rd = src;
        uint8_t     *pCur_wr = des;

        pHAlloc = &pLzma_opr->mem_opr;

        if( !(inBuf = (uint8_t*)pHAlloc->Alloc(pHAlloc, CONFIG_IN_BUF_SIZE)) )
        {
            log_msg("inbuf malloc failed !\n");
            res =  LZMA_ERROR_MEM;
            break;
        }

        if( !(outBuf = (uint8_t*)pHAlloc->Alloc(pHAlloc, CONFIG_OUT_BUF_SIZE)) )
        {
            log_msg("outbuf malloc failed !\n");
            res = LZMA_ERROR_MEM;
            break;
        }

        LzmaDec_Init(pHLzma_dec);

        while(1)
        {
            ELzmaStatus         status;
            SizeT               inProcessed;
            SizeT               outProcessed;
            ELzmaFinishMode     finishMode = LZMA_FINISH_ANY;

            if( inPos == inSize )
            {
                inSize = CONFIG_IN_BUF_SIZE;
                log_msg("LZMA_read_data, addr = %p \n", pCur_rd);

                // partial decompressing
                if( pLzma_opr->Read )
                    pLzma_opr->Read(inBuf, pCur_rd, inSize, pLzma_opr);

                pCur_rd += inSize;
                inPos = 0;
            }

            inProcessed  = inSize - inPos;
            outProcessed = CONFIG_OUT_BUF_SIZE - outPos;
            if( has_remained && outProcessed > unpack_size )
            {
                outProcessed = (SizeT)unpack_size;
                finishMode = LZMA_FINISH_END;
            }

            //-----------------------------------
            // start procedure
            res = LzmaDec_DecodeToBuf(pHLzma_dec,
                                      outBuf + outPos,
                                      &outProcessed,
                                      inBuf + inPos,
                                      &inProcessed,
                                      finishMode,
                                      &status);

            inPos       += inProcessed;
            outPos      += outProcessed;
            unpack_size -= outProcessed;

            // partial move to outside
            if( pLzma_opr->Write )
                pLzma_opr->Write(pCur_wr, outBuf, outPos, pLzma_opr);

            pCur_wr += outPos;
            outPos = 0;

            //-----------------------------------
            // decompress fail
            if( res != SZ_OK || (has_remained && unpack_size == 0) )
            {
                pHAlloc->Free(pHAlloc, inBuf);
                pHAlloc->Free(pHAlloc, outBuf);
                break;;
            }

            if( inProcessed == 0 && outProcessed == 0 )
            {
                pHAlloc->Free(pHAlloc, inBuf);
                pHAlloc->Free(pHAlloc, outBuf);

                if( has_remained || status != LZMA_STATUS_FINISHED_WITH_MARK )
                    res = SZ_ERROR_DATA;

                break;
            }

            //-----------------------------------
            // finish
            if( unpack_size == 0 )
            {
                pHAlloc->Free(pHAlloc, inBuf);
                pHAlloc->Free(pHAlloc, outBuf);
                res = LZMA_OK;
                break;
            }
        }
    } while(0);

    return res;
}


/**
 *  @brief  lzma_decode2flash()
 *
 *  @param [in] destination         - the destination address
 *  @param [in] reserved_size       - the reserved size for decompressed data, set '-1' to ignore output size and destination address
 *  @param [in] source              - the source address on flash of compressed input data
 *  @param [in] pLzma_opr           - the operator for LZMA
 *  @return
 *      SZ_OK                       - OK
 *      SZ_ERROR_DATA               - Data error
 *      SZ_ERROR_MEM                - Memory allocation arror
 *      SZ_ERROR_UNSUPPORTED        - Unsupported properties
 *      SZ_ERROR_INPUT_EOF          - it needs more bytes in input buffer (src)*
 *
 */
static LZMA_ret
lzma_decode2flash(
    uint8_t         *destination,
    uint32_t        reserved_size,
    const uint8_t   *source,
    lzma_opr_t      *pLzma_opr)
{
    SRes        res = 0;
    do {
        uint64_t    unpack_size = 0l;
        uint8_t     lzma_header[LZMA_PROPS_SIZE + 8];
        CLzmaDec    hLzma_dec;

        if( !pLzma_opr->Read || !pLzma_opr->Write ||
            !pLzma_opr->mem_opr.Alloc ||
            !pLzma_opr->mem_opr.Free )
        {
            res = LZMA_ERROR_NO_OPERATOR;
            break;
        }

        // TODO: check alignment of destination
        if( pLzma_opr->Read )
            pLzma_opr->Read((void*)lzma_header, (void*)source, sizeof(lzma_header), pLzma_opr);

        unpack_size = 0l;
        for(int i = 0; i < 8; i++)
            unpack_size += (uint64_t)lzma_header[LZMA_PROPS_SIZE + i] << (i * 8);

        if( pLzma_opr->Report_data_size )
        {
            int     rval = 0;
            rval = pLzma_opr->Report_data_size(unpack_size, pLzma_opr);
            if( rval ) break;
        }

        if( (uint32_t)unpack_size != (-1) && unpack_size > reserved_size )
        {
            log_msg("decompressed size over reserved size !!!\n");
            res = LZMA_ERROR_MEM;
            break;
        }

        LzmaDec_Construct(&hLzma_dec);
        LzmaDec_Allocate(&hLzma_dec, lzma_header, LZMA_PROPS_SIZE, &pLzma_opr->mem_opr);

        res = _Decode2Ram(&hLzma_dec, destination, (uint8_t*)source + sizeof(lzma_header), unpack_size, pLzma_opr);

        LzmaDec_Free(&hLzma_dec, &pLzma_opr->mem_opr);
    } while(0);

    return res;
}


static void
*_alloc(void *p, size_t size)
{
    uint8_t     *pBuf = 0;
    pBuf = malloc(size);
    log_msg("alloc (%p, %d)\n", pBuf, size);
    return pBuf;
}

static void
_free(void *p, void *address)
{
    log_msg("free (%p)\n", address);
    return;
}

static int
_read(void *dest, void *src, int length, lzma_opr_t *pLzma_opr)
{
    int     act_len = 0;
    memcpy(dest, src, length);
    return act_len;
}

static int
_write(void *dest, void *src, int length, lzma_opr_t *pLzma_opr)
{
    FILE    *fout = *((FILE**)pLzma_opr->pExtra);
    int     act_len = 0;

    act_len = fwrite(src, 1, length, fout);
    return act_len;
}

static int
_report_data_size(uint32_t unpack_size, lzma_opr_t *pLzma_opr)
{
    log_msg("the output size = %d\n", (uint32_t)unpack_size);
    return 0;
}

static void
_usage(char *pProg)
{
    log_msg("usage: %s [lzma file] [decompressed file path]\n", pProg);
    system("pause");
    exit(1);
    return;
}


int main(int argc, char **argv)
{
    FILE    *fin = 0, *fout = 0;
    uint8_t *pIn_buf = 0;
    do {
        size_t      in_len = 0;

        if( argc != 3 )
        {
            _usage(argv[0]);
            break;
        }

        if( !(fin = fopen(argv[1], "rb")) )
        {
            log_msg("open %s fail\n", argv[1]);
            break;
        }

        fseek(fin, 0l, SEEK_END);
        in_len = ftell(fin);
        fseek(fin, 0l, SEEK_SET);

        if( !(fout = fopen(argv[2], "wb")) )
        {
            log_msg("open %s fail\n", argv[2]);
            break;
        }

        if( !(pIn_buf = malloc(in_len)) )
        {
            log_msg("malloc %d fail \n", in_len);
            break;
        }

        fread(pIn_buf, 1, in_len, fin);
        fclose(fin);
        fin = 0;

        //---------------------------
        {   // action
            int             ret = 0;
            lzma_opr_t      hLzma_opr =
            {
                .mem_opr            = { _alloc, _free },
                .Read               = _read,
                .Write              = _write,
                .Report_data_size   = _report_data_size,
            };

            hLzma_opr.pExtra = (void*)&fout;

            ret = lzma_decode2flash(0, -1, (const uint8_t*)pIn_buf, &hLzma_opr);
            if( ret != LZMA_OK)
            {
                log_msg("lzma decompress status = %d \n", ret);
                break;
            }
        }

    } while(0);

    if( pIn_buf )   free(pIn_buf);
    if( fout )      fclose(fout);
    if( fin )       fclose(fin);

    system("pause");
    return 0;
}
