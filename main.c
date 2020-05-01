
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
#define LZMA_ERROR_ARCHIVE          16
#define LZMA_ERROR_NO_ARCHIVE       17

typedef int LZMA_ret;


typedef struct
{
    void *(*Alloc)(void *p, size_t size);
    void (*Free)(void *p, void *address);   /* address can be 0 */
} lzma_alloc_t;


#define MIN(a,b)                ((a)<(b)?(a):(b))
#define log_msg(str, ...)       printf("[%s:%d] " str, __func__, __LINE__, ## __VA_ARGS__)

static uint8_t          g_out_buf[CONFIG_MAX_SIZE] = {0};
static uint64_t         g_unpack_size = 0;
static FILE             *g_fout = 0;

static SRes
_Decode2Ram(CLzmaDec *pHLzma_dec, uint8_t *des, uint8_t *src, uint32_t unpack_size, ISzAlloc *alloc)
{
    SRes    res = 0;

    do {
        int         has_remained = (unpack_size != (uint32_t)-1);
        uint8_t     *inBuf = 0;
        uint8_t     *outBuf = 0;
        size_t      inPos = 0, inSize = 0, outPos = 0;

        uint8_t     *pCur_rd = src;
        uint8_t     *pCur_wr = des;

        if( !(inBuf = (uint8_t*)alloc->Alloc(alloc, CONFIG_IN_BUF_SIZE)) )
        {
            log_msg("inbuf malloc failed !\n");
            res =  LZMA_ERROR_MEM;
            break;
        }

        if( !(outBuf = (uint8_t*)alloc->Alloc(alloc, CONFIG_OUT_BUF_SIZE)) )
        {
            log_msg("outbuf malloc failed !\n");
            res = LZMA_ERROR_MEM;
            break;
        }

//        if( !g_fout )
//        {
//            g_fout = fopen("tmp_out.bin", "wb");
//        }

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
                memcpy(inBuf, pCur_rd, inSize);

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
//            if( g_fout )
//                fwrite(outBuf, 1, outPos, g_fout);

            memcpy(pCur_wr, outBuf, outPos);
            pCur_wr += outPos;
            outPos = 0;

            //-----------------------------------
            // decompress fail
            if( res != SZ_OK || (has_remained && unpack_size == 0) )
            {
                alloc->Free(alloc, inBuf);
                alloc->Free(alloc, outBuf);
                break;;
            }

            if( inProcessed == 0 && outProcessed == 0 )
            {
                alloc->Free(alloc, inBuf);
                alloc->Free(alloc, outBuf);

                if( has_remained || status != LZMA_STATUS_FINISHED_WITH_MARK )
                    res = SZ_ERROR_DATA;

//                if( g_fout )
//                    fclose(g_fout);
                break;
            }

            //-----------------------------------
            // finish
            if( unpack_size == 0 )
            {
                alloc->Free(alloc, inBuf);
                alloc->Free(alloc, outBuf);
                res = LZMA_OK;
                break;
            }
        }
    } while(0);

    return res;
}

/*
    lzma_decode to flash, use for decode from flash to flash
    --------------
    In:

        destination                 - the destination address on flash for output data
        reserved_size               - the reserved size for decompressed data
        source                      - the source address on flash of compressed input data
        lzma_alloc                  - the allocator for memory allocate and free

    Returns:
        SZ_OK                       - OK
        SZ_ERROR_DATA               - Data error
        SZ_ERROR_MEM                - Memory allocation arror
        SZ_ERROR_UNSUPPORTED        - Unsupported properties
        SZ_ERROR_INPUT_EOF          - it needs more bytes in input buffer (src)
*/
static LZMA_ret
lzma_decode2flash(
    uint8_t         *destination,
    uint32_t        reserved_size,
    const uint8_t   *source,
    lzma_alloc_t    *lzma_alloc)
{
    SRes        res = 0;
    do {
        uint8_t     lzma_header[LZMA_PROPS_SIZE + 8];
        ISzAlloc    *pHAlloc = (ISzAlloc*)lzma_alloc;
        CLzmaDec    hLzma_dec;

        // TODO: check alignment of destination

        memcpy(&lzma_header, source, sizeof(lzma_header));

        g_unpack_size = 0;
        for(int i = 0; i < 8; i++)
            g_unpack_size += (uint64_t)lzma_header[LZMA_PROPS_SIZE + i] << (i * 8);

        if( (uint32_t)g_unpack_size != (-1) && g_unpack_size > reserved_size )
        {
            log_msg("decompressed size over reserved size !!!\n");
            res = LZMA_ERROR_MEM;
            break;
        }

        LzmaDec_Construct(&hLzma_dec);
        LzmaDec_Allocate(&hLzma_dec, lzma_header, LZMA_PROPS_SIZE, pHAlloc);

        res = _Decode2Ram(&hLzma_dec, destination, (uint8_t*)source + sizeof(lzma_header), g_unpack_size, pHAlloc);

        LzmaDec_Free(&hLzma_dec, pHAlloc);
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
            lzma_alloc_t    hAlloc = { _alloc, _free };

            ret = lzma_decode2flash(g_out_buf, CONFIG_MAX_SIZE, (const uint8_t*)pIn_buf, &hAlloc);
            if( ret != LZMA_OK)
            {
                log_msg("lzma decompress status = %d \n", ret);
                break;
            }
        }

        //-----------------------
        // handle decompressed data
        if( !g_unpack_size )
        {
            log_msg("lzma decode fail \n");
            break;
        }

        log_msg("the output size = %d\n", g_unpack_size);
        fwrite(g_out_buf, 1, g_unpack_size, fout);

    } while(0);

    if( pIn_buf )   free(pIn_buf);
    if( fout )      fclose(fout);
    if( fin )       fclose(fin);

    system("pause");
    return 0;
}
